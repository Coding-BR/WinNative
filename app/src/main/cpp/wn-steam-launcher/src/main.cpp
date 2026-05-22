// wn-steam-launcher.exe — in-process Steam client host.
//
// Replaces the wn-steam-helper.exe Proton stub for Bionic Steam mode. Loads
// Valve's REAL Windows steamclient64.dll into THIS process, drives the
// CLIENTENGINE_INTERFACE_VERSION005 refresh-token logon, then launches the
// game via Valve's IClientAppManager::LaunchApp (CreateProcess fallback) so
// SteamStub-wrapped DRM exes self-decrypt. The game's steam_api64.dll then
// talks to a LOCAL in-process
// steamclient64.dll via Valve's normal IPC slot mechanism — no cross-process
// bridge to the Android side needed. All Steam features (matchmaking, P2P,
// SDR, friends, chat) flow through Valve's real client natively.
//
// Mirrors the IClientEngine/IClientUser dance we use in wn-steam-bootstrap on
// the Android side (steam_bootstrap.cpp:687-770). Same vtable offsets — Valve
// keeps the SDK ABI consistent across Windows and bionic-Linux builds for
// CLIENTENGINE_INTERFACE_VERSION005.
//
// Reads from env (set by XServerDisplayActivity before launching us):
//   WN_STEAM_TOKEN      Steam refresh token (JWT)
//   WN_STEAM_USERNAME   account name
//   WN_STEAM_STEAMID    64-bit Steam ID (decimal string)
//   WN_STEAM_APPID      game app id (decimal string, informational)
//
// Argv:
//   argv[1]             game .exe full Windows path (e.g.
//                       "C:\Program Files (x86)\Steam\steamapps\common\Forest\TheForest.exe")
//   argv[2..]           extra args passed through to the game
//
// Exit codes:
//   0   game ran to completion
//   1   missing argv / env (caller bug)
//   2   LoadLibrary steamclient64.dll failed
//   3   GetProcAddress CreateInterface failed
//   4   Steam_CreateGlobalUser failed (pipe+user setup)
//   5   CreateInterface(CLIENTENGINE_INTERFACE_VERSION005) returned null
//   6   GetIClientUser returned null
//   7   BLoggedOn never reached true (10s timeout)
//   8   CreateProcess game.exe failed

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <tlhelp32.h>

// IClientUser / IClientEngine vtable offsets — CONFIRMED by binary RE of
// Valve's steamclient64.dll (steam_client_0403) AND by decompiling GameHub
// SteamAgent's SteamCore::LoginToSteam (FUN_140055690).
//
// The Windows steamclient64.dll has NO `LogOnWithRefreshToken` method. The
// online refresh-token logon SteamAgent performs is exactly 3 IClientUser
// calls, in order:
//   1. SetLoginToken(token, accountName)         +0x1C0
//   2. GetSteamID(&out)  -> resolved CSteamID    +0x50
//   3. LogOn(thatCSteamID)                       +0x08
// SteamAgent does NOT call SetAccountNameForCachedCredentialLogin or
// SetLoginInformation on the online path — those belong to the offline-mode
// branch only. The old launcher mislabeled the offsets (from the bionic
// libsteamclient.so) and called SetAccountNameForCachedCredentialLogin,
// which the online CM logon never wants → EResult=15 AccessDenied.
static const int kVtEngine_GetIClientUser   = 0x40;  // IClientEngine slot 8
// IClientUser:
static const int kVtUser_LogOn              = 0x08;  // slot  1: EResult LogOn(uint64 steamID)
static const int kVtUser_BLoggedOn          = 0x20;  // slot  4: bool BLoggedOn()
static const int kVtUser_GetSteamID         = 0x50;  // slot 10: CSteamID& GetSteamID(CSteamID& out)
static const int kVtUser_BHasCachedCreds    = 0x188; // slot 49: bool BHasCachedCredentials(const char*)
static const int kVtUser_SetLoginToken      = 0x1C0; // slot 56: EResult SetLoginToken(const char* token, const char* account)

// IClientEngine slot 43 — GetIClientAppManager(hUser, hPipe). CONFIRMED by
// disassembling GameHub SteamAgent (FUN_140055180). IClientAppManager slot 2
// — LaunchApp — CONFIRMED two ways: GameHub's SteamCore::LaunchApplication
// call site (call [appmgr_vtable+0x10]) and the steamclient64.dll
// IClientAppManager proxy vtable (0 InstallApp, 1 UninstallApp, 2 LaunchApp,
// 3 ShutdownApp).
static const int kVtEngine_GetIClientAppManager = 0x158; // IClientEngine slot 43
static const int kVtEngine_GetIClientApps       = 0x88;  // IClientEngine slot 17
static const int kVtAppMgr_LaunchApp            = 0x10;  // IClientAppManager slot 2
// IClientApps slot 7 — bool RequestAppInfoUpdate(AppId_t* pAppIDs, int32 n).
// CONFIRMED from steamclient64.dll's IClientApps proxy thunk table.
static const int kVtApps_RequestAppInfoUpdate   = 0x38;  // IClientApps slot 7

// IClientEngine slot 24 — GetIClientRemoteStorage(hUser, hPipe), and the
// IClientRemoteStorage cloud-sync slots. CONFIRMED against steamclient64.dll
// build steam_client_0403 by RE of GameHub SteamAgent's cloud worker
// (FUN_14005bbd0): it acquires IClientRemoteStorage, calls BeginAppSync,
// busy-waits on IsAppSyncInProgress, then confirms via GetSyncState.
static const int kVtEngine_GetIClientRemoteStorage = 0xC0;  // IClientEngine slot 24
static const int kVtRS_GetSyncState         = 0x240; // int  GetSyncState(AppId_t) -> EGetFileSyncState
static const int kVtRS_BeginAppSync         = 0x270; // bool BeginAppSync(AppId_t, EAppSyncCommand, flags)
static const int kVtRS_IsAppSyncInProgress  = 0x278; // bool IsAppSyncInProgress(AppId_t)
static const int kVtRS_GetConflictInfo      = 0x258; // bool GetConflictInfo(AppId_t, int* outLocal, int* outRemote)
static const int kVtRS_ResolveConflict      = 0x268; // bool ResolveConflict(AppId_t, bool acceptLocal)

typedef void* (*CreateInterfaceFn)(const char* version, int* returnCode);
typedef int   (*Steam_CreateGlobalUser_fn)(int* pipe_out);
typedef bool  (*Steam_BLoggedOn_fn)(int pipe, int user);
typedef bool  (*Steam_BGetCallback_fn)(int pipe, void* cb);
typedef void  (*Steam_FreeLastCallback_fn)(int pipe);
typedef void  (*Breakpad_SteamSetAppID_fn)(unsigned app_id);

static void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n] = '\n';
    buf[n + 1] = '\0';
    // Wine routes stderr to the host log when launched under our XServer
    // GuestProgramLauncher; that's the channel logcat sees.
    fputs(buf, stderr);
    OutputDebugStringA(buf);
    // Also append to C:\wn-launcher.log so the logon trace is readable
    // off-device via `adb ... cat .../drive_c/wn-launcher.log` — the
    // launcher's stderr does NOT reliably reach wine_stderr.log.
    FILE* lf = fopen("C:\\wn-launcher.log", "a");
    if (lf) { fputs(buf, lf); fclose(lf); }
}

static uint64_t env_u64(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return 0;
    return (uint64_t) _strtoui64(v, NULL, 10);
}

// base64url alphabet value for one char, -1 if not a b64url digit.
static int b64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// Decode + log the JWT *payload* (middle segment) of the refresh token.
// The payload carries the account-metadata claims — aud (audience: must
// include "client" for a Steam-client logon), exp (expiry), iat, iss,
// sub (SteamID). Logging these makes an EResult=15 caused by an
// expired / wrong-audience token diagnosable in one read. The signature
// segment is NOT logged. Best-effort — silently skips on malformed input.
static void log_token_claims(const char* token) {
    if (!token || !*token) { log_line("[wn-launcher] token: (empty)"); return; }
    const char* dot1 = strchr(token, '.');
    if (!dot1) { log_line("[wn-launcher] token: not a JWT (no '.')"); return; }
    const char* dot2 = strchr(dot1 + 1, '.');
    if (!dot2) { log_line("[wn-launcher] token: not a JWT (one '.')"); return; }
    // Middle segment = base64url(payload).
    size_t seglen = (size_t)(dot2 - (dot1 + 1));
    if (seglen == 0 || seglen > 2000) {
        log_line("[wn-launcher] token: payload segment size unusable (%zu)", seglen);
        return;
    }
    char out[1536];
    size_t op = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < seglen && op < sizeof(out) - 1; ++i) {
        unsigned char c = (unsigned char) (dot1 + 1)[i];
        int v = b64url_val(c);
        if (v < 0) continue;  // skip '=' / stray
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[op++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[op] = '\0';
    log_line("[wn-launcher] token JWT payload: %s", out);
}

// Write HKCU\Software\Valve\Steam\ActiveProcess so the game's steam_api64.dll
// resolves SteamClientDll/SteamClientDll64 to our staged Valve binaries and
// believes Steam is "running".
static void seed_active_process_registry(uint32_t our_pid, uint32_t steam_account_id) {
    HKEY h = NULL;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER,
            "Software\\Valve\\Steam\\ActiveProcess",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, NULL);
    if (rc != ERROR_SUCCESS) {
        log_line("[wn-launcher] RegCreateKeyEx(ActiveProcess) failed rc=%ld", rc);
        return;
    }
    const char* clientDll   = "C:\\Program Files (x86)\\Steam\\steamclient.dll";
    const char* clientDll64 = "C:\\Program Files (x86)\\Steam\\steamclient64.dll";
    const char* installPath = "C:\\Program Files (x86)\\Steam";
    DWORD universe = 1;  // k_EUniversePublic
    DWORD pid_dw = (DWORD) our_pid;
    DWORD active_user = (DWORD) steam_account_id;
    RegSetValueExA(h, "SteamClientDll",   0, REG_SZ, (const BYTE*) clientDll,   (DWORD) strlen(clientDll)   + 1);
    RegSetValueExA(h, "SteamClientDll64", 0, REG_SZ, (const BYTE*) clientDll64, (DWORD) strlen(clientDll64) + 1);
    RegSetValueExA(h, "Universe",         0, REG_DWORD, (const BYTE*) &universe, sizeof(universe));
    RegSetValueExA(h, "pid",              0, REG_DWORD, (const BYTE*) &pid_dw,   sizeof(pid_dw));
    RegSetValueExA(h, "ActiveUser",       0, REG_DWORD, (const BYTE*) &active_user, sizeof(active_user));
    RegCloseKey(h);

    // Also write HKCU\Software\Valve\Steam\Apps\<appId>\Installed=1 for
    // certain games that consult it before launching.
    const char* appIdStr = getenv("WN_STEAM_APPID");
    if (appIdStr && *appIdStr) {
        char keyPath[256];
        snprintf(keyPath, sizeof(keyPath),
                 "Software\\Valve\\Steam\\Apps\\%s", appIdStr);
        HKEY h2 = NULL;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, keyPath, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h2, NULL) == ERROR_SUCCESS) {
            DWORD one = 1;
            DWORD zero = 0;
            RegSetValueExA(h2, "Installed", 0, REG_DWORD, (const BYTE*) &one,  sizeof(one));
            RegSetValueExA(h2, "Running",   0, REG_DWORD, (const BYTE*) &one,  sizeof(one));
            RegSetValueExA(h2, "Updating",  0, REG_DWORD, (const BYTE*) &zero, sizeof(zero));
            RegCloseKey(h2);
        }
    }
    // GameHub model: write the full Steam-install registry contract so the
    // game's GENUINE steam_api64.dll can locate the Steam dir and
    // SetDllDirectory() it before loading steamclient64.dll. Without
    // SteamPath set, steam_api64.dll loads steamclient64.dll without the
    // Steam dir on the DLL search path, and steamclient64.dll's tier0_s64 /
    // vstdlib_s64 imports fail to resolve (-> ntdll stubs -> page fault).
    // Real Steam writes SteamPath forward-slash lowercase, InstallPath
    // backslash. Mirrors what GameHub's SteamAgent writes.
    {
        const char* steamFwd  = "c:/program files (x86)/steam";
        const char* steamExe  = "c:/program files (x86)/steam/steam.exe";
        const char* steamBack = "C:\\Program Files (x86)\\Steam";
        HKEY hk = NULL;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, NULL,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hk, "SteamPath", 0, REG_SZ,
                           (const BYTE*) steamFwd, (DWORD) strlen(steamFwd) + 1);
            RegSetValueExA(hk, "SteamExe",  0, REG_SZ,
                           (const BYTE*) steamExe, (DWORD) strlen(steamExe) + 1);
            RegCloseKey(hk);
        }
        HKEY hm = NULL;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Valve\\Steam", 0, NULL,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hm, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hm, "InstallPath", 0, REG_SZ,
                           (const BYTE*) steamBack, (DWORD) strlen(steamBack) + 1);
            RegSetValueExA(hm, "SteamPath",   0, REG_SZ,
                           (const BYTE*) steamFwd,  (DWORD) strlen(steamFwd) + 1);
            RegCloseKey(hm);
        }
        // Some steam_api builds also consult the SteamPath env var.
        SetEnvironmentVariableA("SteamPath", steamBack);
    }

    log_line("[wn-launcher] HKCU ActiveProcess + Steam install registry seeded "
             "(pid=%u, activeUser=%u, SteamPath set)",
             our_pid, steam_account_id);
}

// Stage empty C:\Program Files (x86)\Steam\config\{config,local}.vdf.
// Valve's steamclient64.dll stat()s these on its CreateGlobalUser /
// CreateSteamPipe path and can stall/bail without an obvious error
// when the config dir doesn't exist on a fresh prefix. Empty stubs
// are enough for the stat to succeed; Valve repopulates them after a
// successful logon. Mirrors steam_bootstrap.cpp's stage_steam_config_dir.
static void stage_steam_config(void) {
    const char* cfgDir = "C:\\Program Files (x86)\\Steam\\config";
    CreateDirectoryA(cfgDir, NULL);  // no-op if it already exists
    const char* files[2] = {
        "C:\\Program Files (x86)\\Steam\\config\\config.vdf",
        "C:\\Program Files (x86)\\Steam\\config\\local.vdf",
    };
    for (int i = 0; i < 2; ++i) {
        DWORD attr = GetFileAttributesA(files[i]);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            HANDLE h = CreateFileA(files[i], GENERIC_WRITE, 0, NULL,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                log_line("[wn-launcher] staged empty %s", files[i]);
            }
        }
    }
}

// Write a minimal appmanifest_<appId>.acf so Valve's steamclient64.dll treats
// the (self-staged) game as a fully-installed Steam app — IClientAppManager::
// LaunchApp only launches *installed* apps. steamclient scans steamapps/*.acf
// during init, so this MUST run before steamclient64.dll loads. installdir is
// the path component right after "\steamapps\common\" in the game exe path;
// StateFlags 4 = fully installed (the load-bearing field, per RE of GameHub's
// SteamAgent — it ships no API installer, it relies on the on-disk ACF).
static void stage_app_manifest(uint32_t appId, const char* gameExe) {
    if (appId == 0 || !gameExe) return;
    const char* marker = "\\steamapps\\common\\";
    size_t mlen = strlen(marker);
    const char* hit = NULL;
    for (const char* s = gameExe; *s; ++s) {
        if (_strnicmp(s, marker, mlen) == 0) { hit = s; break; }
    }
    if (!hit) {
        log_line("[wn-launcher] app manifest: game not under steamapps\\common "
                 "— skipping (LaunchApp may report not-installed)");
        return;
    }
    const char* dirStart = hit + mlen;
    const char* dirEnd = strchr(dirStart, '\\');
    if (!dirEnd || dirEnd == dirStart) return;
    char installdir[260];
    size_t n = (size_t)(dirEnd - dirStart);
    if (n >= sizeof(installdir)) return;
    memcpy(installdir, dirStart, n);
    installdir[n] = '\0';

    CreateDirectoryA("C:\\Program Files (x86)\\Steam\\steamapps", NULL);
    char acf[MAX_PATH];
    snprintf(acf, sizeof(acf),
             "C:\\Program Files (x86)\\Steam\\steamapps\\appmanifest_%u.acf",
             appId);
    FILE* f = fopen(acf, "w");
    if (!f) {
        log_line("[wn-launcher] app manifest: fopen(%s) failed", acf);
        return;
    }
    const char* owner = getenv("WN_STEAM_STEAMID");
    fprintf(f,
            "\"AppState\"\n"
            "{\n"
            "\t\"appid\"\t\t\"%u\"\n"
            "\t\"Universe\"\t\t\"1\"\n"
            "\t\"name\"\t\t\"%s\"\n"
            "\t\"StateFlags\"\t\t\"4\"\n"
            "\t\"installdir\"\t\t\"%s\"\n"
            "\t\"LastUpdated\"\t\t\"0\"\n"
            "\t\"SizeOnDisk\"\t\t\"0\"\n"
            "\t\"buildid\"\t\t\"0\"\n"
            "\t\"LastOwner\"\t\t\"%s\"\n"
            "\t\"InstalledDepots\"\n"
            "\t{\n"
            "\t}\n"
            "}\n",
            appId, installdir, installdir,
            (owner && *owner) ? owner : "0");
    fclose(f);
    log_line("[wn-launcher] app manifest staged: %s (installdir=\"%s\", StateFlags=4)",
             acf, installdir);
}

// Count running processes whose image name == exeName (case-insensitive,
// basename). IClientAppManager::LaunchApp hands back no process handle, so
// the LaunchApp path watches the process table to know when the game exits.
static int count_process_by_name(const char* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    int count = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exeName) == 0) count++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

// True if `p` points into committed, executable memory. Used to sanity-check
// runtime-built vtable slots before calling through them — MinGW's GCC has no
// __try/__except, so this VirtualQuery probe is our guard against an offset
// shift in a future steamclient build (a wrong slot reads a data/null pointer;
// calling it would crash the launcher and abort the game launch).
static bool is_exec_ptr(void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD x = mbi.Protect & 0xFF;
    return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ ||
           x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
}

// Drive a Steam Cloud sync for `appId` through Valve's real steamclient64.dll
// and block until it completes. isDownload=true pulls cloud saves DOWN before
// launch; isDownload=false pushes local saves UP after the game exits.
//
// Mirrors GameHub SteamAgent's cloud worker (FUN_14005bbd0): acquire
// IClientRemoteStorage, BeginAppSync, busy-wait IsAppSyncInProgress draining
// callbacks, then confirm GetSyncState. We deliberately do NOT call
// ResolveConflict on an unresolved conflict — it discards one side and a wrong
// guess loses the user's saves; logging and proceeding is the safe default.
//
// Every runtime-built vtable slot is is_exec_ptr()-checked before use: a slot
// shift would otherwise crash the launcher before the game ever starts, so on a
// failed check we log and return and the game still launches (without sync).
static void cloud_sync(void* engine, int hUser, int pipe, uint32_t appId,
                       bool isDownload,
                       Steam_BGetCallback_fn bGetCallback,
                       Steam_FreeLastCallback_fn freeLastCallback) {
    const char* dir = isDownload ? "download" : "upload";
    if (!engine || appId == 0) return;

    // The app's Cloud Saves toggle gates launcher-side sync.
    const char* noCloud = getenv("WN_STEAM_NO_CLOUD");
    if (noCloud && *noCloud) {
        log_line("[wn-launcher] cloud %s: skipped — cloud saves disabled", dir);
        return;
    }

    void** engine_vt = *(void***) engine;
    void* getRSp = engine_vt[kVtEngine_GetIClientRemoteStorage / 8];
    if (!is_exec_ptr(getRSp)) {
        log_line("[wn-launcher] cloud %s: GetIClientRemoteStorage slot "
                 "(+0x%x) not executable (%p) — skipping sync",
                 dir, kVtEngine_GetIClientRemoteStorage, getRSp);
        return;
    }
    typedef void* (*GetIClientRemoteStorageFn)(void* self, int u, int p);
    void* rs = ((GetIClientRemoteStorageFn) getRSp)(engine, hUser, pipe);
    log_line("[wn-launcher] cloud %s: GetIClientRemoteStorage -> %p", dir, rs);
    if (!rs) return;

    void** rs_vt = *(void***) rs;
    void* stateP = rs_vt[kVtRS_GetSyncState / 8];
    void* beginP = rs_vt[kVtRS_BeginAppSync / 8];
    void* progP  = rs_vt[kVtRS_IsAppSyncInProgress / 8];
    log_line("[wn-launcher] cloud %s: RS vt GetSyncState=%p BeginAppSync=%p "
             "IsAppSyncInProgress=%p", dir, stateP, beginP, progP);
    if (!is_exec_ptr(stateP) || !is_exec_ptr(beginP) || !is_exec_ptr(progP)) {
        log_line("[wn-launcher] cloud %s: an IClientRemoteStorage slot is not "
                 "executable — offsets may not match this steamclient build; "
                 "skipping sync", dir);
        return;
    }
    typedef bool (*BeginAppSyncFn)(void* self, uint32_t app, int d, int f);
    typedef bool (*IsAppSyncInProgressFn)(void* self, uint32_t app);
    typedef int  (*GetSyncStateFn)(void* self, uint32_t app);
    BeginAppSyncFn        beginSync  = (BeginAppSyncFn) beginP;
    IsAppSyncInProgressFn inProgress = (IsAppSyncInProgressFn) progP;
    GetSyncStateFn        getState   = (GetSyncStateFn) stateP;

    // BeginAppSync(appId, EAppSyncCommand, flags) — CONFIRMED from GameHub
    // SteamAgent's cloud worker FUN_14005bbd0:
    //   launch DOWNLOAD = BeginAppSync(appId, 1, 0)  cmd bit0 "AutoCloud Launch"
    //   exit   UPLOAD   = BeginAppSync(appId, 2, 4)  cmd bit1 + flag bit2 "AC Exit"
    // The exit/upload path MUST carry a non-zero command mask + the AC-Exit
    // flag: with a zero command mask steamclient runs no sync job at all and
    // returns "synced" instantly (~90ms) without transferring anything — that
    // was the upload no-op bug.
    const int kCmd  = isDownload ? 1 : 2;
    const int kFlag = isDownload ? 0 : 4;

    // EGetFileSyncState — CONFIRMED from SteamAgent's GetSyncState name table:
    //   0 Disabled  1 Synchronized  2 InProgress  3 PendingDownload
    //   4 PendingUpload  5 PendingBoth  6 ConflictingChanges
    int state0 = getState(rs, appId);
    log_line("[wn-launcher] cloud %s: pre-sync state=%d", dir, state0);

    // SteamAgent re-runs the sync while it stays in a pending state. Do the
    // same: up to 3 attempts, stopping on Synchronized(1) or Conflict(6).
    const int kMaxWaitMs = 60000;
    int finalState = state0;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        bool started = beginSync(rs, appId, kCmd, kFlag);
        log_line("[wn-launcher] cloud %s: BeginAppSync(appId=%u, cmd=%d, "
                 "flag=%d) attempt %d -> %d",
                 dir, appId, kCmd, kFlag, attempt, started ? 1 : 0);
        if (!started) {
            log_line("[wn-launcher] cloud %s: BeginAppSync declined", dir);
            break;
        }
        int waited = 0;
        while (inProgress(rs, appId) && waited < kMaxWaitMs) {
            if (bGetCallback && freeLastCallback) {
                char cb[64];
                while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
            }
            Sleep(10);
            waited += 10;
        }
        finalState = getState(rs, appId);
        log_line("[wn-launcher] cloud %s: attempt %d finished in %dms, state=%d "
                 "(1=Synced 3=pendDL 4=pendUL 5=pendBoth 6=CONFLICT)",
                 dir, attempt, waited, finalState);
        if (finalState == 1 || finalState == 6) break;
        bool retry = isDownload ? (finalState == 3)
                                : (finalState == 4 || finalState == 5);
        if (!retry) break;
        Sleep(300);
    }

    if (finalState == 6) {
        // Conflict — ask the app which copy to keep via a request/response file.
        void* infoP    = rs_vt[kVtRS_GetConflictInfo / 8];
        void* resolveP = rs_vt[kVtRS_ResolveConflict / 8];
        if (!is_exec_ptr(infoP) || !is_exec_ptr(resolveP)) {
            log_line("[wn-launcher] cloud %s: conflict — resolve slots not "
                     "executable; leaving both copies intact", dir);
        } else {
            typedef bool (*GetConflictInfoFn)(void*, uint32_t, int*, int*);
            typedef bool (*ResolveConflictFn)(void*, uint32_t, bool);
            int localT = 0, remoteT = 0;
            ((GetConflictInfoFn) infoP)(rs, appId, &localT, &remoteT);
            log_line("[wn-launcher] cloud %s: CONFLICT — localTime=%d "
                     "remoteTime=%d; asking app", dir, localT, remoteT);
            remove("C:\\wn-cloud-conflict.resp");
            FILE* rq = fopen("C:\\wn-cloud-conflict.req", "w");
            if (rq) {
                fprintf(rq, "%u %d %d\n", appId, localT, remoteT);
                fclose(rq);
            }
            char choice[16] = {0};
            int cw = 0;
            while (cw < 600000) {
                if (bGetCallback && freeLastCallback) {
                    char cb[64];
                    while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                }
                FILE* rp = fopen("C:\\wn-cloud-conflict.resp", "r");
                if (rp) {
                    if (!fgets(choice, sizeof(choice), rp)) choice[0] = '\0';
                    fclose(rp);
                    break;
                }
                Sleep(200);
                cw += 200;
            }
            remove("C:\\wn-cloud-conflict.req");
            remove("C:\\wn-cloud-conflict.resp");
            if (choice[0] != 'l' && choice[0] != 'c') {
                log_line("[wn-launcher] cloud %s: no conflict choice from app; "
                         "leaving both copies intact", dir);
            } else {
                bool acceptLocal = (choice[0] == 'l');
                bool rr = ((ResolveConflictFn) resolveP)(rs, appId, acceptLocal);
                log_line("[wn-launcher] cloud %s: ResolveConflict(acceptLocal="
                         "%d) -> %d", dir, acceptLocal ? 1 : 0, rr ? 1 : 0);
                if (beginSync(rs, appId, kCmd, kFlag)) {
                    int w = 0;
                    while (inProgress(rs, appId) && w < kMaxWaitMs) {
                        if (bGetCallback && freeLastCallback) {
                            char cb[64];
                            while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                        }
                        Sleep(10);
                        w += 10;
                    }
                }
                finalState = getState(rs, appId);
            }
        }
    }

    if (finalState == 1) {
        log_line("[wn-launcher] cloud %s: COMPLETE (Synchronized)", dir);
    } else if (finalState == 6) {
        log_line("[wn-launcher] cloud %s: CONFLICT unresolved — both copies "
                 "left intact", dir);
    } else {
        log_line("[wn-launcher] cloud %s: did NOT fully sync (state=%d)",
                 dir, finalState);
    }
}

int main(int argc, char** argv) {
    setbuf(stderr, NULL);
    setbuf(stdout, NULL);
    // Truncate the log file at process start so each launch's trace is
    // self-contained.
    { FILE* lf = fopen("C:\\wn-launcher.log", "w"); if (lf) fclose(lf); }
    log_line("[wn-launcher] Steam Launcher in-process Steam launcher starting (pid=%lu)",
             (unsigned long) GetCurrentProcessId());

    if (argc < 2) {
        log_line("[wn-launcher] FATAL: missing game exe argv[1]");
        return 1;
    }
    const char* gameExe = argv[1];

    const char* token   = getenv("WN_STEAM_TOKEN");
    const char* user    = getenv("WN_STEAM_USERNAME");
    uint64_t    steamId = env_u64("WN_STEAM_STEAMID");
    uint32_t    appId   = (uint32_t) env_u64("WN_STEAM_APPID");
    bool        haveCreds = token && *token && user && *user && steamId != 0;
    log_line("[wn-launcher] game=\"%s\"", gameExe);
    log_line("[wn-launcher] creds: user=\"%s\" steamId=%llu tokenLen=%d appId=%u",
             user ? user : "(null)",
             (unsigned long long) steamId,
             token ? (int) strlen(token) : 0,
             appId);
    // Decode the refresh-token JWT claims so an EResult=15 caused by an
    // expired / wrong-audience token is visible in wn-launcher.log.
    log_token_claims(token);

    // Account ID (lower 32 bits of SteamID) — used for HKCU ActiveUser.
    uint32_t accId = (uint32_t)(steamId & 0xFFFFFFFFull);
    seed_active_process_registry(GetCurrentProcessId(), accId);

    // Stage config/{config,local}.vdf stubs before loading steamclient64.dll.
    stage_steam_config();

    // Stage appmanifest_<appId>.acf so steamclient sees the game as an
    // installed Steam app — required for IClientAppManager::LaunchApp.
    stage_app_manifest(appId, gameExe);

    // Log the TLS cert env so a logon timeout caused by a failed CM
    // TLS handshake (no trusted CA) is diagnosable. XServerDisplayActivity
    // sets STEAM_SSL_CERT_FILE in the Steam Launcher env block.
    {
        const char* sslCert = getenv("STEAM_SSL_CERT_FILE");
        log_line("[wn-launcher] STEAM_SSL_CERT_FILE=%s",
                 (sslCert && *sslCert) ? sslCert : "(unset)");
    }

    // ------------------------------------------------------------------
    // STEP 1: LoadLibrary Valve's real steamclient64.dll from the Steam
    // dir. WE USE THE FULL ABSOLUTE PATH so Wine's DllOverrides
    // (which redirect bare "steamclient64.dll" to the bionic bridge's
    // lsteamclient.dll in system32) don't kick in. The bridge's
    // Steam_CreateGlobalUser is an unimplemented stub; we need Valve's
    // real implementation.
    //
    // Pre-load tier0_s64.dll then vstdlib_s64.dll by full path FIRST.
    // steamclient64.dll imports ~50 V_*/Plat_*/CThread*/Spew* functions
    // from those two DLLs. Proton 9's Wine resolves the transitive
    // imports against steamclient64.dll's own directory; Proton 10's
    // loader does not, so they fall back to ntdll unimplemented stubs
    // (0x00A1xxxx) and the first call into one during process_attach
    // page-faults on execute access (LoadLibraryEx fails, GLE=998).
    // Loading the real modules up front puts them in the module list so
    // steamclient64.dll's imports bind by basename. tier0 must come
    // first: vstdlib_s64.dll itself imports tier0_s64.dll.
    // ------------------------------------------------------------------
    SetDllDirectoryA("C:\\Program Files (x86)\\Steam");
    {
        const char* deps[] = {
            "C:\\Program Files (x86)\\Steam\\tier0_s64.dll",
            "C:\\Program Files (x86)\\Steam\\vstdlib_s64.dll",
        };
        for (const char* dep : deps) {
            HMODULE dm = LoadLibraryExA(
                dep, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (dm) {
                log_line("[wn-launcher] preloaded %s at %p", dep, dm);
            } else {
                log_line("[wn-launcher] WARN preload %s FAILED, GLE=%lu",
                         dep, GetLastError());
            }
        }
    }
    const char* steamclientPath =
        "C:\\Program Files (x86)\\Steam\\steamclient64.dll";
    HMODULE lsc = LoadLibraryExA(
        steamclientPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lsc) {
        log_line("[wn-launcher] LoadLibraryEx(%s) FAILED, GLE=%lu",
                 steamclientPath, GetLastError());
        return 2;
    }
    log_line("[wn-launcher] steamclient64.dll loaded at %p (from %s)",
             lsc, steamclientPath);

    CreateInterfaceFn createInterface =
        (CreateInterfaceFn) GetProcAddress(lsc, "CreateInterface");
    Steam_CreateGlobalUser_fn createGlobalUser =
        (Steam_CreateGlobalUser_fn) GetProcAddress(lsc, "Steam_CreateGlobalUser");
    Steam_BLoggedOn_fn        bLoggedOn =
        (Steam_BLoggedOn_fn) GetProcAddress(lsc, "Steam_BLoggedOn");
    Steam_BGetCallback_fn     bGetCallback =
        (Steam_BGetCallback_fn) GetProcAddress(lsc, "Steam_BGetCallback");
    Steam_FreeLastCallback_fn freeLastCallback =
        (Steam_FreeLastCallback_fn) GetProcAddress(lsc, "Steam_FreeLastCallback");
    Breakpad_SteamSetAppID_fn breakpadSetAppId =
        (Breakpad_SteamSetAppID_fn) GetProcAddress(lsc, "Breakpad_SteamSetAppID");

    if (!createInterface) {
        log_line("[wn-launcher] dlsym CreateInterface FAILED");
        return 3;
    }
    log_line("[wn-launcher] exports: CreateInterface=%p Steam_CreateGlobalUser=%p "
             "Steam_BLoggedOn=%p Steam_BGetCallback=%p",
             createInterface, createGlobalUser, bLoggedOn, bGetCallback);

    if (breakpadSetAppId) {
        breakpadSetAppId(appId);
    }

    // ------------------------------------------------------------------
    // STEP 2: Create pipe + global user. Steam_CreateGlobalUser is the
    // Valve flat-C entry point that allocates a pipe AND connects a
    // global user in one call. (Same mechanism we use on the Android
    // side via Steam_CreateGlobalUser on libsteamclient.so.)
    // ------------------------------------------------------------------
    int pipe = 0;
    int hUser = 0;
    if (createGlobalUser) {
        hUser = createGlobalUser(&pipe);
    } else {
        // Fallback path via ISteamClient ABI if the flat-C export is
        // missing in this Steam build. Cheaper to add later if needed.
        log_line("[wn-launcher] Steam_CreateGlobalUser missing; falling back "
                 "to ISteamClient.CreateSteamPipe (not implemented yet)");
        return 4;
    }
    if (hUser == 0 || pipe == 0) {
        log_line("[wn-launcher] Steam_CreateGlobalUser failed pipe=%d user=%d",
                 pipe, hUser);
        return 4;
    }
    log_line("[wn-launcher] Steam_CreateGlobalUser OK pipe=%d user=%d",
             pipe, hUser);

    // ------------------------------------------------------------------
    // STEP 3: IClientEngine refresh-token logon. Same dance as
    // wn-steam-bootstrap on Android side, just here we call Valve's
    // Windows steamclient64.dll in our process.
    // ------------------------------------------------------------------
    void* engine = NULL;
    if (haveCreds) {
        int err = 0;
        engine = createInterface("CLIENTENGINE_INTERFACE_VERSION005", &err);
        if (!engine || err != 0) {
            log_line("[wn-launcher] CreateInterface(CLIENTENGINE_INTERFACE_VERSION005) "
                     "-> engine=%p err=%d", engine, err);
            // Don't abort yet — Steam may still be reachable via cached
            // session in config.vdf. Skip to BLoggedOn poll.
        } else {
            void** engine_vt = *(void***) engine;
            log_line("[wn-launcher] engine=%p vtable=%p", engine, engine_vt);
            // First-run vtable hex dump — Valve's Windows steamclient64.dll
            // is a different binary than the bionic libsteamclient.so, so a
            // hidden slot reordering would otherwise SIGSEGV silently. Dump
            // the first 16 engine slots so a 1-byte misalignment is a
            // 30-second logcat read instead of a debugging marathon.
            for (int i = 0; i < 16; ++i) {
                log_line("[wn-launcher] engine_vt[%2d] @ +0x%02x = %p",
                         i, i * 8, engine_vt[i]);
            }

            typedef void* (*GetIClientUserFn)(void* self, int hUser, int hPipe);
            GetIClientUserFn getIClientUser = (GetIClientUserFn)
                engine_vt[kVtEngine_GetIClientUser / 8];
            void* iuser = getIClientUser(engine, hUser, pipe);
            log_line("[wn-launcher] IClientEngine.GetIClientUser -> %p", iuser);

            if (iuser) {
                void** iuser_vt = *(void***) iuser;
                // Defensive dump of IClientUser slots 0..60 — covers the
                // logon methods we use: LogOn(1), BLoggedOn(4),
                // BHasCachedCredentials(49), SetAccountNameForCached-
                // CredentialLogin(50), SetLoginInformation(54),
                // SetLoginToken(56). A Valve DLL update that reorders the
                // vtable shows up here as a 30-second logcat read.
                for (int i = 0; i < 60; ++i) {
                    log_line("[wn-launcher] iuser_vt[%2d] @ +0x%02x = %p",
                             i, i * 8, iuser_vt[i]);
                }

                // ── Refresh-token logon — DECOMPILED from GameHub's ──
                // SteamAgent SteamCore::LoginToSteam (FUN_140055690),
                // online path. Exactly three IClientUser calls, in order:
                //   1. SetLoginToken(refreshToken, accountName)   +0x1C0
                //   2. GetSteamID(&out)  → resolved CSteamID       +0x50
                //   3. LogOn(thatCSteamID)                         +0x08
                // SteamAgent does NOT call SetAccountNameForCached-
                // CredentialLogin / SetLoginInformation here — those are
                // the OFFLINE-mode branch. The result arrives async via
                // the connection callbacks drained in STEP 4.

                // Diagnostic only — BHasCachedCredentials (slot 49).
                typedef bool (*BHasCachedCredsFn)(void* self, const char* user);
                BHasCachedCredsFn hasCached = (BHasCachedCredsFn)
                    iuser_vt[kVtUser_BHasCachedCreds / 8];
                log_line("[wn-launcher] BHasCachedCredentials(%s) = %d",
                         user, hasCached(iuser, user) ? 1 : 0);

                // STEP 3.1 — SetLoginToken(refreshToken, accountName).
                // Both args are C-strings (confirmed: SteamAgent passes
                // RDX=token, R8=accountName).
                typedef int (*SetLoginTokenFn)(void* self, const char* token,
                                               const char* account);
                SetLoginTokenFn setLoginToken = (SetLoginTokenFn)
                    iuser_vt[kVtUser_SetLoginToken / 8];
                int tokRc = setLoginToken(iuser, token, user);
                log_line("[wn-launcher] SetLoginToken(tokenLen=%d, account=%s) -> %d",
                         (int) strlen(token), user, tokRc);

                // STEP 3.2 — GetSteamID(&out): resolves the CSteamID now
                // that the token is set. SteamAgent feeds THIS value to
                // LogOn (not a caller-supplied SteamID). Valve's
                // CSteamID& GetSteamID(CSteamID&) fills the out-arg and
                // returns a pointer to it.
                typedef void* (*GetSteamIDFn)(void* self, void* outBuf);
                GetSteamIDFn getSteamID = (GetSteamIDFn)
                    iuser_vt[kVtUser_GetSteamID / 8];
                uint64_t outSid = 0;
                void* sidRet = getSteamID(iuser, &outSid);
                uint64_t logonSid = outSid;
                if (logonSid == 0 && sidRet) logonSid = *(uint64_t*) sidRet;
                if (logonSid == 0) {
                    logonSid = steamId;  // fall back to the env-supplied SteamID
                    log_line("[wn-launcher] GetSteamID returned 0 — falling back "
                             "to env steamId=%llu", (unsigned long long) steamId);
                } else {
                    log_line("[wn-launcher] GetSteamID -> %llu (env steamId=%llu)",
                             (unsigned long long) logonSid,
                             (unsigned long long) steamId);
                }

                // STEP 3.3 — LogOn(steamID). Returns EResult synchronously
                // (immediate reject visible here); the final result still
                // arrives via the SteamServersConnected/ConnectFailure
                // callbacks drained in STEP 4.
                typedef int (*LogOnFn)(void* self, uint64_t steamID);
                LogOnFn logOn = (LogOnFn) iuser_vt[kVtUser_LogOn / 8];
                int logonRc = logOn(iuser, logonSid);
                log_line("[wn-launcher] LogOn(%llu) -> EResult=%d "
                         "(1=OK 5=InvalidPassword 15=AccessDenied 16=Timeout 84=RateLimit)",
                         (unsigned long long) logonSid, logonRc);
                if (logonRc == 15) {
                    log_line("[wn-launcher] WARNING: LogOn returned AccessDenied "
                             "synchronously — credentials rejected pre-network");
                }
            }
        }
    } else {
        log_line("[wn-launcher] no creds — skipping refresh-token logon "
                 "(game may run in offline / no-auth mode)");
    }

    // ------------------------------------------------------------------
    // STEP 4: Poll BLoggedOn up to 20s while draining pending callbacks.
    //
    // Decodes the connection callbacks (SteamServersConnected_t=101,
    // SteamServerConnectFailure_t=102, SteamServersDisconnected_t=103)
    // so a logon failure is diagnosable from one log read:
    //   - 101 seen, BLoggedOn false  -> connected but auth pending/failed
    //   - 102 seen + EResult         -> CM connection failed (3/16 = TLS/
    //                                   network, 5/15 = bad token/auth)
    //   - nothing seen               -> client engine never connected
    // ------------------------------------------------------------------
    bool loggedOn = false;
    bool sawConnected = false, sawConnFail = false;
    int  connFailEResult = 0;
    int  polls = 0;
    if (bLoggedOn) {
        const int kMaxPolls = 200;  // 200 * 100ms = 20s
        char cbBuf[64] = {0};
        for (; polls < kMaxPolls; ++polls) {
            if (bGetCallback && freeLastCallback) {
                while (bGetCallback(pipe, cbBuf)) {
                    int cbId = *(int*)(cbBuf + 4);
                    // CallbackMsg_t: {hUser@0,iCallback@4,pubParam@8,cubParam@16}
                    void* param = *(void**)(cbBuf + 8);
                    if (cbId == 101) {
                        sawConnected = true;
                        log_line("[wn-launcher] callback 101 SteamServersConnected");
                    } else if (cbId == 102) {
                        sawConnFail = true;
                        int er = param ? *(int*)param : -1;
                        connFailEResult = er;
                        log_line("[wn-launcher] callback 102 SteamServerConnectFailure "
                                 "EResult=%d (3=NoConnection 5=InvalidPassword "
                                 "15=AccessDenied 16=Timeout 84=RateLimit)", er);
                    } else if (cbId == 103) {
                        int er = param ? *(int*)param : -1;
                        log_line("[wn-launcher] callback 103 SteamServersDisconnected "
                                 "EResult=%d", er);
                    } else {
                        log_line("[wn-launcher] callback id=%d drained", cbId);
                    }
                    freeLastCallback(pipe);
                }
            }
            if (bLoggedOn(pipe, hUser)) {
                loggedOn = true;
                log_line("[wn-launcher] Steam_BLoggedOn=true after %dx100ms",
                         polls + 1);
                break;
            }
            // A hard auth failure (bad/expired token, access denied,
            // rate-limited) never recovers by waiting — bail at once
            // instead of burning the remaining timeout.
            if (sawConnFail && (connFailEResult == 5 ||
                                connFailEResult == 15 ||
                                connFailEResult == 84)) {
                log_line("[wn-launcher] hard auth failure (EResult=%d) — "
                         "skipping remaining logon wait", connFailEResult);
                break;
            }
            Sleep(100);
        }
    }
    if (!loggedOn) {
        log_line("[wn-launcher] WARNING: Steam_BLoggedOn not true after %dx100ms "
                 "(sawConnected=%d sawConnFail=%d) — proceeding with game launch "
                 "anyway (game may end up in offline mode)",
                 polls, sawConnected ? 1 : 0, sawConnFail ? 1 : 0);
    }

    // ------------------------------------------------------------------
    // STEP 4.5: Refresh the game's app-info so steamclient has its launch
    // config (which exe to run) before LaunchApp. Mirrors GameHub's
    // SteamCore::RefreshApps. Without this, LaunchApp is accepted (returns
    // a valid HSteamAPICall) but steamclient has no launch config for the
    // app and never spawns the game.
    // ------------------------------------------------------------------
    if (loggedOn && engine && appId != 0) {
        void** engine_vt = *(void***) engine;
        typedef void* (*GetIClientAppsFn)(void* self, int hUser, int hPipe);
        GetIClientAppsFn getApps = (GetIClientAppsFn)
            engine_vt[kVtEngine_GetIClientApps / 8];
        void* iApps = getApps(engine, hUser, pipe);
        log_line("[wn-launcher] IClientEngine.GetIClientApps -> %p", iApps);
        if (iApps) {
            void** apps_vt = *(void***) iApps;
            typedef bool (*RequestAppInfoUpdateFn)(void* self,
                                                   uint32_t* appIds, int count);
            RequestAppInfoUpdateFn reqInfo = (RequestAppInfoUpdateFn)
                apps_vt[kVtApps_RequestAppInfoUpdate / 8];
            uint32_t appIds[1] = { appId };
            bool reqRc = reqInfo(iApps, appIds, 1);
            log_line("[wn-launcher] RequestAppInfoUpdate(appId=%u) -> %d",
                     appId, reqRc ? 1 : 0);
            // Wait for AppInfoUpdateComplete_t (callback 1003), up to 10s,
            // draining the pipe meanwhile.
            bool appInfoDone = false;
            for (int i = 0; i < 100 && !appInfoDone; ++i) {
                if (bGetCallback && freeLastCallback) {
                    char cb[64];
                    while (bGetCallback(pipe, cb)) {
                        if (*(int*)(cb + 4) == 1003) appInfoDone = true;
                        freeLastCallback(pipe);
                    }
                }
                if (!appInfoDone) Sleep(100);
            }
            log_line("[wn-launcher] AppInfoUpdateComplete_t %s",
                     appInfoDone ? "received" : "NOT received within 10s");
        }
    }

    // ------------------------------------------------------------------
    // STEP 4.6: Steam Cloud — pull saves DOWN before launch. Drives a
    // sync through Valve's real steamclient64.dll and blocks until it
    // finishes, exactly as GameHub's SteamAgent waits for the Steam Cloud
    // download before starting the game. Without this the game can start
    // before its cloud saves land locally and clobber them with a fresh
    // profile. Skipped when not logged on (no cloud session).
    // ------------------------------------------------------------------
    if (loggedOn && engine && appId != 0) {
        cloud_sync(engine, hUser, pipe, appId, /*isDownload=*/true,
                   bGetCallback, freeLastCallback);
    }

    // ------------------------------------------------------------------
    // STEP 5: Launch the game. Preferred path is Valve's own
    // IClientAppManager::LaunchApp — steamclient sets up the full app
    // environment (the SteamStub DRM decryption context, app-running
    // registration, overlay) exactly as the desktop client does, so a
    // DRM-wrapped exe self-decrypts. If LaunchApp is unavailable or
    // fails, fall back to a direct CreateProcess (the pre-LaunchApp
    // behaviour — fine for non-DRM games).
    // ------------------------------------------------------------------
    // Compose the CreateProcess fallback cmdline (argv[1]=exe, argv[2..]=extras).
    char cmdline[4096];
    int  cmdpos = 0;
    cmdpos += snprintf(cmdline + cmdpos, sizeof(cmdline) - cmdpos,
                       "\"%s\"", gameExe);
    for (int i = 2; i < argc; ++i) {
        cmdpos += snprintf(cmdline + cmdpos, sizeof(cmdline) - cmdpos,
                           " \"%s\"", argv[i]);
        if (cmdpos >= (int) sizeof(cmdline) - 2) break;
    }
    cmdline[sizeof(cmdline) - 1] = '\0';

    // Game CWD = dirname(gameExe); exe basename = LaunchApp exit-watch key.
    char gameCwd[MAX_PATH];
    strncpy(gameCwd, gameExe, sizeof(gameCwd) - 1);
    gameCwd[sizeof(gameCwd) - 1] = '\0';
    { char* sep = strrchr(gameCwd, '\\'); if (sep) *sep = '\0'; }
    const char* exeName = strrchr(gameExe, '\\');
    exeName = exeName ? exeName + 1 : gameExe;

    // STEP 5a — try IClientAppManager::LaunchApp.
    bool launchedViaApp = false;
    if (engine && appId != 0) {
        void** engine_vt = *(void***) engine;
        typedef void* (*GetIClientAppManagerFn)(void* self, int hUser, int hPipe);
        GetIClientAppManagerFn getAppMgr = (GetIClientAppManagerFn)
            engine_vt[kVtEngine_GetIClientAppManager / 8];
        void* appMgr = getAppMgr(engine, hUser, pipe);
        log_line("[wn-launcher] IClientEngine.GetIClientAppManager -> %p", appMgr);
        if (appMgr) {
            void** appMgr_vt = *(void***) appMgr;
            // LaunchApp returns an HSteamAPICall (async handle), NOT a
            // success code. CONFIRMED arg order from GameHub's
            // SteamCore::LaunchApplication decompile (FUN_140058f20):
            //   arg2 CGameID*      — 8 bytes; low dword = appId & 0xFFFFFF
            //                        (k_EGameIDTypeApp, mod/instance = 0)
            //   arg3 uLaunchOption — 0 for a normal launch
            //   arg4 ELaunchSource — 300 (dash_applaunch)
            //   arg5 pszUserArgs   — "" when there are no launch options
            typedef uint64_t (*LaunchAppFn)(void* self, void* pGameId,
                                            uint32_t uLaunchOption,
                                            uint32_t eLaunchSource,
                                            const char* pszUserArgs);
            LaunchAppFn launchApp = (LaunchAppFn)
                appMgr_vt[kVtAppMgr_LaunchApp / 8];
            uint64_t gameId = (uint64_t)(appId & 0xFFFFFFu);
            uint64_t apiCall = launchApp(appMgr, &gameId, 0, 300, "");
            log_line("[wn-launcher] IClientAppManager.LaunchApp(appId=%u) "
                     "-> HSteamAPICall=0x%llx", appId,
                     (unsigned long long) apiCall);
            if (apiCall != 0) {
                // LaunchApp is async — steamclient spawns the game. Wait
                // for the process to appear (up to 25s), pumping callbacks.
                for (int i = 0; i < 50 && !launchedViaApp; ++i) {
                    if (count_process_by_name(exeName) > 0) {
                        launchedViaApp = true;
                    } else {
                        if (bGetCallback && freeLastCallback) {
                            char cb[64];
                            while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                        }
                        Sleep(500);
                    }
                }
                if (launchedViaApp)
                    log_line("[wn-launcher] LaunchApp: \"%s\" is running", exeName);
                else
                    log_line("[wn-launcher] LaunchApp dispatched but \"%s\" never "
                             "appeared in 25s — falling back to CreateProcess",
                             exeName);
            } else {
                log_line("[wn-launcher] LaunchApp returned a null call handle "
                         "— falling back to CreateProcess");
            }
        }
    }

    // STEP 5b — LaunchApp brought the game up: stay alive (our process
    // hosts the Steam session) until the game exits. LaunchApp gives no
    // process handle, so watch the process table by exe name.
    if (launchedViaApp) {
        log_line("[wn-launcher] watching \"%s\" for exit (LaunchApp path)", exeName);
        int absent = 0;
        while (absent < 4) {
            Sleep(1000);
            if (bGetCallback && freeLastCallback) {
                char cb[64];
                while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
            }
            absent = (count_process_by_name(exeName) != 0) ? 0 : absent + 1;
        }
        log_line("[wn-launcher] game \"%s\" exited (LaunchApp path)", exeName);
        // STEP 6: Steam Cloud — push saves UP now the game has exited.
        if (loggedOn && engine && appId != 0) {
            cloud_sync(engine, hUser, pipe, appId, /*isDownload=*/false,
                       bGetCallback, freeLastCallback);
        }
        log_line("[wn-launcher] Steam Launcher shutdown");
        return 0;
    }

    // STEP 5c — fallback: direct CreateProcess.
    log_line("[wn-launcher] launching (CreateProcess): %s", cmdline);
    log_line("[wn-launcher] cwd: %s", gameCwd);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL,
                        gameCwd, &si, &pi)) {
        log_line("[wn-launcher] CreateProcess FAILED GLE=%lu", GetLastError());
        return 8;
    }
    log_line("[wn-launcher] game process started pid=%lu — waiting for exit",
             pi.dwProcessId);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    log_line("[wn-launcher] game process exited exitCode=%lu", exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // STEP 6: Steam Cloud — push saves UP now the game has exited.
    if (loggedOn && engine && appId != 0) {
        cloud_sync(engine, hUser, pipe, appId, /*isDownload=*/false,
                   bGetCallback, freeLastCallback);
    }

    log_line("[wn-launcher] Steam Launcher shutdown");
    return 0;
}
