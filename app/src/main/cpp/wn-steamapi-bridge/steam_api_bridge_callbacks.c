/* Callback lifecycle pass-throughs for the SteamAPI flat-C surface.
 *
 * Task #163 foundation: SteamAPI_RegisterCallback / Unregister /
 * RegisterCallResult / UnregisterCallResult / RunCallbacks are
 * carved out of the PE forwards to gbe_fork (see gen_forward_def.py
 * OVERRIDE_NAMES) so we can layer in dual-dispatch incrementally.
 *
 * THIS ITERATION: pass-through only. Every call goes straight to
 * gbe_fork's same-named export (resolved via LoadLibrary +
 * GetProcAddress on original_steam_api64.dll). No regression.
 *
 * NEXT ITERATIONS: extend RunCallbacks to also drain our
 * libsteamclient.so's callback queue, and route those callbacks
 * into the listeners registered via SteamAPI_RegisterCallback /
 * RegisterCallResult so matchmaking responses (LobbyMatchList_t,
 * etc.) actually reach the game's CCallback objects.
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define WN_STEAMAPI_EXPORT __declspec(dllexport)

/* Forward declarations — wnb_publish_dispatch_pointers takes the
 * addresses of the dispatch functions defined further down.
 *
 * Dispatch ABI carries an explicit payload size because the caller
 * (cm_bridge.cpp inside libwnsteam.so, same wine process) is the one
 * who knows what callback type it's emitting. We stash the payload
 * bytes inline in the pending table when no listener is registered
 * yet, so a late RegisterCallResult can still receive the data. */
__attribute__((visibility("default")))
void wnb_dispatch_callback(int iCallback, const void* data, size_t data_size);
__attribute__((visibility("default")))
void wnb_dispatch_call_result(uint64_t hAPICall, int io_failure,
                              const void* data, size_t data_size);

/* Bounded local registry — game CCallback/CCallResult listeners we
 * see registered via our SteamAPI_RegisterCallback override. Stored
 * here so a future iteration can dispatch libsteamclient.so callbacks
 * (LobbyMatchList_t etc.) directly to them, bypassing the gbe_fork
 * queue that doesn't know about our cross-process state. */
#define WNB_MAX_LISTENERS 64

typedef struct {
    void*    callback;   /* CCallbackBase* */
    int      iCallback;  /* for CCallback path */
    uint64_t hAPICall;   /* for CCallResult path (0 = CCallback) */
} WnbListener;

static WnbListener g_listeners[WNB_MAX_LISTENERS];
static CRITICAL_SECTION g_listeners_cs;
static int g_listeners_inited = 0;

/* Late-bind pending-result table. wnb_dispatch_call_result blocks
 * synchronously inside RequestLobbyList (etc.) BEFORE the game has a
 * chance to register its CCallResult listener — the universal SDK
 * pattern is "hCall = RequestLobbyList(); result.Set(hCall, …)" so
 * the listener arrives AFTER our dispatch. We stash the payload here
 * keyed by hCall; SteamAPI_RegisterCallResult drains any matching
 * entry immediately after the listener lands.
 *
 * Sized for ~64 in-flight unmatched results (worst case if a game
 * fires many CallResults before registering listeners). Payload
 * capped at 256 bytes — exceeds every shipping Steam CallResult
 * struct (LobbyMatchList_t=4, LobbyEnter_t=16, CreateItemResult_t=24,
 * GlobalAchievementPercentagesReady_t=8, …). */
#define WNB_MAX_PENDING  64
#define WNB_MAX_PAYLOAD  256

typedef struct {
    uint64_t hAPICall;             /* 0 = empty slot */
    int      io_failure;
    size_t   payload_size;
    uint8_t  payload[WNB_MAX_PAYLOAD];
} WnbPendingResult;

static WnbPendingResult g_pending[WNB_MAX_PENDING];

static void listeners_init(void) {
    if (g_listeners_inited) return;
    InitializeCriticalSection(&g_listeners_cs);
    g_listeners_inited = 1;
}

static void listeners_add(void* pCallback, int iCallback, uint64_t hAPICall) {
    if (!pCallback) return;
    listeners_init();
    EnterCriticalSection(&g_listeners_cs);
    for (int i = 0; i < WNB_MAX_LISTENERS; ++i) {
        if (g_listeners[i].callback == NULL) {
            g_listeners[i].callback  = pCallback;
            g_listeners[i].iCallback = iCallback;
            g_listeners[i].hAPICall  = hAPICall;
            break;
        }
    }
    LeaveCriticalSection(&g_listeners_cs);
}

static void listeners_remove(void* pCallback) {
    if (!pCallback) return;
    listeners_init();
    EnterCriticalSection(&g_listeners_cs);
    for (int i = 0; i < WNB_MAX_LISTENERS; ++i) {
        if (g_listeners[i].callback == pCallback) {
            g_listeners[i].callback = NULL;
        }
    }
    LeaveCriticalSection(&g_listeners_cs);
}

/* Lazy resolver for original_steam_api64.dll (gbe_fork). The PE
 * forwarders auto-load the module at first GetProcAddress, but our
 * direct dlsym path here loads it explicitly. Cached across calls. */
typedef void (*VoidFn)(void);
typedef void (*RegisterFn)(void* pCallback, int iCallback);
typedef void (*UnregisterFn)(void* pCallback);
typedef void (*RegisterResultFn)(void* pCallback, uint64_t hAPICall);
typedef void (*UnregisterResultFn)(void* pCallback, uint64_t hAPICall);

static HMODULE g_gbe_fork = NULL;
static RegisterFn         g_gbe_register_callback     = NULL;
static UnregisterFn       g_gbe_unregister_callback   = NULL;
static RegisterResultFn   g_gbe_register_call_result   = NULL;
static UnregisterResultFn g_gbe_unregister_call_result = NULL;
static VoidFn             g_gbe_run_callbacks         = NULL;

static void resolve_gbe_fork(void) {
    if (g_gbe_fork != NULL) return;
    g_gbe_fork = LoadLibraryA("original_steam_api64.dll");
    if (g_gbe_fork == NULL) {
        OutputDebugStringA(
            "[wnb-callbacks] LoadLibrary(original_steam_api64.dll) failed");
        return;
    }
    g_gbe_register_callback =
        (RegisterFn)GetProcAddress(g_gbe_fork, "SteamAPI_RegisterCallback");
    g_gbe_unregister_callback =
        (UnregisterFn)GetProcAddress(g_gbe_fork, "SteamAPI_UnregisterCallback");
    g_gbe_register_call_result =
        (RegisterResultFn)GetProcAddress(g_gbe_fork, "SteamAPI_RegisterCallResult");
    g_gbe_unregister_call_result =
        (UnregisterResultFn)GetProcAddress(g_gbe_fork, "SteamAPI_UnregisterCallResult");
    g_gbe_run_callbacks =
        (VoidFn)GetProcAddress(g_gbe_fork, "SteamAPI_RunCallbacks");
}

WN_STEAMAPI_EXPORT void SteamAPI_RegisterCallback(void* pCallback, int iCallback) {
    resolve_gbe_fork();
    if (g_gbe_register_callback != NULL) {
        g_gbe_register_callback(pCallback, iCallback);
    }
    listeners_add(pCallback, iCallback, /*hAPICall=*/0);
}

WN_STEAMAPI_EXPORT void SteamAPI_UnregisterCallback(void* pCallback) {
    resolve_gbe_fork();
    if (g_gbe_unregister_callback != NULL) {
        g_gbe_unregister_callback(pCallback);
    }
    listeners_remove(pCallback);
}

/* Drain any pending result that arrived for `hAPICall` before this
 * register call. Invokes vt[1] CCallResult::Run on the listener and
 * clears the pending slot. The whole reason this exists: our wine-
 * side synchronous file-IPC fulfills RequestLobbyList INSIDE the
 * call's stack frame, BEFORE the game registers its CCallResult
 * (universal pattern: `h = RequestLobbyList(); result.Set(h, …)`).
 * Without this drain, every CCallResult fires-and-forgets into the
 * void. */
static void drain_pending_for(void* pCallback, uint64_t hAPICall) {
    if (!pCallback || hAPICall == 0) return;
    listeners_init();
    uint8_t  payload[WNB_MAX_PAYLOAD];
    size_t   payload_size = 0;
    int      io_failure = 0;
    int      found = 0;
    EnterCriticalSection(&g_listeners_cs);
    for (int i = 0; i < WNB_MAX_PENDING; ++i) {
        if (g_pending[i].hAPICall == hAPICall) {
            payload_size = g_pending[i].payload_size;
            if (payload_size > WNB_MAX_PAYLOAD) payload_size = WNB_MAX_PAYLOAD;
            memcpy(payload, g_pending[i].payload, payload_size);
            io_failure = g_pending[i].io_failure;
            g_pending[i].hAPICall = 0;
            g_pending[i].payload_size = 0;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_listeners_cs);
    if (!found) return;
    /* Clear the listeners entry too — CCallResult is single-shot;
     * after we invoke Run, the listener has been delivered. */
    listeners_remove(pCallback);
    typedef void (*RunResultFn)(void* /*this*/, void* /*pvParam*/, int /*bIOFailure*/);
    void** vtable = *(void***)pCallback;
    RunResultFn run = (RunResultFn)vtable[1];
    run(pCallback, payload, io_failure);
}

WN_STEAMAPI_EXPORT void SteamAPI_RegisterCallResult(void* pCallback, uint64_t hAPICall) {
    resolve_gbe_fork();
    if (g_gbe_register_call_result != NULL) {
        g_gbe_register_call_result(pCallback, hAPICall);
    }
    listeners_add(pCallback, /*iCallback=*/0, hAPICall);
    /* Late-bind: if dispatch already fired for this hCall (the typical
     * synchronous-block case), deliver it now. */
    drain_pending_for(pCallback, hAPICall);
}

WN_STEAMAPI_EXPORT void SteamAPI_UnregisterCallResult(void* pCallback, uint64_t hAPICall) {
    resolve_gbe_fork();
    if (g_gbe_unregister_call_result != NULL) {
        g_gbe_unregister_call_result(pCallback, hAPICall);
    }
    listeners_remove(pCallback);
}

/* Internal dispatch hook the bridge's matchmaking-response code path
 * can call to fan out a Callback_t to any registered CCallback whose
 * iCallback matches. CCallbackBase::Run is vt[0] in MSVC ABI:
 *   void Run(void* pvParam) — virtual
 * Future-call site: the wine-side cross-process-IPC reader (task #164,
 * cm_bridge::try_lobby_list_from_file). For now this is unreferenced
 * but exported so future iterations can dlsym it from libsteamclient.so
 * after we wire that path. Not in the .def — internal-only. */
/* Publish the dispatch function addresses to a known file so the
 * companion libsteamclient.so (in the same wine process but a
 * separate Linux ELF module) can invoke them. Same address space so
 * raw function pointer values are valid across the module boundary.
 *
 * Called once per process by get_our_matchmaking() (in
 * steam_api_bridge_overrides.c) the first time the game asks for the
 * matchmaking interface. Path matches cm_bridge.cpp's wn_state_dir():
 * default /tmp/, override via WN_STATE_DIR env (set by app process's
 * nativeInit when the wine process inherits env). */
__attribute__((visibility("default")))
void wnb_publish_dispatch_pointers(void) {
    const char* dir = getenv("WN_STATE_DIR");
    if (!dir || !*dir) dir = "/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/wnb_ptrs.txt", dir);
    FILE* f = fopen(path, "w");
    if (!f) {
        OutputDebugStringA("[wnb-callbacks] publish_dispatch_pointers: fopen failed");
        return;
    }
    /* Function pointer addresses as decimal — cm_bridge parses with stoull. */
    fprintf(f, "dispatch_callback %llu\n",
            (unsigned long long)(uintptr_t)wnb_dispatch_callback);
    fprintf(f, "dispatch_call_result %llu\n",
            (unsigned long long)(uintptr_t)wnb_dispatch_call_result);
    fclose(f);
}

__attribute__((visibility("default")))
void wnb_dispatch_callback(int iCallback, const void* data, size_t data_size) {
    (void)data_size;  /* CCallback fan-out doesn't need late-bind; the
                       * SDK contract is that Callback<T> objects live
                       * for the whole period the game wants events. */
    listeners_init();
    EnterCriticalSection(&g_listeners_cs);
    /* Snapshot first to release the lock before invoking — a callback's
     * Run() may register/unregister, which would deadlock under a held
     * lock. Bounded by WNB_MAX_LISTENERS so stack-alloc is fine. */
    void* matches[WNB_MAX_LISTENERS];
    int   n = 0;
    for (int i = 0; i < WNB_MAX_LISTENERS; ++i) {
        if (g_listeners[i].callback != NULL
                && g_listeners[i].hAPICall == 0
                && g_listeners[i].iCallback == iCallback) {
            matches[n++] = g_listeners[i].callback;
        }
    }
    LeaveCriticalSection(&g_listeners_cs);
    typedef void (*RunFn)(void* /*this*/, const void* /*pvParam*/);
    for (int i = 0; i < n; ++i) {
        void** vtable = *(void***)matches[i];
        RunFn run = (RunFn)vtable[0];
        run(matches[i], data);
    }
}

__attribute__((visibility("default")))
void wnb_dispatch_call_result(uint64_t hAPICall, int io_failure,
                              const void* data, size_t data_size) {
    listeners_init();
    EnterCriticalSection(&g_listeners_cs);
    void* matches[WNB_MAX_LISTENERS];
    int   n = 0;
    for (int i = 0; i < WNB_MAX_LISTENERS; ++i) {
        if (g_listeners[i].callback != NULL
                && g_listeners[i].hAPICall == hAPICall) {
            matches[n++] = g_listeners[i].callback;
            /* CCallResults are single-shot — clear after capturing. */
            g_listeners[i].callback = NULL;
        }
    }
    if (n == 0) {
        /* Late-bind path. No listener yet — the universal SDK pattern
         * has the game register only AFTER it receives hCall from the
         * API call, and our synchronous file-IPC fires INSIDE that
         * call. Stash the payload bytes inline; the matching
         * RegisterCallResult call drain_pending_for will fire it. */
        size_t copy = data_size > WNB_MAX_PAYLOAD ? WNB_MAX_PAYLOAD : data_size;
        for (int i = 0; i < WNB_MAX_PENDING; ++i) {
            if (g_pending[i].hAPICall == 0) {
                g_pending[i].hAPICall     = hAPICall;
                g_pending[i].io_failure   = io_failure;
                g_pending[i].payload_size = copy;
                if (data && copy) memcpy(g_pending[i].payload, data, copy);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_listeners_cs);
    /* CCallResult::Run signature: (pvParam, bIOFailure). vt[1] per SDK. */
    typedef void (*RunResultFn)(void* /*this*/, const void* /*pvParam*/, int /*bIOFailure*/);
    for (int i = 0; i < n; ++i) {
        void** vtable = *(void***)matches[i];
        RunResultFn run = (RunResultFn)vtable[1];
        run(matches[i], data, io_failure);
    }
}

/* Steam Launcher — drains Valve's real steamclient64.dll callback queue and
 * fans events out to the game's CCallback / CCallResult listeners.
 * Defined in steam_api_bridge_overrides.c (it owns the Valve
 * ISteamClient handle + pipe). No-op until the game has asked for the
 * matchmaking interface at least once (which runs the resolver). */
extern void wnb_pump_valve_callbacks(void);

WN_STEAMAPI_EXPORT void SteamAPI_RunCallbacks(void) {
    resolve_gbe_fork();
    /* gbe_fork first — it backs friends / userstats / cloud / the
     * game's own non-matchmaking callbacks. */
    if (g_gbe_run_callbacks != NULL) {
        g_gbe_run_callbacks();
    }
    /* Steam Launcher: then drain Valve's pipe. RequestLobbyList /
     * RequestInternetServerList results (LobbyMatchList_t, server
     * rows, LobbyDataUpdate_t, …) arrive here — without this pump the
     * server browser stays empty even though the request reached
     * Valve's CMs. */
    wnb_pump_valve_callbacks();
}
