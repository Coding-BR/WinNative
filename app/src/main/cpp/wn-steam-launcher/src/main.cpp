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
#include <filesystem>
#include <string>
#include <vector>

// LoadLibraryEx search-path flags — fallbacks in case the mingw-w64 headers
// gate them behind a higher _WIN32_WINNT than we compile at.
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_IGNORE_CODE_AUTHZ_LEVEL
#define LOAD_IGNORE_CODE_AUTHZ_LEVEL 0x00000010
#endif

// Calling convention for steamclient's C++ virtual (vtable) methods. On a
// 32-bit build those are MSVC __thiscall (this in ECX); on x86-64 there is
// one convention so it expands to nothing. The flat Steam_* / CreateInterface
// exports stay __cdecl (the compiler default) on both.
#ifdef __i386__
#define WN_THISCALL __thiscall
#else
#define WN_THISCALL
#endif

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

// IClientUser install-script engine slots — CONFIRMED from GameHub SteamAgent
// (FUN_14005a850 / FUN_1400504a0) against steamclient64.dll. The agent runs the
// game's installscript.vdf (VC++ redist, prerequisites, registry setup) through
// these before LaunchApp; a prerequisite-dependent game won't boot without it.
static const int kVtUser_RunInstallScript       = 0x310; // slot 98: bool RunInstallScript(AppId_t, int flags)
static const int kVtUser_IsInstallScriptRunning = 0x318; // slot 99: int  IsInstallScriptRunning()
static const int kVtUser_GetInstallScriptState  = 0x320; // slot 100: bool GetInstallScriptState(char*, uint32, int*, int*)

// IClientEngine slot 43 — GetIClientAppManager(hUser, hPipe). CONFIRMED by
// disassembling GameHub SteamAgent (FUN_140055180). IClientAppManager slot 2
// — LaunchApp — CONFIRMED two ways: GameHub's SteamCore::LaunchApplication
// call site (call [appmgr_vtable+0x10]) and the steamclient64.dll
// IClientAppManager proxy vtable (0 InstallApp, 1 UninstallApp, 2 LaunchApp,
// 3 ShutdownApp).
static const int kVtEngine_GetIClientAppManager = 0x158; // IClientEngine slot 43
static const int kVtAppMgr_LaunchApp            = 0x10;  // IClientAppManager slot 2
// IClientAppManager slots — make steamclient see the game as FullyInstalled
// before LaunchApp. RefreshAppInfo re-scans library folders; GetAppInstallState
// returns EAppState bits (bit 2 = FullyInstalled). Verified working on-device.
static const int kVtAppMgr_RefreshAppInfo       = 0x298; // void RefreshAppInfo()
static const int kVtAppMgr_GetAppInstallState   = 0x20;  // int  GetAppInstallState(AppId_t)

// IClientApps — request the PICS appinfo for the game before LaunchApp so
// steamclient has the launch config loaded. Without this, LaunchApp's job
// fails with EAppUpdateError=9 (MissingConfig) and silently doesn't spawn.
// Slot offsets verified against steamclient64.dll's IClientApps proxy
// thunk table (CONFIRMED in HEAD's launcher; GameHub's SteamCore::RefreshApps
// uses the same call shape).
static const int kVtEngine_GetIClientApps       = 0x88;  // slot 17: IClientApps*(hUser, hPipe)
static const int kVtApps_RequestAppInfoUpdate   = 0x38;  // slot 7:  bool(AppId_t* ids, int n)

// IClientUtils — used to poll LaunchApp's HSteamAPICall result so we can read
// the EAppUpdateError that explains why the spawn silently didn't happen.
// Slots verified by RE of GameHub SteamAgent's poll loop (FUN_14005d5c0 +
// FUN_14005518... at VA 0x140055580, IClientUtils stored at this+0x78) +
// cross-check with the IClientUtilsMap proxy vtable in steamclient64.dll @
// 0x1392e6718.
static const int kVtEngine_GetIClientUtils       = 0x70;  // slot 14: IClientUtils*(HSteamPipe)
static const int kVtUtils_IsAPICallCompleted     = 0xB0;  // slot 22: bool(apiCall, *pbFailed)
static const int kVtUtils_GetAPICallFailureReason = 0xB8; // slot 23: int(apiCall)  ESteamAPICallFailure
static const int kVtUtils_GetAPICallResult       = 0xC0;  // slot 24: bool(apiCall, pCb, cubCb, iCbExpected, *pbFailed)

// LaunchAppResult_t — k_iClientAppManagerCallbacks + 0xB = 0x1361 << 8 | 0x0b.
// Size + error-offset extracted from FUN_140058f20 disassembly:
//   mov $0x13610b, [%rsp+0x20]     iCallbackExpected
//   mov $0x20c,   %r9d              cubCallback (524 bytes)
//   lea -0x10(%rbp), %r8            buffer base
//   cmp %esi, -0x8(%rbp)            error read at buffer+0x8
static const int kLaunchAppResultCallbackId    = 0x13610B;
static const int kLaunchAppResultSize          = 0x20C;
static const int kLaunchResultErrorOffset      = 0x8;     // int32 EAppUpdateError

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
// Top-level unhandled-exception filter — logs the AV that Wine's ntdll
// catches and turns into the opaque GLE=998. Note: VEH (AddVectored
// ExceptionHandler) was tried first but its install call hung the launcher
// on Proton 10 — Wine 10's ntdll appears to have a VEH bug. SEH /
// UnhandledExceptionFilter doesn't go through that code path.
// Dump every currently-loaded module's base + size + path. Called before
// LoadLibrary so the UEF can map the fault IP to a module address range.
static void dump_loaded_modules(const char* when) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        log_line("[wn-launcher] modules(%s): CreateToolhelp32Snapshot failed GLE=%lu",
                 when, GetLastError());
        return;
    }
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int n = 0;
    if (Module32First(snap, &me)) {
        do {
            log_line("[wn-launcher] modules(%s): base=%p size=0x%lx name=%s path=%s",
                     when, me.modBaseAddr, (unsigned long) me.modBaseSize,
                     me.szModule, me.szExePath);
            n++;
        } while (Module32Next(snap, &me));
    }
    log_line("[wn-launcher] modules(%s): total=%d", when, n);
    CloseHandle(snap);
}

static LONG WINAPI launcher_unhandled_filter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    void* ip = er->ExceptionAddress;

    char modName[MAX_PATH] = {0};
    HMODULE faultMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ip, &faultMod)) {
        GetModuleFileNameA(faultMod, modName, sizeof(modName));
    }

    char bytes[3 * 16 + 1] = {0};
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ip, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
            const unsigned char* p = (const unsigned char*)ip;
            int hp = 0;
            for (int i = 0; i < 16 && hp + 3 < (int)sizeof(bytes); ++i) {
                hp += snprintf(bytes + hp, sizeof(bytes) - hp, "%02x ", p[i]);
            }
        }
    }

    log_line("[wn-launcher] UEF: tid=%lu pid=%lu exc=0x%lx at %p mod='%s' bytes=%s",
             (unsigned long) GetCurrentThreadId(),
             (unsigned long) GetCurrentProcessId(),
             er->ExceptionCode, ip, modName[0] ? modName : "(unknown)", bytes);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = (er->ExceptionInformation[0] == 0) ? "read"
                       : (er->ExceptionInformation[0] == 1) ? "write"
                       : (er->ExceptionInformation[0] == 8) ? "DEP" : "?";
        log_line("[wn-launcher] UEF: AV %s fault_addr=0x%llx",
                 op, (unsigned long long) er->ExceptionInformation[1]);
    }

    // Dump page info around the fault IP — explains why DEP fired.
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(ip, &mbi, sizeof(mbi))) {
            log_line("[wn-launcher] UEF: page base=%p size=0x%llx state=0x%lx "
                     "protect=0x%lx alloc_protect=0x%lx type=0x%lx",
                     mbi.BaseAddress, (unsigned long long) mbi.RegionSize,
                     mbi.State, mbi.Protect, mbi.AllocationProtect, mbi.Type);
        }
    }

    // Context register dump (Rax-R15 + Rip + Rsp) — useful to spot
    // which register held the bad function pointer just before the call.
    if (info->ContextRecord) {
        const CONTEXT* c = info->ContextRecord;
        log_line("[wn-launcher] UEF: ctx Rip=%llx Rsp=%llx Rbp=%llx",
                 (unsigned long long) c->Rip,
                 (unsigned long long) c->Rsp,
                 (unsigned long long) c->Rbp);
        log_line("[wn-launcher] UEF: ctx Rax=%llx Rcx=%llx Rdx=%llx Rbx=%llx",
                 (unsigned long long) c->Rax, (unsigned long long) c->Rcx,
                 (unsigned long long) c->Rdx, (unsigned long long) c->Rbx);
        log_line("[wn-launcher] UEF: ctx Rsi=%llx Rdi=%llx R8=%llx R9=%llx",
                 (unsigned long long) c->Rsi, (unsigned long long) c->Rdi,
                 (unsigned long long) c->R8,  (unsigned long long) c->R9);
        // First 8 stack qwords — return-address chain hints.
        const uint64_t* sp = (const uint64_t*) c->Rsp;
        MEMORY_BASIC_INFORMATION smbi;
        if (sp && VirtualQuery((LPCVOID) sp, &smbi, sizeof(smbi))
            && smbi.State == MEM_COMMIT) {
            char chain[256]; int p = 0;
            for (int i = 0; i < 8; ++i) {
                p += snprintf(chain + p, sizeof(chain) - p, "%llx ",
                              (unsigned long long) sp[i]);
            }
            log_line("[wn-launcher] UEF: stack[0..7]=%s", chain);
        }
    }

    dump_loaded_modules("UEF");
    return EXCEPTION_EXECUTE_HANDLER;
}

// Install (idempotent) and start the "Steam Client Service" backed by
// steamservice.exe so steamclient64.dll's IPC queue gets a consumer.
// Without this, IClientAppManager::LaunchApp marshals the call into a
// CSerializingBuffer + IPCClient::DispatchAndReturnAPICall, returns a
// non-zero HSteamAPICall, and the work is silently dropped because there
// is no peer process draining the named-event-backed pipe — verified by
// Ghidra: the CAPIJobLaunchApp factory (steamclient64.dll @0x1384c1610)
// and CUser::SpawnProcess (@0x1389d5dd0) are only reachable from a
// server-side dispatcher that lives in steamservice.exe.
//
// GameHub's SteamAgent does the same (decompiled FUN_140052b40 /
// FUN_1400531b0): OpenServiceW(L"Steam Client Service"), install if
// missing via CreateService, StartService, poll status.
//
// Returns true iff the service is in SERVICE_RUNNING state when we
// return. A false return is non-fatal — LaunchApp will still queue the
// job and the launcher will fall through to the CreateProcess path that
// already works for non-DRM games.
static bool start_steam_client_service(void) {
    const char* kSvcName       = "Steam Client Service";
    const char* kSvcExe        = "C:\\Program Files (x86)\\Steam\\bin\\steamservice.exe";
    const char* kSvcBinPath    = "\"C:\\Program Files (x86)\\Steam\\bin\\steamservice.exe\" /RunAsService";

    DWORD attr = GetFileAttributesA(kSvcExe);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        log_line("[wn-launcher] steamservice: binary not present at %s — "
                 "LaunchApp's IPC queue will have no peer; will use "
                 "CreateProcess fallback", kSvcExe);
        return false;
    }
    log_line("[wn-launcher] steamservice: found %s", kSvcExe);

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        log_line("[wn-launcher] steamservice: OpenSCManager failed GLE=%lu",
                 GetLastError());
        return false;
    }

    SC_HANDLE svc = OpenServiceA(scm, kSvcName, SERVICE_ALL_ACCESS);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            log_line("[wn-launcher] steamservice: service missing — "
                     "installing as \"%s\"", kSvcName);
            svc = CreateServiceA(
                scm, kSvcName, kSvcName,
                SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_DEMAND_START,
                SERVICE_ERROR_NORMAL,
                kSvcBinPath,
                NULL, NULL, NULL, NULL, NULL);
            if (!svc) {
                log_line("[wn-launcher] steamservice: CreateService failed GLE=%lu",
                         GetLastError());
                CloseServiceHandle(scm);
                return false;
            }
            log_line("[wn-launcher] steamservice: service installed");
        } else {
            log_line("[wn-launcher] steamservice: OpenService failed GLE=%lu", err);
            CloseServiceHandle(scm);
            return false;
        }
    }

    SERVICE_STATUS status;
    memset(&status, 0, sizeof(status));
    QueryServiceStatus(svc, &status);
    log_line("[wn-launcher] steamservice: pre-start state=%lu", status.dwCurrentState);

    if (status.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceA(svc, 0, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) {
                log_line("[wn-launcher] steamservice: StartService failed GLE=%lu",
                         err);
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return false;
            }
        }
        int waited = 0;
        while (waited < 30000) {
            if (!QueryServiceStatus(svc, &status)) break;
            if (status.dwCurrentState == SERVICE_RUNNING ||
                status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
            waited += 200;
        }
        log_line("[wn-launcher] steamservice: post-start state=%lu after %dms",
                 status.dwCurrentState, waited);
    }

    bool running = (status.dwCurrentState == SERVICE_RUNNING);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

static bool is_exec_ptr(void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD x = mbi.Protect & 0xFF;
    return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ ||
           x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
}

// Steam Cloud sync is driven entirely on the Android side by wn-steam-client
// (SteamLaunchCloudSync.syncBeforeLaunch / SteamExitCloudSync.syncOnExit).
// The in-Wine IClientRemoteStorage::BeginAppSync path that used to live
// here has been removed: steamclient's own auto-sync runs out-of-band on
// logon, our explicit call was always declined (state=2 stable) and never
// landed any bytes, so it was a no-op that just added latency + log noise.

// -------------------------------------------------------------------------
// Per-container redistributable installer with marker-file tracking.
//
// Replaces steamclient's RunInstallScript path for redistributables
// (VC++, DirectX, etc.) shipped under the game's `_CommonRedist/` folder.
// Steam's path silently fails on Wine for these installers — its
// CreateProcess on `_CommonRedist/vcredist/2022/VC_redist.x64.exe` returns
// ERROR_PATH_NOT_FOUND when the game install dir is a symlink (which it
// always is under our /storage/emulated mount). We do the install
// ourselves with CreateProcess + WaitForSingleObject and persist a marker
// file inside the Wine prefix so the next launch in the same container
// can skip already-installed redists.
//
// Tracking storage: `C:\wn-installed-redists.txt`. This lives inside the
// per-container Wine prefix:
//   <container.rootDir>/.wine/drive_c/wn-installed-redists.txt
// Container deletion removes the prefix → marker goes with it. Recreating
// a same-named container produces a new container ID and a new prefix
// directory, so the marker is absent on first launch and redists install
// fresh. No global state, no cross-container leakage.
//
// Format: one entry per line, tab-separated
//   <filename>\t<size>\t<unix-timestamp>
// Header comment line at top: `# wn-installed-redists v1`.
// Match is by (filename, size) so a game update that replaces the
// bundled installer's bytes triggers a clean reinstall.
// -------------------------------------------------------------------------

static const char* kRedistsMarkerPath = "C:\\wn-installed-redists.txt";

// Derive the game's install directory from its full exe path.
// Returns std::string for simplicity. Strips the trailing filename.
static std::string game_dir_of(const char* gameExePath) {
    if (!gameExePath || !*gameExePath) return {};
    std::string p(gameExePath);
    // Trim trailing backslash if any.
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    auto pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    return p.substr(0, pos);
}

// True if the marker file already records this (filename, size) pair.
// Tolerant of a missing marker file (returns false).
static bool redist_already_installed(const std::string& name, uint64_t size) {
    FILE* f = fopen(kRedistsMarkerPath, "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
        // Parse `<name>\t<size>\t<ts>` — name may contain spaces, never a tab.
        char* tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char* tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        uint64_t lineSize = _strtoui64(tab1 + 1, nullptr, 10);
        if (name == line && lineSize == size) { found = true; break; }
    }
    fclose(f);
    return found;
}

// Append a new entry to the marker file. Creates the file with a header
// on first use. The fourth column (exitCode) records the actual installer
// exit so the user can see at a glance which installs succeeded vs which
// refused (e.g. DXSETUP returning a Wine-specific code because the prefix
// already has builtin d3dx9 DLLs). Both states result in a "marked, skip
// on next launch" decision — only true timeouts (which never invoke this
// function) leave the entry off the file so the user can retry.
static void mark_redist_installed(const std::string& name, uint64_t size,
                                  uint32_t exitCode) {
    bool needHeader = false;
    {
        FILE* probe = fopen(kRedistsMarkerPath, "r");
        if (!probe) needHeader = true; else fclose(probe);
    }
    FILE* f = fopen(kRedistsMarkerPath, "a");
    if (!f) {
        log_line("[wn-launcher] redist mark: cannot open %s for append (GLE=%lu)",
                 kRedistsMarkerPath, GetLastError());
        return;
    }
    if (needHeader) fprintf(f, "# wn-installed-redists v1\n");
    fprintf(f, "%s\t%llu\t%llu\t%lu\n", name.c_str(),
            (unsigned long long) size, (unsigned long long) time(nullptr),
            (unsigned long) exitCode);
    fclose(f);
}

// Lowercase a copy of the input (ASCII only — installer names are
// always plain ASCII).
static std::string to_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char) tolower((unsigned char) c);
    return r;
}

// Pick silent-install command-line args based on the installer's filename.
// Different installer families use incompatible silent-flag conventions —
// `/quiet` works for modern MSI bootstrappers (VC++ 2015-2022), but
// DXSETUP.exe needs `/silent`, InnoSetup wants `/VERYSILENT`, NSIS uses
// `/S`. Picking the wrong flag = installer pops a GUI dialog and waits
// forever for user input (which is exactly the hang the user reported
// with DXSETUP.exe under our prior `/quiet /norestart` default).
//
// Matrix sourced from each installer family's published silent-install
// reference (Microsoft DirectX SDK docs, Visual C++ redistributable
// command-line reference, InnoSetup setup command-line guide, NSIS
// silent-install convention).
//
// Returns the args string to pass after the executable name. Does not
// include the executable itself.
static std::string silent_args_for(const std::string& filename) {
    std::string lower = to_lower(filename);
    // DirectX 9.0c June 2010 redistributable (and older DXSetup builds).
    if (lower == "dxsetup.exe") return "/silent";
    // Modern Visual C++ 2015-2022 redistributable bootstrapper.
    if (lower.rfind("vc_redist", 0) == 0) return "/quiet /norestart";
    // Legacy Visual C++ 2005-2013 redistributable.
    if (lower.rfind("vcredist_", 0) == 0) return "/q /norestart";
    // OpenAL Soft.
    if (lower == "oalinst.exe") return "/silent";
    // PhysX system software installers (NVIDIA).
    if (lower.find("physx") != std::string::npos) return "/quiet";
    // .NET Framework / .NET Runtime installers.
    if (lower.rfind("dotnetfx", 0) == 0
            || lower.rfind("dotnet", 0) == 0
            || lower.rfind("ndp", 0) == 0) {
        return "/q /norestart";
    }
    // UE4/UE5 prerequisites bootstrapper.
    if (lower.find("prereqsetup") != std::string::npos
            || lower.find("ue4prereq") != std::string::npos
            || lower.find("ue5prereq") != std::string::npos) {
        return "/quiet /norestart";
    }
    // InnoSetup (Unity prereqs, many Steam middleware installers).
    // Heuristic: filename literally `setup.exe` — far from perfect but
    // catches the InnoSetup convention.
    if (lower == "setup.exe") return "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
    // Generic MSI-bootstrapper fallback. Works for ~80% of redist
    // installers in the wild. If a specific installer hangs here add
    // it to the dispatch above.
    return "/quiet /norestart";
}

// Three-way result from running a single redist installer:
//   OK       — installer exited cleanly with 0 or 3010 (= success +
//              reboot pending; we never reboot, harmless).
//   FAILED   — installer ran and exited with a non-zero, non-3010 code.
//              Mark anyway (don't retry) since the installer almost
//              certainly knew what it was doing — common case is
//              DXSETUP returning a Wine-specific code because the
//              prefix already has builtin d3dx9 DLLs and it refuses
//              to overwrite. Retrying every launch helps nobody and
//              just adds 1-3 s of pointless work per launch.
//   TIMEOUT  — silent install hung past 90 s. Almost always the wrong
//              silent flag for this installer family (Inno/NSIS/etc.).
//              Do NOT mark, so user can fix the dispatch and retry.
enum class RedistInstallResult { OK, FAILED, TIMEOUT };

// Run a single redist installer with the appropriate silent flags for
// its installer family (see silent_args_for). Per-installer wait
// capped at 90s — silent installs complete in <30s in practice; if
// we hit 90s the installer is almost certainly hung on a GUI dialog
// despite the silent flag (incorrect flag for this installer family).
// Returns the exit code in *outExitCode (0xFFFFFFFF on CreateProcess
// failure, 0xFFFFFFFE on timeout).
static RedistInstallResult run_redist_installer(
        const std::filesystem::path& installer, uint32_t* outExitCode) {
    std::string fn = installer.filename().string();
    std::string args = silent_args_for(fn);
    std::string cmd;
    cmd.reserve(installer.string().size() + args.size() + 4);
    cmd += '"';
    cmd += installer.string();
    cmd += "\" ";
    cmd += args;

    log_line("[wn-launcher] redist install: spawning %s with args: %s",
             fn.c_str(), args.c_str());

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string cwdStr = installer.parent_path().string();
    if (!CreateProcessA(
            installer.string().c_str(),
            cmd.data(),
            nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            cwdStr.empty() ? nullptr : cwdStr.c_str(),
            &si, &pi)) {
        log_line("[wn-launcher] redist install: CreateProcess failed for %s "
                 "(GLE=%lu)",
                 installer.string().c_str(), GetLastError());
        if (outExitCode) *outExitCode = 0xFFFFFFFFu;
        return RedistInstallResult::FAILED;
    }
    constexpr DWORD kPerInstallerTimeoutMs = 90 * 1000;
    DWORD waitResult = WaitForSingleObject(pi.hProcess, kPerInstallerTimeoutMs);
    DWORD exitCode = ~0u;
    bool timedOut = false;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        log_line("[wn-launcher] redist install: %s — 90s timeout (silent "
                 "flag '%s' likely incorrect for this installer family); "
                 "killing process. Not marking as installed so the user "
                 "can retry once the right flag is added.",
                 fn.c_str(), args.c_str());
        TerminateProcess(pi.hProcess, 1);
        // Wait briefly for kill to propagate, then collect.
        WaitForSingleObject(pi.hProcess, 5000);
        timedOut = true;
        exitCode = 0xFFFFFFFEu;  // sentinel for "killed by our timeout"
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (outExitCode) *outExitCode = exitCode;
    if (timedOut) {
        return RedistInstallResult::TIMEOUT;
    }
    // 0 = success, 3010 = success + reboot pending (we never reboot).
    bool ok = (exitCode == 0 || exitCode == 3010);
    log_line("[wn-launcher] redist install: %s exit=%lu (%s)",
             fn.c_str(), exitCode,
             ok ? "ok" : "fail (marking anyway — installer reported a "
                         "definitive non-zero status; treating as "
                         "\"tried, don't retry\" so the splash isn't "
                         "stuck re-running it every launch)");
    return ok ? RedistInstallResult::OK : RedistInstallResult::FAILED;
}

// Main entry: scan <gameDir>/_CommonRedist/ for *.exe and install each
// one not already recorded in the per-container marker file. No-op if
// the folder doesn't exist or the game has no .exe redistributables.
static void scan_and_install_redists(const char* gameExePath) {
    std::string gameDir = game_dir_of(gameExePath);
    if (gameDir.empty()) {
        log_line("[wn-launcher] redist scan: cannot derive game dir from \"%s\"",
                 gameExePath ? gameExePath : "(null)");
        return;
    }
    std::filesystem::path redistRoot = std::filesystem::path(gameDir) / "_CommonRedist";
    std::error_code ec;
    if (!std::filesystem::is_directory(redistRoot, ec)) {
        log_line("[wn-launcher] redist scan: no _CommonRedist at %s — skipping",
                 redistRoot.string().c_str());
        return;
    }
    log_line("[wn-launcher] redist scan: scanning %s", redistRoot.string().c_str());

    // Collect every *.exe under _CommonRedist/.
    std::vector<std::filesystem::path> installers;
    for (auto it = std::filesystem::recursive_directory_iterator(
                redistRoot, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) {
            log_line("[wn-launcher] redist scan: iteration error: %s",
                     ec.message().c_str());
            ec.clear();
            continue;
        }
        const auto& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        std::string ext = p.extension().string();
        for (auto& c : ext) c = (char) tolower((unsigned char) c);
        if (ext != ".exe") continue;
        installers.push_back(p);
    }

    if (installers.empty()) {
        log_line("[wn-launcher] redist scan: 0 *.exe installers under %s",
                 redistRoot.string().c_str());
        return;
    }
    log_line("[wn-launcher] redist scan: found %zu installer(s)", installers.size());

    int installed = 0, skipped = 0, failedMarked = 0, timedOut = 0;
    int idx = 0;
    for (const auto& installer : installers) {
        ++idx;
        std::string name = installer.filename().string();
        uintmax_t sizeUm = std::filesystem::file_size(installer, ec);
        uint64_t size = ec ? 0 : (uint64_t) sizeUm;
        ec.clear();
        if (redist_already_installed(name, size)) {
            log_line("[wn-launcher] redistributable %s already installed in this "
                     "container, skipping (%d/%zu)",
                     name.c_str(), idx, installers.size());
            ++skipped;
            continue;
        }
        log_line("[wn-launcher] installing redistributable: %s (%d/%zu, %llu bytes)",
                 name.c_str(), idx, installers.size(),
                 (unsigned long long) size);
        uint32_t exitCode = 0;
        RedistInstallResult r = run_redist_installer(installer, &exitCode);
        switch (r) {
            case RedistInstallResult::OK:
                mark_redist_installed(name, size, exitCode);
                ++installed;
                break;
            case RedistInstallResult::FAILED:
                // Mark with the failure exit code so we don't re-run on
                // every launch. Common case: DXSETUP refusing to install
                // over Wine builtin DirectX 9 DLLs (returns a Wine-specific
                // non-zero code). The launcher's job is to do its best; we
                // don't keep banging on an installer that's told us it
                // won't proceed.
                mark_redist_installed(name, size, exitCode);
                ++failedMarked;
                break;
            case RedistInstallResult::TIMEOUT:
                // Timeout almost always means the silent flag is wrong
                // (Inno/NSIS/etc.) and the installer is hung on a GUI
                // dialog. NOT marked, so the next launch retries after
                // the user (or a code change) fixes silent_args_for.
                ++timedOut;
                break;
        }
    }
    log_line("[wn-launcher] redist scan: installed %d, skipped %d, "
             "failed-marked %d, timed-out-unmarked %d (of %zu total)",
             installed, skipped, failedMarked, timedOut, installers.size());
}

int main(int argc, char** argv) {
    setbuf(stderr, NULL);
    setbuf(stdout, NULL);
    // No console-hide step: this binary is linked --subsystem,windows
    // (see build.sh), so Wine never attaches a console and never maps
    // a visible console window. Earlier attempts to hide a console
    // attached by --subsystem,console raced the X server and let the
    // window briefly map before SW_HIDE took effect — which then
    // satisfied isApplicationWindow() and prematurely closed the
    // Android-side preloader. Truncate the log file at process start
    // so each launch's trace is self-contained.
    { FILE* lf = fopen("C:\\wn-launcher.log", "w"); if (lf) fclose(lf); }
    log_line("[wn-launcher] Steam Launcher in-process Steam launcher starting (pid=%lu tid=%lu)",
             (unsigned long) GetCurrentProcessId(),
             (unsigned long) GetCurrentThreadId());

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

    // Env-propagation probe: log a handful of Proton/Wine env vars that
    // we expect to inherit from the spawner so we can tell whether the
    // env layer made it through wine -> winhandler -> steam.exe. If
    // PROTON_DISABLE_LSTEAMCLIENT is "1" here but ntdll still prints no
    // "lsteamclient disabled.", the gate is reading from a different
    // source than libc getenv.
    {
        const char* probes[] = {
            "PROTON_DISABLE_LSTEAMCLIENT",
            "WINEDLLOVERRIDES",
            "WINEDEBUG",
            "WINEESYNC",
            "WINENTSYNC",
            "PROTON_USE_WOW64",
        };
        for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
            const char* libc = getenv(probes[i]);
            char winv[256] = {0};
            DWORD wlen = GetEnvironmentVariableA(probes[i], winv, sizeof(winv));
            log_line("[wn-launcher] env probe: %-30s libc=%-10s win32=%s",
                     probes[i],
                     (libc && *libc) ? libc : "(unset)",
                     (wlen > 0 && wlen < sizeof(winv)) ? winv : "(unset)");
        }
    }

    // ------------------------------------------------------------------
    // STEP 1: LoadLibrary Valve's real steamclient64.dll from the Steam
    // dir, by FULL ABSOLUTE PATH so Wine's DllOverrides (which redirect a
    // bare "steamclient64.dll" to the bionic bridge's lsteamclient.dll)
    // don't kick in — the bridge's Steam_CreateGlobalUser is a stub.
    //
    // We host the 64-bit steamclient64.dll — the same binary GameHub's
    // agent uses — so IClientAppManager::LaunchApp drives the game through
    // steamclient's own app-launch path. We preload the dependency chain —
    // Wine CRT thunks then Steam's own tier0_s64/vstdlib_s64 — and try a
    // cascade of LoadLibrary strategies with cold-start backoff.
    // ------------------------------------------------------------------
    const char* kSteamDir = "C:\\Program Files (x86)\\Steam";
    SetDllDirectoryA(kSteamDir);
    {
        // Wine CRT thunks by short name (found in system32); Steam's
        // siblings by absolute path. Every preload is best-effort — a
        // missing optional DLL just logs and continues.
        struct Preload { const char* name; bool fullPath; };
        // tier0_s64.dll and vstdlib_s64.dll INTENTIONALLY DROPPED from
        // preload on Proton 10+. Their DllMain spawns a worker thread that
        // later exits and trips Wine 10's stricter RtlProcessFlsData over
        // a stale FLS callback (some other DLL's bad pointer; we saw a DEP
        // AV at 0x7ffc90ae08 = unmapped). steamclient64.dll will pull both
        // in via its import table when we LoadLibrary it; SetDllDirectory
        // (steamDir) above gives the search path. CRT preloads stay — they
        // never spawn a worker.
        const Preload preloads[] = {
            { "msvcr120.dll", false }, { "msvcp120.dll", false },
            { "vcruntime140.dll", false }, { "msvcp140.dll", false },
            { "video64.dll", true }, { "SteamUI.dll", true },
        };
        for (const Preload& p : preloads) {
            HMODULE dm;
            if (p.fullPath) {
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s\\%s", kSteamDir, p.name);
                dm = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            } else {
                dm = LoadLibraryA(p.name);
            }
            if (dm) {
                log_line("[wn-launcher] preload %s: ok (%p)", p.name, dm);
            } else {
                log_line("[wn-launcher] preload %s: failed GLE=%lu (continuing)",
                         p.name, GetLastError());
            }
        }
    }

    log_line("[wn-launcher] preloads done; installing unhandled-exception filter");
    LPTOP_LEVEL_EXCEPTION_FILTER prevFilter =
        SetUnhandledExceptionFilter(launcher_unhandled_filter);
    log_line("[wn-launcher] UEF installed (prev=%p)", prevFilter);
    dump_loaded_modules("pre-LoadLibrary");

    char steamclientPath[MAX_PATH];
    snprintf(steamclientPath, sizeof(steamclientPath),
             "%s\\steamclient64.dll", kSteamDir);

    // Cascade of load strategies. Different flag combinations exercise
    // different paths in Wine's PE loader; under FEX/arm64ec one often
    // works where another faults. We never use DONT_RESOLVE_DLL_REFERENCES
    // — it skips DllMain, leaving steamclient uninitialised.
    struct LoadAttempt { DWORD flags; const char* desc; };
    const LoadAttempt attempts[] = {
        { LOAD_WITH_ALTERED_SEARCH_PATH, "LOAD_WITH_ALTERED_SEARCH_PATH" },
        { 0, "no flags" },
        { LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, "SEARCH_DEFAULT_DIRS" },
        { LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_IGNORE_CODE_AUTHZ_LEVEL,
          "SEARCH_DEFAULT_DIRS + IGNORE_CFG" },
        { LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,
          "DLL_LOAD_DIR + SYSTEM32" },
    };
    const int kAttempts = (int)(sizeof(attempts) / sizeof(attempts[0]));
    HMODULE lsc = NULL;
    DWORD lastErr = 0;
    for (int i = 0; i < kAttempts && !lsc; i++) {
        lsc = LoadLibraryExA(steamclientPath, NULL, attempts[i].flags);
        if (lsc) {
            log_line("[wn-launcher] steamclient64.dll loaded at %p "
                     "(strategy %d/%d: %s)",
                     lsc, i + 1, kAttempts, attempts[i].desc);
            break;
        }
        lastErr = GetLastError();
        log_line("[wn-launcher] load strategy %d/%d (%s) FAILED, GLE=%lu",
                 i + 1, kAttempts, attempts[i].desc, lastErr);
        Sleep(50);
    }
    // Cold-start backoff: a freshly-booted prefix (wineserver warming,
    // siblings still unpacking) can fail every strategy transiently.
    for (int round = 0; round < 3 && !lsc; round++) {
        log_line("[wn-launcher] steamclient64.dll cold-start retry "
                 "round %d/3 after 500ms", round + 1);
        Sleep(500);
        for (int i = 0; i < kAttempts && !lsc; i++) {
            lsc = LoadLibraryExA(steamclientPath, NULL, attempts[i].flags);
            if (!lsc) lastErr = GetLastError();
        }
        if (lsc) {
            log_line("[wn-launcher] steamclient64.dll loaded at %p "
                     "(retry round %d)", lsc, round + 1);
        }
    }
    // Last resort: plain LoadLibraryA routes through a different loader
    // path than LoadLibraryExA on some Wine builds.
    if (!lsc) {
        lsc = LoadLibraryA(steamclientPath);
        if (lsc) {
            log_line("[wn-launcher] steamclient64.dll loaded at %p "
                     "(plain LoadLibraryA)", lsc);
        } else {
            lastErr = GetLastError();
        }
    }
    if (!lsc) {
        // Distinguish "file bad" from "DllMain init faulted": a DATAFILE
        // load maps the image without running its code.
        HMODULE probe = LoadLibraryExA(steamclientPath, NULL,
                                       LOAD_LIBRARY_AS_DATAFILE);
        if (probe) {
            log_line("[wn-launcher] diag: DATAFILE load OK — file is "
                     "well-formed; failure is in DllMain/runtime init");
            FreeLibrary(probe);
        } else {
            log_line("[wn-launcher] diag: DATAFILE load also FAILED, GLE=%lu",
                     GetLastError());
        }
        log_line("[wn-launcher] LoadLibrary(%s) FAILED after all strategies, "
                 "last GLE=%lu", steamclientPath, lastErr);
        return 2;
    }

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

            typedef void* (WN_THISCALL *GetIClientUserFn)(void* self, int hUser, int hPipe);
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
                typedef bool (WN_THISCALL *BHasCachedCredsFn)(void* self, const char* user);
                BHasCachedCredsFn hasCached = (BHasCachedCredsFn)
                    iuser_vt[kVtUser_BHasCachedCreds / 8];
                log_line("[wn-launcher] BHasCachedCredentials(%s) = %d",
                         user, hasCached(iuser, user) ? 1 : 0);

                // STEP 3.1 — SetLoginToken(refreshToken, accountName).
                // Both args are C-strings (confirmed: SteamAgent passes
                // RDX=token, R8=accountName).
                typedef int (WN_THISCALL *SetLoginTokenFn)(void* self, const char* token,
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
                typedef void* (WN_THISCALL *GetSteamIDFn)(void* self, void* outBuf);
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
                typedef int (WN_THISCALL *LogOnFn)(void* self, uint64_t steamID);
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
        // 60s. The i386 steamclient under FEX often fails its WebSocket CM
        // attempts first and only connects after falling back to a UDP CM
        // ~25s in, so a 20s window timed out just before logon completed.
        // Hard auth failures (EResult 5/15/84) still bail early below.
        const int kMaxPolls = 600;  // 600 * 100ms = 60s
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
    // STEP 4.4: Request fresh PICS appinfo for the game so steamclient has
    // the launch config loaded before LaunchApp. Without this, LaunchApp
    // accepts the job (returns a valid HSteamAPICall) but the CGameLauncher
    // job aborts with EAppUpdateError=9 (MissingConfig) and silently never
    // spawns the game — empirically observed on MHST (appId 2356560)
    // via the LaunchAppResult_t poll added in STEP 5. Mirrors GameHub
    // SteamCore::RefreshApps.
    // ------------------------------------------------------------------
    if (loggedOn && engine && appId != 0) {
        void** engine_vt = *(void***) engine;
        typedef void* (WN_THISCALL *GetIClientAppsFn)(void* self, int hUser, int hPipe);
        GetIClientAppsFn getApps = (GetIClientAppsFn)
            engine_vt[kVtEngine_GetIClientApps / 8];
        void* iApps = getApps(engine, hUser, pipe);
        log_line("[wn-launcher] IClientEngine.GetIClientApps -> %p", iApps);
        if (iApps) {
            void** apps_vt = *(void***) iApps;
            void* reqP = apps_vt[kVtApps_RequestAppInfoUpdate / 8];
            if (!is_exec_ptr(reqP)) {
                log_line("[wn-launcher] RequestAppInfoUpdate slot not executable — "
                         "skipping appinfo refresh");
            } else {
                typedef bool (WN_THISCALL *RequestAppInfoUpdateFn)(void* self,
                                                       uint32_t* appIds, int count);
                RequestAppInfoUpdateFn reqInfo = (RequestAppInfoUpdateFn) reqP;
                uint32_t appIds[1] = { appId };
                bool reqRc = reqInfo(iApps, appIds, 1);
                log_line("[wn-launcher] RequestAppInfoUpdate(appId=%u) -> %d",
                         appId, reqRc ? 1 : 0);
                // Wait for AppInfoUpdateComplete_t (callback id 1003) up
                // to 10s, draining other callbacks meanwhile.
                bool appInfoDone = false;
                int  waited = 0;
                for (int i = 0; i < 100 && !appInfoDone; ++i) {
                    if (bGetCallback && freeLastCallback) {
                        char cb[64];
                        while (bGetCallback(pipe, cb)) {
                            if (*(int*)(cb + 4) == 1003) appInfoDone = true;
                            freeLastCallback(pipe);
                        }
                    }
                    if (!appInfoDone) { Sleep(100); waited += 100; }
                }
                log_line("[wn-launcher] AppInfoUpdateComplete_t %s after %dms",
                         appInfoDone ? "received" : "NOT received", waited);
            }
        }
    }

    // ------------------------------------------------------------------
    // STEP 4.5: Make steamclient treat the game as FullyInstalled before
    // LaunchApp — without it LaunchApp's async job aborts and the game is
    // never spawned. Mirrors GameHub SteamAgent's install-state gate
    // (FUN_14005a850).
    // ------------------------------------------------------------------
    if (loggedOn && engine && appId != 0) {
        void** engine_vt = *(void***) engine;
        typedef void* (WN_THISCALL *GetIfaceFn)(void* self, int hUser, int hPipe);
        void* appMgr = ((GetIfaceFn) engine_vt[kVtEngine_GetIClientAppManager / 8])
                           (engine, hUser, pipe);
        log_line("[wn-launcher] readiness: IClientAppManager=%p", appMgr);

        // Refresh library/appinfo so the staged appmanifest_<id>.acf is
        // parsed into steamclient's in-memory app-state table, then poll
        // GetAppInstallState until k_EAppStateFullyInstalled (bit 2) is set.
        if (appMgr) {
            void** am_vt = *(void***) appMgr;
            void* refreshP = am_vt[kVtAppMgr_RefreshAppInfo / 8];
            void* stateP   = am_vt[kVtAppMgr_GetAppInstallState / 8];
            if (is_exec_ptr(refreshP)) {
                typedef void (WN_THISCALL *RefreshAppInfoFn)(void* self);
                ((RefreshAppInfoFn) refreshP)(appMgr);
                log_line("[wn-launcher] RefreshAppInfo() called");
            }
            if (is_exec_ptr(stateP)) {
                typedef int (WN_THISCALL *GetAppInstallStateFn)(void* self, uint32_t app);
                GetAppInstallStateFn getInstallState = (GetAppInstallStateFn) stateP;
                int st = 0;
                for (int i = 0; i < 100; ++i) {
                    st = getInstallState(appMgr, appId);
                    if (st & 4) break;
                    if (bGetCallback && freeLastCallback) {
                        char cb[64];
                        while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                    }
                    Sleep(100);
                }
                log_line("[wn-launcher] GetAppInstallState(appId=%u) = 0x%x (%s)",
                         appId, st,
                         (st & 4) ? "FullyInstalled"
                                  : "NOT installed — LaunchApp may no-op");
            }
        }
    }

    // ------------------------------------------------------------------
    // STEP 4.5: Scan the game's _CommonRedist/ folder and run each
    // *.exe redistributable installer that hasn't already been installed
    // in THIS container. Replaces steamclient's RunInstallScript path
    // (which silently fails on Wine because Steam's CreateProcess on
    // the redist installers returns ERROR_PATH_NOT_FOUND when the game
    // install dir is a /storage/emulated symlink — `step 1/0` then
    // never advances, locking the launcher).
    //
    // Tracking is per-container: the marker file lives at
    //   C:\wn-installed-redists.txt
    // which is inside the Wine prefix (deleted when the container is
    // deleted, fresh when a same-named container is recreated with a
    // new ID). Each entry is `<filename>\t<size>\t<unix-ts>`. Re-check
    // is by (filename, size) — a game update that changes the bundled
    // installer's bytes triggers reinstall.
    //
    // Skipped if no _CommonRedist/ folder exists.
    // ------------------------------------------------------------------
    scan_and_install_redists(gameExe);

    // STEP 4.6 is intentionally skipped when scan_and_install_redists
    // ran — the scan handles redistributables directly and Steam's
    // RunInstallScript is the same broken path. The original block is
    // kept below behind a compile-time flag for emergency revert.
    constexpr bool kRunSteamInstallScript = false;
    if (kRunSteamInstallScript && loggedOn && engine && appId != 0) {
        void** engine_vt = *(void***) engine;
        typedef void* (WN_THISCALL *GetIClientUserFn)(void* self, int hUser, int hPipe);
        GetIClientUserFn getUser = (GetIClientUserFn)
            engine_vt[kVtEngine_GetIClientUser / 8];
        void* iUser = getUser(engine, hUser, pipe);
        log_line("[wn-launcher] install script: IClientUser -> %p", iUser);
        if (iUser) {
            void** user_vt = *(void***) iUser;
            void* runP   = user_vt[kVtUser_RunInstallScript / 8];
            void* progP  = user_vt[kVtUser_IsInstallScriptRunning / 8];
            void* stateP = user_vt[kVtUser_GetInstallScriptState / 8];
            if (!is_exec_ptr(runP) || !is_exec_ptr(progP) || !is_exec_ptr(stateP)) {
                log_line("[wn-launcher] install script: a vtable slot is not "
                         "executable — skipping (offsets may not match this "
                         "steamclient build)");
            } else {
                typedef bool (WN_THISCALL *RunInstallScriptFn)(void* self,
                                               uint32_t app, int flags);
                typedef int  (WN_THISCALL *IsInstallScriptRunningFn)(void* self);
                typedef bool (WN_THISCALL *GetInstallScriptStateFn)(void* self,
                                               char* buf, uint32_t cb,
                                               int* outA, int* outB);
                bool started = ((RunInstallScriptFn) runP)(iUser, appId, 0);
                log_line("[wn-launcher] RunInstallScript(appId=%u) -> %d",
                         appId, started ? 1 : 0);
                if (started) {
                    IsInstallScriptRunningFn isRunning =
                        (IsInstallScriptRunningFn) progP;
                    GetInstallScriptStateFn getState =
                        (GetInstallScriptStateFn) stateP;
                    // kMaxWaitMs: hard ceiling — the original 180s safety
                    //   net for a script that's actually doing real work.
                    // kEmptyScriptGraceMs: short grace for total_steps to
                    //   populate after RunInstallScript returns. Many
                    //   modern Steam games ship an installscript.vdf that
                    //   declares no actual steps (UE4/Unity games whose
                    //   redistributables are bundled under _CommonRedist
                    //   and were already run at install time). Steam
                    //   returns RunInstallScript=true but the script's
                    //   total_steps stays at 0. Without breaking out we
                    //   loop the full 180s waiting for steps that will
                    //   never appear — locking up the launcher and the
                    //   splash screen for any such game.
                    //   Symptom we're fixing: log shows `step 1/0` and
                    //   then no progress for the full 180s before
                    //   proceeding to steamservice → LaunchApp.
                    // kNoProgressMs: catch a script that DOES report a
                    //   real total but stalls on one step (network
                    //   redistributable download blocked, etc.).
                    const int kMaxWaitMs = 180000;
                    const int kEmptyScriptGraceMs = 3000;
                    const int kNoProgressMs = 30000;
                    int waited = 0, lastStep = -1, lastTotal = -1;
                    int lastProgressAtMs = 0;
                    const char* breakReason = "complete";
                    while (waited < kMaxWaitMs) {
                        if (isRunning(iUser) == 0) {
                            breakReason = "complete";
                            break;
                        }
                        char buf[1024];
                        int stepNo = 0, stepCount = 0;
                        if (getState(iUser, buf, sizeof(buf),
                                     &stepNo, &stepCount)) {
                            if (stepNo != lastStep || stepCount != lastTotal) {
                                lastStep = stepNo;
                                lastTotal = stepCount;
                                lastProgressAtMs = waited;
                                log_line("[wn-launcher] install script: step %d/%d",
                                         stepNo, stepCount);
                            }
                            // Empty / no-op script: total never published.
                            // After the grace period, assume nothing real
                            // is going to run and proceed.
                            if (stepCount == 0
                                    && waited >= kEmptyScriptGraceMs) {
                                breakReason = "empty (total_steps=0)";
                                break;
                            }
                            // Real script but stuck on one step.
                            if (waited - lastProgressAtMs >= kNoProgressMs) {
                                breakReason = "no-progress timeout";
                                break;
                            }
                        }
                        if (bGetCallback && freeLastCallback) {
                            char cb[64];
                            while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                        }
                        Sleep(200);
                        waited += 200;
                    }
                    log_line("[wn-launcher] install script finished in %dms (%s)",
                             waited,
                             waited >= kMaxWaitMs ? "hard-timeout"
                                                  : breakReason);
                }
            }
        }
    }

    // Cloud sync: driven entirely Android-side by wn-steam-client; no
    // in-Wine step here.

    // ------------------------------------------------------------------
    // STEP 4.8: Bring up steamservice.exe as the IPC backend so
    // IClientAppManager::LaunchApp's marshaled job has someone to drain
    // the named-event queue. Without this, LaunchApp returns a valid
    // HSteamAPICall but never spawns the game; the launcher falls through
    // to the CreateProcess fallback below (which is fine for non-DRM
    // games, but bypasses steamclient's CGameLauncher path that SteamStub
    // DRM games need for in-process self-decryption).
    //
    // No-op if steamservice.exe is not staged in <Steam>/bin/. The
    // CreateProcess fallback then carries the launch as before.
    // ------------------------------------------------------------------
    bool svcRunning = start_steam_client_service();
    log_line("[wn-launcher] steamservice running: %d", svcRunning ? 1 : 0);

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
        typedef void* (WN_THISCALL *GetIClientAppManagerFn)(void* self, int hUser, int hPipe);
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
            typedef uint64_t (WN_THISCALL *LaunchAppFn)(void* self, void* pGameId,
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

            // Poll the LaunchAppResult_t to surface the EAppUpdateError
            // that explains why the spawn silently doesn't happen. This is
            // a diagnostic gate, not a control-flow one — we still
            // continue to the process-watch loop below, then fall back to
            // CreateProcess if no game process appears, but the logged
            // error code tells us exactly which precondition LaunchApp
            // bailed on. Mirrors GameHub SteamAgent's FUN_14005d5c0.
            if (apiCall != 0) {
                typedef void* (WN_THISCALL *GetIClientUtilsFn)(void* self, int hPipe);
                GetIClientUtilsFn getUtils = (GetIClientUtilsFn)
                    engine_vt[kVtEngine_GetIClientUtils / 8];
                void* utils = getUtils(engine, pipe);
                log_line("[wn-launcher] IClientEngine.GetIClientUtils -> %p", utils);
                if (utils) {
                    void** utils_vt = *(void***) utils;
                    void* isCompletedP = utils_vt[kVtUtils_IsAPICallCompleted / 8];
                    void* getResultP   = utils_vt[kVtUtils_GetAPICallResult / 8];
                    void* getReasonP   = utils_vt[kVtUtils_GetAPICallFailureReason / 8];
                    log_line("[wn-launcher] utils vt IsAPICallCompleted=%p "
                             "GetAPICallFailureReason=%p GetAPICallResult=%p",
                             isCompletedP, getReasonP, getResultP);
                    if (is_exec_ptr(isCompletedP) && is_exec_ptr(getResultP)) {
                        typedef bool (WN_THISCALL *IsAPICallCompletedFn)(void* self,
                                                       uint64_t apiCall, bool* pbFailed);
                        typedef int  (WN_THISCALL *GetFailureReasonFn)(void* self,
                                                       uint64_t apiCall);
                        typedef bool (WN_THISCALL *GetAPICallResultFn)(void* self,
                                                       uint64_t apiCall, void* pCb,
                                                       int cubCb, int iCbExpected,
                                                       bool* pbFailed);
                        IsAPICallCompletedFn isCompleted = (IsAPICallCompletedFn) isCompletedP;
                        GetFailureReasonFn   getReason   = (GetFailureReasonFn) getReasonP;
                        GetAPICallResultFn   getResult   = (GetAPICallResultFn) getResultP;

                        const int kPollMaxMs = 10000;
                        int  waited = 0;
                        bool failed = false;
                        bool completed = false;
                        while (waited < kPollMaxMs) {
                            failed = false;
                            completed = isCompleted(utils, apiCall, &failed);
                            if (completed) break;
                            if (bGetCallback && freeLastCallback) {
                                char cb[64];
                                while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                            }
                            Sleep(100);
                            waited += 100;
                        }
                        if (!completed) {
                            log_line("[wn-launcher] LaunchApp poll: TIMED OUT "
                                     "after %dms — job still pending", waited);
                        } else if (failed) {
                            int reason = is_exec_ptr(getReasonP) ? getReason(utils, apiCall) : -99;
                            log_line("[wn-launcher] LaunchApp poll: API CALL FAILED "
                                     "after %dms, reason=%d "
                                     "(-1=NoFailure 0=SteamGone 1=NetworkFailure "
                                     "2=InvalidHandle 3=MismatchedCallback)",
                                     waited, reason);
                        } else {
                            unsigned char buf[kLaunchAppResultSize];
                            memset(buf, 0, sizeof(buf));
                            bool resFailed = false;
                            bool got = getResult(utils, apiCall, buf,
                                                  kLaunchAppResultSize,
                                                  kLaunchAppResultCallbackId,
                                                  &resFailed);
                            int eAppError = *(int*)(buf + kLaunchResultErrorOffset);
                            log_line("[wn-launcher] LaunchApp poll: COMPLETED in %dms "
                                     "got=%d resFailed=%d EAppUpdateError=%d "
                                     "(0=NoError 1=Unspecified 2=Paused 3=Cancelled "
                                     "4=Suspended 5=NoSubscription 6=NoConnection "
                                     "7=Timeout 8=MissingKey 9=MissingConfig "
                                     "0xE=AppLocked 0xF=OtherSessionPlaying "
                                     "0x10=AlreadyRunning 0x21=33 0x23=35 0x2D=45)",
                                     waited, got ? 1 : 0, resFailed ? 1 : 0, eAppError);
                            // Hex dump first 32 bytes of the result struct so a
                            // future RE pass can verify the field layout.
                            char hex[3 * 32 + 1];
                            int hp = 0;
                            for (int i = 0; i < 32; ++i) {
                                hp += snprintf(hex + hp, sizeof(hex) - hp, "%02x ", buf[i]);
                            }
                            log_line("[wn-launcher] LaunchApp result hex+0..32: %s", hex);
                        }
                    } else {
                        log_line("[wn-launcher] LaunchApp poll: IClientUtils vtable "
                                 "slots not executable — skipping poll");
                    }
                }
            }

            if (apiCall != 0) {
                // LaunchApp queued the launch asynchronously. Wait up to 60s
                // for the game process to appear, draining callbacks so
                // steamclient's launch job advances; if it never shows, fall
                // back to CreateProcess.
                for (int i = 0; i < 120 && !launchedViaApp; ++i) {
                    if (count_process_by_name(exeName) > 0) {
                        launchedViaApp = true;
                        break;
                    }
                    if (bGetCallback && freeLastCallback) {
                        char cb[64];
                        while (bGetCallback(pipe, cb)) freeLastCallback(pipe);
                    }
                    Sleep(500);
                }
                if (launchedViaApp)
                    log_line("[wn-launcher] LaunchApp: \"%s\" is running", exeName);
                else
                    log_line("[wn-launcher] LaunchApp dispatched but \"%s\" never "
                             "appeared — falling back to CreateProcess", exeName);
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
        // Cloud upload at exit is driven Android-side by SteamExitCloudSync.
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

    // Cloud upload at exit is driven Android-side by SteamExitCloudSync.

    log_line("[wn-launcher] Steam Launcher shutdown");
    return 0;
}
