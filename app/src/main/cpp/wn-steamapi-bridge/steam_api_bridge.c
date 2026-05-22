/*
 * wn-steamapi-bridge — replacement steam_api64.dll for Bionic Steam mode.
 *
 * Drops into the game's install dir in place of Valve's stock
 * steam_api64.dll. Exports the SteamAPI_* + SteamAPI_ISteamClient_*
 * + SteamAPI_ISteam*_* flat-C surface. Inside SteamAPI_Init, calls
 * LoadLibrary("steamclient64.dll") — which Wine resolves through the
 * Bionic Steam SteamPath overlay to our own lsteamclient.dll bridge
 * (system32/lsteamclient.dll copied under that name). The bridge
 * dlopens libsteamclient.so, libsteamclient.so's constructor seeds
 * pushed_state from the launch env, and all subsequent Steam SDK
 * calls land on REAL state populated by WinNative's CMClient.
 *
 * Why we need this instead of GameNative's gbe_fork:
 *   gbe_fork is a self-contained Steamworks emulator — it never talks
 *   to a real Steam client / our libsteamclient.so. Replacing the
 *   game's steam_api64.dll with gbe_fork gets the SDK init working
 *   but returns canned/empty data (no real lobby list, no real
 *   friends, etc). This bridge is the missing link: it activates our
 *   existing Wine PE bridge → libsteamclient.so chain that has full
 *   real-Steam state via the CMClient session.
 *
 * Build (MinGW cross from Linux):
 *   x86_64-w64-mingw32-gcc -shared -O2 -fPIC \
 *       -o steam_api64.dll steam_api_bridge.c \
 *       -static-libgcc -Wl,--enable-stdcall-fixup \
 *       -Wl,steam_api64.def
 *
 * Skeleton phase: only SteamAPI_Init / Shutdown / IsSteamRunning /
 * GetHSteamPipe / GetHSteamUser are implemented. The 500+ flat-C
 * SteamAPI_ISteam*_* forwarders will be generated programmatically
 * from the Steamworks SDK headers in a follow-up pass.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define WN_STEAMAPI_EXPORT __declspec(dllexport)

/* Global module handle for steamclient64.dll. Loaded once in DllMain
 * and reused across the SteamAPI surface. NULL → not loaded (init
 * hasn't run yet, or LoadLibrary failed). */
static HMODULE g_steamclient64 = NULL;
static int32_t g_pipe = 0;
static int32_t g_user = 0;

/* CreateInterface function pointer resolved from steamclient64.dll.
 * Pre-2024 SDK era: Steamclient.dll exports a global C function
 * `CreateInterface(const char* version, int* outResultCode)` that
 * returns a void* to the requested ISteam* interface. We resolve it
 * once in DllMain and reuse. */
typedef void* (*CreateInterface_fn)(const char* pchVersion, int* pCode);
static CreateInterface_fn g_CreateInterface = NULL;

/* The ISteamClient interface pointer we get from CreateInterface
 * during SteamAPI_Init. Cached so subsequent Steam API calls don't
 * re-create it. */
static void* g_steam_client = NULL;

/* DllMain runs at PE attach time. We can't actually call
 * LoadLibrary("steamclient64.dll") here — Wine's loader serializes
 * LoadLibrary calls and a LoadLibrary inside DllMain risks
 * deadlocking. The actual load happens lazily in SteamAPI_Init. */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

static int ensure_steamclient_loaded(void) {
    if (g_steamclient64 != NULL) return 1;
    g_steamclient64 = LoadLibraryA("steamclient64.dll");
    if (g_steamclient64 == NULL) {
        OutputDebugStringA("[wn-steamapi-bridge] LoadLibrary(steamclient64.dll) failed");
        return 0;
    }
    g_CreateInterface = (CreateInterface_fn)GetProcAddress(
        g_steamclient64, "CreateInterface");
    if (g_CreateInterface == NULL) {
        OutputDebugStringA(
            "[wn-steamapi-bridge] steamclient64.dll missing CreateInterface");
        return 0;
    }
    OutputDebugStringA(
        "[wn-steamapi-bridge] steamclient64.dll loaded + CreateInterface resolved");
    return 1;
}

/* SteamAPI_Init: load steamclient64.dll, create the ISteamClient
 * interface, allocate a pipe + user. Returns true on success.
 *
 * Steamworks SDK contract: subsequent SteamAPI_GetHSteamPipe /
 * GetHSteamUser must return the values we computed here. SteamAPI
 * _IsSteamRunning must return true after a successful Init. Init can
 * be called multiple times — idempotent. */
WN_STEAMAPI_EXPORT int SteamAPI_Init(void) {
    if (!ensure_steamclient_loaded()) return 0;
    if (g_steam_client == NULL) {
        int code = 0;
        g_steam_client = g_CreateInterface("SteamClient020", &code);
        if (g_steam_client == NULL) {
            /* Try older versions in case the resolved DLL is an older
             * lsteamclient bridge from a different Proton flavour. */
            g_steam_client = g_CreateInterface("SteamClient019", &code);
        }
        if (g_steam_client == NULL) {
            g_steam_client = g_CreateInterface("SteamClient017", &code);
        }
        if (g_steam_client == NULL) {
            OutputDebugStringA(
                "[wn-steamapi-bridge] CreateInterface(SteamClient0XX) returned NULL");
            return 0;
        }
    }
    /* Pipe + user creation goes through ISteamClient vtable. The exact
     * slot offsets are implementation-defined per SDK version, so we
     * delegate to the steamclient64.dll's SteamAPI_ISteamClient_*
     * flat-C exports if present, otherwise fall back to slot 0/2
     * vtable dispatch (Steam SDK convention: slot 0 =
     * CreateSteamPipe, slot 2 = ConnectToGlobalUser). Follow-up: do
     * proper resolution. */
    typedef int (*CreateSteamPipe_fn)(void* self);
    typedef int (*ConnectToGlobalUser_fn)(void* self, int pipe);
    void* csp_sym = GetProcAddress(g_steamclient64,
                                    "SteamAPI_ISteamClient_CreateSteamPipe");
    void* ctgu_sym = GetProcAddress(g_steamclient64,
                                     "SteamAPI_ISteamClient_ConnectToGlobalUser");
    if (csp_sym && ctgu_sym) {
        g_pipe = ((int (*)(void*))csp_sym)(g_steam_client);
        g_user = ((int (*)(void*, int))ctgu_sym)(g_steam_client, g_pipe);
    } else {
        /* Vtable slot 0 / 2 convention. ISteamClient::CreateSteamPipe
         * is the first virtual method; ConnectToGlobalUser is the
         * third. Aarch64/x86_64 ABI: vtable is a pointer-array at
         * **(void***)self. */
        typedef int (*Slot0Fn)(void*);
        typedef int (*Slot2Fn)(void*, int);
        void** vt = *(void***)g_steam_client;
        g_pipe = ((Slot0Fn)vt[0])(g_steam_client);
        g_user = ((Slot2Fn)vt[2])(g_steam_client, g_pipe);
    }
    char log[128];
    snprintf(log, sizeof(log),
             "[wn-steamapi-bridge] SteamAPI_Init pipe=%d user=%d",
             g_pipe, g_user);
    OutputDebugStringA(log);
    return (g_pipe != 0 && g_user != 0) ? 1 : 0;
}

WN_STEAMAPI_EXPORT int SteamAPI_InitSafe(void) { return SteamAPI_Init(); }

WN_STEAMAPI_EXPORT int SteamAPI_InitAnonymousUser(void) { return SteamAPI_Init(); }

WN_STEAMAPI_EXPORT int SteamAPI_InitFlat(void* /*p_outErrMsg*/) {
    return SteamAPI_Init() ? 0 : 2;  /* ESteamAPIInitResult: 0=OK 2=FailedGeneric */
}

WN_STEAMAPI_EXPORT void SteamAPI_Shutdown(void) {
    g_pipe = 0;
    g_user = 0;
    g_steam_client = NULL;
    /* Don't FreeLibrary(steamclient64) — the SDK contract doesn't say
     * unload, and unloading triggers all-callbacks-cancel teardown
     * paths that aren't safe to do from a guest game's main thread. */
    OutputDebugStringA("[wn-steamapi-bridge] SteamAPI_Shutdown");
}

WN_STEAMAPI_EXPORT int SteamAPI_IsSteamRunning(void) { return 1; }

WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamPipe(void) { return g_pipe; }
WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamUser(void) { return g_user; }

WN_STEAMAPI_EXPORT int SteamGameServer_GetHSteamPipe(void) { return g_pipe; }

WN_STEAMAPI_EXPORT int SteamAPI_RestartAppIfNecessary(uint32_t /*unOwnAppID*/) {
    /* false = don't restart via Steam, game proceeds with the current
     * launch. The Bionic mode launch path already ensures the env
     * exposes SteamAppId / STEAMID before exec — there's nothing for
     * a "restart via Steam" to gain. */
    return 0;
}

WN_STEAMAPI_EXPORT void SteamAPI_ReleaseCurrentThreadMemory(void) {}
WN_STEAMAPI_EXPORT void SteamAPI_SetTryCatchCallbacks(int /*bTryCatchCallbacks*/) {}
WN_STEAMAPI_EXPORT void SteamAPI_WriteMiniDump(uint32_t /*code*/, void* /*ex*/, uint32_t /*build*/) {}

/* SteamAPI_RunCallbacks: dispatch any pending CCallbackBase entries.
 * Skeleton: just forward to steamclient64.dll's equivalent if
 * exported; otherwise no-op. */
WN_STEAMAPI_EXPORT void SteamAPI_RunCallbacks(void) {
    if (g_steamclient64 == NULL) return;
    typedef void (*RunCb_fn)(void);
    RunCb_fn p = (RunCb_fn)GetProcAddress(g_steamclient64,
                                           "SteamAPI_RunCallbacks");
    if (p != NULL) p();
}

WN_STEAMAPI_EXPORT void SteamAPI_RegisterCallback(void* /*pCallback*/, int /*iCallback*/) {}
WN_STEAMAPI_EXPORT void SteamAPI_UnregisterCallback(void* /*pCallback*/) {}
WN_STEAMAPI_EXPORT void SteamAPI_RegisterCallResult(void* /*pCallback*/, uint64_t /*hCall*/) {}
WN_STEAMAPI_EXPORT void SteamAPI_UnregisterCallResult(void* /*pCallback*/, uint64_t /*hCall*/) {}

/* The 500+ SteamAPI_ISteam*_* forwarders are missing from this
 * skeleton — to be code-generated from the SDK headers in a
 * follow-up. Without them, games that bind directly to the flat-C
 * surface (newer SDK versions) will fail at first call. The older
 * vtable-dispatch style (Forest, most pre-2020 Unity games) goes
 * through the ISteamClient pointer above and works against this
 * skeleton already.
 *
 * Until the forwarders ship, set CSteamworks-style games that depend
 * on the flat-C exports to ColdClient mode. */
