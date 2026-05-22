/* Bridge lifecycle overrides — route Forest's CSteamworks-style
 * `SteamClient()->GetISteamMatchmaking()` dispatch through Valve's
 * REAL in-process Windows `steamclient64.dll` (staged by Steam Launcher at
 * `C:\Program Files (x86)\Steam\steamclient64.dll`) instead of
 * dead-ending at gbe_fork's LAN-only emulator.
 *
 * Symptom we fix: Forest reaches MULTIPLAYER → JOIN GAME, gets
 * "Dedicated (LAN)" hits between two devices on the same Wi-Fi but
 * "Dedicated (Internet)" stays empty. gbe_fork's matchmaking does UDP
 * broadcast on the local subnet — no Valve CM connection — which is
 * exactly the observed pattern.
 *
 * Strategy (Steam Launcher refinement, 2026-05-21):
 *   1. Keep gbe_fork initialized in-process. It backs ~1200 forwarded
 *      flat-C exports (friends, userstats, cloud, callbacks, …) that
 *      MUST keep working. Removing gbe would crash the game.
 *   2. Return gbe's real `ISteamClient*` from `SteamClient()` so all
 *      its non-matchmaking vtable slots dispatch normally — but…
 *   3. …RUNTIME-PATCH gbe's vtable, replacing slot 10
 *      (`GetISteamMatchmaking`) and slot 11
 *      (`GetISteamMatchmakingServers`) with thunks that return
 *      Valve's REAL matchmaking pointers (acquired via
 *      `get_our_matchmaking()` in steam_api_bridge_overrides.c,
 *      which `LoadLibrary("steamclient64.dll")`s Valve's 25 MB DLL
 *      and CreateInterface's `SteamClient020`).
 *   4. The two patched slots use the SAME `ISteamMatchmaking` /
 *      `Servers` SDK vtable layout regardless of who created them
 *      (Valve maintains stable ABI for interface versions), so
 *      Forest's downstream `CreateLobby / RequestLobbyList /
 *      RequestInternetServerList` etc. dispatch correctly through
 *      Valve's CM connection.
 *
 * Why vtable-patch instead of returning a proxy object:
 *   - A proxy with a parallel vtable that forwards most slots to gbe
 *     requires translating `this` on every call (proxy_this →
 *     gbe_this) AND a thunk per slot. ~50 slots, error-prone.
 *   - Hooking gbe's vtable in place keeps `this` correct for every
 *     non-patched slot. Only slots 10/11 are diverted, and they
 *     ignore `this` anyway (just return the cached Valve interface
 *     pointer).
 *   - VirtualProtect lets us flip page protection on the gbe vtable
 *     long enough to write two pointers.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define WN_STEAMAPI_EXPORT __declspec(dllexport)

static HMODULE g_wine_bridge = NULL;
static HMODULE g_gbe = NULL;
static int32_t g_pipe = 0;
static int32_t g_user = 0;
static int g_inited = 0;
static void* g_steam_client = NULL;

typedef void* (*CreateInterface_fn)(const char*, int*);
static CreateInterface_fn g_CreateInterface = NULL;

static void wnb_log(const char* msg) {
    FILE* f = fopen("C:\\wnb.log", "a");
    if (f) { fputs(msg, f); fputs("\n", f); fclose(f); }
}

/* Returns 1 if wine bridge ISteamClient is available. Doesn't call
 * the vtable's CreateSteamPipe/ConnectToGlobalUser — those crashed
 * Forest at 0xC0000005 (kernelbase unwind) because Valve's libsteamclient.so
 * needs Steam_StartThread + Steam_CreateGlobalUser bootstrap before its
 * vtable methods are safe to invoke, and we don't do that bootstrap
 * inside the wine guest. Leaving pipe/user at the gbe defaults — Forest's
 * matchmaking calls will pass those to the wine bridge, which may or
 * may not work depending on whether the wine bridge tolerates unknown
 * pipe/user values. See [[project-bionic-use-valve-lsclient]]. */
static int load_wine_bridge(void) {
    if (g_wine_bridge != NULL) return g_steam_client != NULL;
    wnb_log("[lifecycle] load_wine_bridge: LoadLibrary(steamclient64.dll)");
    g_wine_bridge = LoadLibraryA("steamclient64.dll");
    if (!g_wine_bridge) {
        wnb_log("[lifecycle] LoadLibrary(steamclient64.dll) FAILED");
        return 0;
    }
    wnb_log("[lifecycle] load_wine_bridge: GetProcAddress(CreateInterface)");
    g_CreateInterface = (CreateInterface_fn)GetProcAddress(g_wine_bridge, "CreateInterface");
    if (!g_CreateInterface) {
        wnb_log("[lifecycle] steamclient64.dll missing CreateInterface");
        return 0;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "[lifecycle] CreateInterface fn=%p — calling SteamClient020",
             (void*)g_CreateInterface);
    wnb_log(buf);
    int code = 0;
    g_steam_client = g_CreateInterface("SteamClient020", &code);
    snprintf(buf, sizeof(buf), "[lifecycle] CreateInterface(SteamClient020) -> %p code=%d",
             g_steam_client, code);
    wnb_log(buf);
    if (!g_steam_client) {
        g_steam_client = g_CreateInterface("SteamClient019", &code);
        snprintf(buf, sizeof(buf), "[lifecycle] CreateInterface(SteamClient019) -> %p", g_steam_client);
        wnb_log(buf);
    }
    if (!g_steam_client) {
        g_steam_client = g_CreateInterface("SteamClient017", &code);
        snprintf(buf, sizeof(buf), "[lifecycle] CreateInterface(SteamClient017) -> %p", g_steam_client);
        wnb_log(buf);
    }
    if (!g_steam_client) {
        wnb_log("[lifecycle] all CreateInterface attempts NULL");
        return 0;
    }
    /* Pipe/user remain whatever gbe set (we'll add gbe-handle queries
     * in a follow-up if needed). Don't call vtable slot 0/2 here — that
     * crashed previously. */
    snprintf(buf, sizeof(buf),
             "[lifecycle] wine bridge init OK: client=%p (skipping CreateSteamPipe/ConnectToGlobalUser)",
             g_steam_client);
    wnb_log(buf);
    return 1;
}

static int gbe_init(void) {
    if (g_gbe != NULL) return 1;
    g_gbe = LoadLibraryA("original_steam_api64.dll");
    if (!g_gbe) {
        wnb_log("[lifecycle] LoadLibrary(original_steam_api64.dll) FAILED");
        return 0;
    }
    typedef int (*Init_fn)(void);
    Init_fn p = (Init_fn)GetProcAddress(g_gbe, "SteamAPI_Init");
    if (p) {
        int rc = p();
        char log[80];
        snprintf(log, sizeof(log), "[lifecycle] gbe SteamAPI_Init -> %d", rc);
        wnb_log(log);
        return rc;
    }
    return 0;
}

WN_STEAMAPI_EXPORT int SteamAPI_Init(void) {
    if (g_inited) return 1;
    wnb_log("[lifecycle] SteamAPI_Init called");
    /* Initialize gbe_fork first — it backs every non-matchmaking forwarded
     * export. Skipping it would crash gbe's flat-C calls (friends, cloud,
     * userstats, etc.) that access its internal state. */
    gbe_init();
    /* Wine PE bridge CreateInterface crashes inside the wine guest because
     * Valve's libsteamclient.so requires Steam_StartThread + Steam_CreateGlobalUser
     * bootstrap, which only runs in the Android-side process via WnSteamBoot.
     * Inside wine we can't dlsym those bionic symbols directly. Until a wine-
     * guest-safe bootstrap exists, leave the wine bridge unloaded; Forest will
     * use gbe's SteamClient via the fallback in our SteamClient() override.
     * See [[project-bridge-cmclient-gap]] for the routing.
     *
     *     load_wine_bridge();  // crashes Forest at CreateInterface */
    g_inited = 1;
    return 1;
}

WN_STEAMAPI_EXPORT int SteamAPI_InitSafe(void) { return SteamAPI_Init(); }

WN_STEAMAPI_EXPORT int SteamAPI_InitFlat(void* p_outErrMsg) {
    (void)p_outErrMsg;
    return SteamAPI_Init() ? 0 : 2;
}

WN_STEAMAPI_EXPORT void SteamAPI_Shutdown(void) {
    wnb_log("[lifecycle] SteamAPI_Shutdown");
    if (g_gbe) {
        typedef void (*Sht_fn)(void);
        Sht_fn p = (Sht_fn)GetProcAddress(g_gbe, "SteamAPI_Shutdown");
        if (p) p();
    }
    /* Don't unload wine bridge — its teardown isn't safe from a guest
     * game's main thread once libsteamclient.so has a CM connection. */
    g_inited = 0;
}

WN_STEAMAPI_EXPORT int SteamAPI_IsSteamRunning(void) { return 1; }

WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamPipe(void) {
    if (!g_inited) SteamAPI_Init();
    if (g_pipe != 0) return g_pipe;
    /* Fall back to gbe's pipe — we don't bootstrap the wine bridge's
     * pipe/user safely yet, so let gbe satisfy the SDK contract. */
    if (g_gbe) {
        typedef int (*P_fn)(void);
        P_fn p = (P_fn)GetProcAddress(g_gbe, "SteamAPI_GetHSteamPipe");
        if (p) return p();
    }
    return 0;
}

WN_STEAMAPI_EXPORT int SteamAPI_GetHSteamUser(void) {
    if (!g_inited) SteamAPI_Init();
    if (g_user != 0) return g_user;
    if (g_gbe) {
        typedef int (*P_fn)(void);
        P_fn p = (P_fn)GetProcAddress(g_gbe, "SteamAPI_GetHSteamUser");
        if (p) return p();
    }
    return 0;
}

WN_STEAMAPI_EXPORT int SteamAPI_RestartAppIfNecessary(uint32_t unOwnAppID) {
    (void)unOwnAppID;
    return 0;
}

/* Forward declarations — defined in steam_api_bridge_overrides.c.
 * Steam Launcher: these LoadLibrary("steamclient64.dll") + CreateInterface +
 * vtable[10/11] dispatch against VALVE's real Windows DLL (staged at
 * C:\Program Files (x86)\Steam by the Steam Launcher asset stage). The DLL is
 * shared across this game process and the wn-steam-launcher.exe
 * sibling process via Valve's normal `\BaseNamedObjects\STEAM_*` IPC
 * — the launcher's Steam_CreateGlobalUser registers the slot, our
 * `GetISteamMatchmaking` call here connects as a client of it. */
extern void* get_our_matchmaking(void);
extern void* get_our_matchmaking_servers(void);

/* Vtable thunks injected into gbe's `ISteamClient` vtable slots 10
 * + 11 (see hook_gbe_matchmaking_slots). The args (hUser, hPipe,
 * pchVersion) are what gbe-or-CSteamworks passes; we ignore them and
 * return Valve's matchmaking pointer. The cached pointer in
 * get_our_matchmaking() is resolved through Valve's vtable[10] using
 * real handles from Steam_CreateGlobalUser/CreateSteamPipe — done
 * inside that resolver. */
static void* WINAPI thunk_GetISteamMatchmaking(
        void* self, int hSteamUser, int hSteamPipe, const char* pchVersion) {
    (void)self; (void)hSteamUser; (void)hSteamPipe; (void)pchVersion;
    static int logged = 0;
    if (!logged) {
        wnb_log("[lifecycle] vtable thunk: GetISteamMatchmaking -> Valve client");
        logged = 1;
    }
    return get_our_matchmaking();
}

static void* WINAPI thunk_GetISteamMatchmakingServers(
        void* self, int hSteamUser, int hSteamPipe, const char* pchVersion) {
    (void)self; (void)hSteamUser; (void)hSteamPipe; (void)pchVersion;
    static int logged = 0;
    if (!logged) {
        wnb_log("[lifecycle] vtable thunk: GetISteamMatchmakingServers -> Valve client");
        logged = 1;
    }
    return get_our_matchmaking_servers();
}

/* Patch gbe's ISteamClient vtable in place: replace slots 10 + 11.
 * Idempotent — fires once per process. Saves the originals in case
 * we ever need to revert (we don't, but the locals make the hook
 * inspectable in a debugger). */
static void hook_gbe_matchmaking_slots(void* gbe_client) {
    static int hooked = 0;
    if (hooked || gbe_client == NULL) return;

    void** vt = *(void***)gbe_client;
    void* orig10 = vt[10];
    void* orig11 = vt[11];

    DWORD old_prot = 0;
    if (!VirtualProtect(&vt[10], sizeof(void*) * 2,
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        wnb_log("[lifecycle] hook: VirtualProtect WRITE failed; cannot redirect matchmaking");
        return;
    }
    vt[10] = (void*)thunk_GetISteamMatchmaking;
    vt[11] = (void*)thunk_GetISteamMatchmakingServers;
    DWORD restored_prot = 0;
    VirtualProtect(&vt[10], sizeof(void*) * 2, old_prot, &restored_prot);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[lifecycle] hook: patched gbe ISteamClient vt[10] (was %p -> %p) "
             "vt[11] (was %p -> %p)",
             orig10, vt[10], orig11, vt[11]);
    wnb_log(buf);
    hooked = 1;
}

WN_STEAMAPI_EXPORT void* SteamClient(void) {
    if (!g_inited) SteamAPI_Init();

    /* Steam Launcher path: return gbe's real ISteamClient but with vt[10]/[11]
     * runtime-patched to route through Valve's real matchmaking. */
    if (g_gbe) {
        typedef void* (*SC_fn)(void);
        SC_fn p = (SC_fn)GetProcAddress(g_gbe, "SteamClient");
        if (p) {
            void* gbe_client = p();
            hook_gbe_matchmaking_slots(gbe_client);  /* one-shot */
            static int logged = 0;
            if (!logged) {
                wnb_log("[lifecycle] SteamClient() -> gbe (matchmaking slots patched to Valve)");
                logged = 1;
            }
            return gbe_client;
        }
    }
    return NULL;
}
