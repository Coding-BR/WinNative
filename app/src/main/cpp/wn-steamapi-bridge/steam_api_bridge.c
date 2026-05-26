
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define WN_STEAMAPI_EXPORT __declspec(dllexport)

static HMODULE g_steamclient64 = NULL;
static int32_t g_pipe = 0;
static int32_t g_user = 0;

typedef void* (*CreateInterface_fn)(const char* pchVersion, int* pCode);
static CreateInterface_fn g_CreateInterface = NULL;

static void* g_steam_client = NULL;

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

WN_STEAMAPI_EXPORT int SteamAPI_Init(void) {
    if (!ensure_steamclient_loaded()) return 0;
    if (g_steam_client == NULL) {
        int code = 0;
        g_steam_client = g_CreateInterface("SteamClient020", &code);
        if (g_steam_client == NULL) {
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
    OutputDebugStringA("[wn-steamapi-bridge] SteamAPI_Shutdown");
}

WN_STEAMAPI_EXPORT int SteamAPI_IsSteamRunning(void) { return 1; }

WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamPipe(void) { return g_pipe; }
WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamUser(void) { return g_user; }

WN_STEAMAPI_EXPORT int SteamGameServer_GetHSteamPipe(void) { return g_pipe; }

WN_STEAMAPI_EXPORT int SteamAPI_RestartAppIfNecessary(uint32_t /*unOwnAppID*/) {
    return 0;
}

WN_STEAMAPI_EXPORT void SteamAPI_ReleaseCurrentThreadMemory(void) {}
WN_STEAMAPI_EXPORT void SteamAPI_SetTryCatchCallbacks(int /*bTryCatchCallbacks*/) {}
WN_STEAMAPI_EXPORT void SteamAPI_WriteMiniDump(uint32_t /*code*/, void* /*ex*/, uint32_t /*build*/) {}

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
