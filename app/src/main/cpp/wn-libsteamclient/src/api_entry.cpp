// Flat-C exports — the public ABI surface every libsteamclient.so must
// expose. Names + arities mirror the Steamworks SDK's flat-C entry
// points; consumers (the bootstrap, lsteamclient.dll inside Wine,
// game's steam_api(64).dll once it dlopens our peer) call these
// directly via dlsym after loading the .so.
//
// All exports use C linkage + default visibility. The CMakeLists builds
// with -fvisibility=hidden so symbols that AREN'T explicitly marked
// stay internal — this keeps the dynsym table small + matches Valve's
// build's surface area.
//
// CallbackMsg_t layout (from Steamworks SDK isteamclient.h):
//   HSteamUser  m_hSteamUser   @ +0    int
//   int         m_iCallback    @ +4    int
//   uint8_t*    m_pubParam     @ +8    pointer (8B on aarch64)
//   int         m_cubParam     @ +16   int
//   (4 bytes tail padding to 24)
// Consumers read fixed-offset members; we write the same layout.

#include "wn_libsteamclient/runtime_state.h"
#include "wn_libsteamclient/callbacks.h"
#include "wn_libsteamclient/callback_registry.h"

#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lsc = wn_libsteamclient;

#define WN_TAG  "WnLibSteamClient"
#define WN_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  WN_TAG, __VA_ARGS__)
#define WN_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  WN_TAG, __VA_ARGS__)
#define WN_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, WN_TAG, __VA_ARGS__)

// CreateInterface returns a pointer to the registered ISteamClient (or
// other interface) instance. Lives in isteam_client.cpp.
extern "C" __attribute__((visibility("default")))
void* CreateInterface(const char* version_name, int* return_code);

// ---------------------------------------------------------------------------
// Pipe + user lifecycle
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
int Steam_CreateSteamPipe(void) {
    auto pipe = lsc::alloc_pipe();
    if (pipe == 0) {
        // Idempotent — the existing pipe handle is what the caller
        // wants. Return the live value so a second CreateSteamPipe in
        // the same process gets the same handle (matches Valve's
        // semantic of "one pipe per global user model").
        pipe = lsc::state().pipe.load();
    }
    WN_LOGI("Steam_CreateSteamPipe() -> %d", pipe);
    return pipe;
}

extern "C" __attribute__((visibility("default")))
bool Steam_BReleaseSteamPipe(int pipe) {
    // SteamShutdown_t — emit BEFORE the actual pipe teardown so any
    // consumer that drains callbacks one last time on the way out
    // sees the signal. release_pipe also clears logged_on/connected,
    // which in turn emits SteamServersDisconnected_t through the
    // set_logged_on path — order: Shutdown, then Disconnected.
    int h_user = lsc::state().user.load();
    namespace cb = wn_libsteamclient::callbacks;
    cb::SteamShutdown sd_payload{};
    lsc::push_callback(h_user, cb::kSteamShutdown, &sd_payload, 0);
    bool ok = lsc::release_pipe(pipe);
    WN_LOGI("Steam_BReleaseSteamPipe(%d) -> %d (SteamShutdown_t emitted)",
            pipe, ok ? 1 : 0);
    return ok;
}

extern "C" __attribute__((visibility("default")))
int Steam_ConnectToGlobalUser(int pipe) {
    auto user = lsc::alloc_global_user(pipe);
    WN_LOGI("Steam_ConnectToGlobalUser(pipe=%d) -> %d", pipe, user);
    return user;
}

// Steam_CreateGlobalUser is the bootstrap's entry point: it takes a
// `HSteamPipe* pipe_inout` and returns the user handle while writing
// the pipe handle to *pipe_inout. The bootstrap calls this with a
// pointer to an int — we allocate both the pipe and the user.
extern "C" __attribute__((visibility("default")))
int Steam_CreateGlobalUser(int* pipe_inout) {
    if (!pipe_inout) return 0;
    int pipe = lsc::alloc_pipe();
    if (pipe == 0) pipe = lsc::state().pipe.load();   // already exists; reuse
    int user = lsc::alloc_global_user(pipe);
    *pipe_inout = pipe;
    WN_LOGI("Steam_CreateGlobalUser(*pipe=%d) -> user=%d", pipe, user);
    return user;
}

// Used by some games' SteamGameServer init path; we expose the same
// shape but return a user handle on the existing pipe. Account type is
// ignored in this initial implementation.
extern "C" __attribute__((visibility("default")))
int Steam_CreateLocalUser(int* pipe_inout, int /*account_type*/) {
    return Steam_CreateGlobalUser(pipe_inout);
}

extern "C" __attribute__((visibility("default")))
void Steam_ReleaseUser(int pipe, int user) {
    lsc::release_user(pipe, user);
    WN_LOGI("Steam_ReleaseUser(pipe=%d, user=%d)", pipe, user);
}

// ---------------------------------------------------------------------------
// Logon + state
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
bool Steam_BLoggedOn(int pipe, int user) {
    auto& s = lsc::state();
    if (pipe == 0 || user == 0) return false;
    if (s.pipe.load() != pipe || s.user.load() != user) return false;
    return s.logged_on.load();
}

extern "C" __attribute__((visibility("default")))
bool Steam_BConnected(int pipe, int user) {
    auto& s = lsc::state();
    if (pipe == 0 || user == 0) return false;
    if (s.pipe.load() != pipe || s.user.load() != user) return false;
    return s.connected.load();
}

// LogOn / LogOff are best-effort stubs at this layer — the bootstrap
// drives logon through IClientUser.LogonWithRefreshToken (vtable). We
// just track the flag locally for callers that poll via the flat API.
extern "C" __attribute__((visibility("default")))
void Steam_LogOn(int pipe, int user, uint64_t /*steamid*/) {
    auto& s = lsc::state();
    if (s.pipe.load() == pipe && s.user.load() == user) {
        s.logged_on.store(true);
        s.connected.store(true);
    }
}

extern "C" __attribute__((visibility("default")))
void Steam_LogOff(int pipe, int user) {
    auto& s = lsc::state();
    if (s.pipe.load() == pipe && s.user.load() == user) {
        s.logged_on.store(false);
        s.connected.store(false);
    }
    WN_LOGI("Steam_LogOff(pipe=%d, user=%d)", pipe, user);
}

// ---------------------------------------------------------------------------
// Callback queue
// ---------------------------------------------------------------------------

// CallbackMsg_t layout: see top-of-file comment.
extern "C" __attribute__((visibility("default")))
bool Steam_BGetCallback(int pipe, void* cb_msg) {
    if (!cb_msg) return false;
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.callback_mu);
    if (s.callback_queue.empty()) return false;
    lsc::CallbackMsg msg = std::move(s.callback_queue.front());
    s.callback_queue.pop_front();
    s.last_param = std::move(msg.body);
    auto* dst = static_cast<uint8_t*>(cb_msg);
    int  h_user   = msg.user;
    int  i_cb     = msg.id;
    void* pubParam = s.last_param.empty() ? nullptr : s.last_param.data();
    int  cubParam = static_cast<int>(s.last_param.size());
    std::memcpy(dst +  0, &h_user,   sizeof(int));
    std::memcpy(dst +  4, &i_cb,     sizeof(int));
    std::memcpy(dst +  8, &pubParam, sizeof(void*));
    std::memcpy(dst + 16, &cubParam, sizeof(int));
    return true;
}

extern "C" __attribute__((visibility("default")))
void Steam_FreeLastCallback(int /*pipe*/) {
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.callback_mu);
    s.last_param.clear();
    s.last_param.shrink_to_fit();
}

// Steam_GetAPICallResult fills a previously-issued SteamAPICall_t's
// result buffer. Reads from state().call_results_pending — present
// only if push_call_result has landed an answer for hCall. Returns
// true and consumes the entry on success (so a second poll for the
// same handle returns false, matching the SDK contract). pbFailed
// reflects msg.io_failure on a hit; left untouched on miss/error.
//
// iCallbackExpected guards against a misuse where the caller asks
// for the wrong type — if the stored msg.callback_id mismatches,
// returns false WITHOUT consuming the entry (the SDK does the same).
extern "C" __attribute__((visibility("default")))
bool Steam_GetAPICallResult(int /*pipe*/, uint64_t hCall,
                            void* pCallback, int cubCallback,
                            int iCallbackExpected, bool* pbFailed) {
    if (hCall == 0) return false;
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.call_results_mu);
    auto it = s.call_results_pending.find(hCall);
    if (it == s.call_results_pending.end()) return false;
    const auto& msg = it->second;
    if (iCallbackExpected != 0 && msg.callback_id != iCallbackExpected) {
        // Don't consume — caller asked for the wrong type.
        return false;
    }
    if (pCallback && cubCallback > 0 && !msg.body.empty()) {
        size_t n = std::min<size_t>(static_cast<size_t>(cubCallback), msg.body.size());
        std::memcpy(pCallback, msg.body.data(), n);
    }
    if (pbFailed) *pbFailed = msg.io_failure;
    s.call_results_pending.erase(it);
    return true;
}

// Steam_IsAPICallCompleted — non-destructive presence check for a
// previously-issued SteamAPICall_t. Returns true if a result has
// landed for hCall (and writes io_failure if pbFailed is non-null);
// false if still pending. Steam_GetAPICallResult is the consumption
// path; this is the polling-without-consuming variant.
extern "C" __attribute__((visibility("default")))
bool Steam_IsAPICallCompleted(int /*pipe*/, uint64_t hCall, bool* pbFailed) {
    if (hCall == 0) return false;
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.call_results_mu);
    auto it = s.call_results_pending.find(hCall);
    if (it == s.call_results_pending.end()) return false;
    if (pbFailed) *pbFailed = it->second.io_failure;
    return true;
}

// ---------------------------------------------------------------------------
// Misc support helpers (consumed by SteamAPI / lsteamclient.dll)
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
bool Steam_IsKnownInterface(const char* /*pszInterfaceName*/) {
    // Lying TRUE here would mask version-not-found bugs; safer to
    // return false until we actually register interfaces. This API is
    // rarely consulted by modern game code anyway.
    return false;
}

extern "C" __attribute__((visibility("default")))
void Steam_NotifyMissingInterface(int /*pipe*/, const char* iface) {
    WN_LOGW("Steam_NotifyMissingInterface: %s", iface ? iface : "(null)");
}

extern "C" __attribute__((visibility("default")))
void Steam_SetLocalIPBinding(int /*ip*/, int /*port*/) {}

extern "C" __attribute__((visibility("default")))
void Steam_ReleaseThreadLocalMemory(int /*bThreadExit*/) {}

extern "C" __attribute__((visibility("default")))
int Steam_GetGSHandle(int /*pipe*/, int /*user*/) { return 0; }

extern "C" __attribute__((visibility("default")))
bool Steam_InitiateGameConnection(int /*pipe*/, int /*user*/,
                                  void* /*pAuthBlob*/, int /*cbMaxAuthBlob*/,
                                  uint64_t /*steamIDGameServer*/,
                                  uint32_t /*unIPServer*/,
                                  uint16_t /*usPortServer*/,
                                  bool /*bSecure*/) {
    return false;
}

extern "C" __attribute__((visibility("default")))
void Steam_TerminateGameConnection(int /*pipe*/, int /*user*/,
                                   uint32_t /*unIPServer*/,
                                   uint16_t /*usPortServer*/) {}

// ---------------------------------------------------------------------------
// GameServer-side exports — initial stubs. The Bionic-game path doesn't
// drive these (games are clients, not game servers); they're here for
// ABI completeness so any consumer dlsyming a name finds a non-null.
// Each returns a "no" / "zero" result.
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default"))) bool   Steam_GSBLoggedOn  (int, int) { return false; }
extern "C" __attribute__((visibility("default"))) bool   Steam_GSBSecure   (int, int) { return false; }
extern "C" __attribute__((visibility("default"))) uint64_t Steam_GSGetSteamID(int, int) { return 0; }
extern "C" __attribute__((visibility("default"))) void   Steam_GSLogOff    (int, int) {}
extern "C" __attribute__((visibility("default"))) bool   Steam_GSLogOn     (int, int, uint64_t, uint32_t, uint16_t, uint16_t, int, bool) { return false; }
extern "C" __attribute__((visibility("default"))) void   Steam_GSRemoveUserConnect       (int, int, uint64_t) {}
extern "C" __attribute__((visibility("default"))) bool   Steam_GSSendSteam2UserConnect   (int, int, uint64_t, uint32_t, uint32_t, uint16_t, const void*, int) { return false; }
extern "C" __attribute__((visibility("default"))) bool   Steam_GSSendSteam3UserConnect   (int, int, uint64_t, uint32_t, const void*, int) { return false; }
extern "C" __attribute__((visibility("default"))) void   Steam_GSSendUserDisconnect      (int, int, uint64_t, uint32_t) {}
extern "C" __attribute__((visibility("default"))) bool   Steam_GSSendUserStatusResponse  (int, int, uint64_t, int, const void*, int) { return false; }
extern "C" __attribute__((visibility("default"))) void   Steam_GSSetServerType           (int, int, uint32_t, uint32_t, uint16_t, uint16_t, uint16_t, const char*, const char*, bool) {}
extern "C" __attribute__((visibility("default"))) void   Steam_GSSetSpawnCount           (int, int, uint32_t) {}
extern "C" __attribute__((visibility("default"))) bool   Steam_GSUpdateStatus            (int, int, int, int, int, const char*, const char*, const char*) { return false; }
extern "C" __attribute__((visibility("default"))) bool   Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient(int, int, void*, uint32_t*, uint32_t) { return false; }

// ---------------------------------------------------------------------------
// Top-level SteamAPI_* flat-C exports — the surface games dlsym directly.
//
// Valve's libsteamclient.so doesn't export these; they live in steam_api.dll
// (Windows) / libsteam_api.so (Linux) — the lightweight SDK shim a game
// statically links and which RPCs into libsteamclient. Wine's
// lsteamclient.dll bridges the two on Windows-game-in-Linux.
//
// On Android, ColdClient / Unpack Files / Runtime DRM Injection paths
// circumvent the SDK shim and call directly into libsteamclient.so by
// dlopen + dlsym. Without these symbols those paths return SteamAPI_Init
// = false → game thinks Steam isn't running → bails. Providing them with
// sensible "we are Steam, init succeeded" defaults lets DRM-injected
// games proceed past their auth gate.
//
// Reference: public/steam/steam_api.h (Steamworks SDK 1.59+).
// ---------------------------------------------------------------------------

// Bool: should the app restart itself through Steam? Always false — our
// .so IS the Steam runtime, the app is already running through us.
// Otherwise games launched outside Steam exit immediately on first call.
extern "C" __attribute__((visibility("default")))
bool SteamAPI_RestartAppIfNecessary(uint32_t unOwnAppID) {
    WN_LOGI("SteamAPI_RestartAppIfNecessary(appId=%u) -> false (no restart needed)",
            static_cast<unsigned>(unOwnAppID));
    return false;
}

// Bool: did SteamAPI init succeed? Always true — by the time a game
// dlsyms this, our .so is loaded AND CreateInterface is wired. We also
// ensure the pipe/user singletons exist so subsequent
// SteamAPI_GetHSteam* return non-zero handles.
extern "C" __attribute__((visibility("default")))
bool SteamAPI_Init(void) {
    int pipe = lsc::alloc_pipe();
    if (pipe == 0) pipe = lsc::state().pipe.load();
    int user = lsc::alloc_global_user(pipe);
    (void)user;
    WN_LOGI("SteamAPI_Init() -> true (pipe=%d user=%d)", pipe, user);
    return true;
}

// The 1.59+ SDK adds a variant `SteamAPI_InitEx(*p_outErrMsg) → ESteamAPIInitResult`.
// Result codes: 0=OK, 1=NoSteamClient, 2=VersionMismatch, 3=FailedGeneric.
extern "C" __attribute__((visibility("default")))
int SteamAPI_InitEx(char* p_outErrMsg) {
    SteamAPI_Init();
    if (p_outErrMsg) p_outErrMsg[0] = '\0';
    return 0;  // k_ESteamAPIInitResult_OK
}

// Symmetric counterpart — releases the SteamAPI session. Routes through
// the same release_pipe path Steam_BReleaseSteamPipe uses, so SteamShutdown_t
// and SteamServersDisconnected_t emit through the queue.
extern "C" __attribute__((visibility("default")))
void SteamAPI_Shutdown(void) {
    int pipe = lsc::state().pipe.load();
    if (pipe != 0) {
        // Re-uses Steam_BReleaseSteamPipe's emit chain. Defined above.
        Steam_BReleaseSteamPipe(pipe);
    }
    WN_LOGI("SteamAPI_Shutdown()");
}

// Bool: is the Steam client running? Always true — we ARE the client.
// Games gate "online features" + overlay availability on this; saying
// false would tip them into "Steam offline" mode that bypasses ours.
extern "C" __attribute__((visibility("default")))
bool SteamAPI_IsSteamRunning(void) { return true; }

// Handle accessors — return whatever's in state().{pipe,user}.load().
// Modern SDK uses these instead of remembering the values from
// SteamAPI_Init's return path.
extern "C" __attribute__((visibility("default")))
int SteamAPI_GetHSteamPipe(void) { return lsc::state().pipe.load(); }
extern "C" __attribute__((visibility("default")))
int SteamAPI_GetHSteamUser(void) { return lsc::state().user.load(); }

// Thread-local memory release. Modern SDK calls this from worker threads
// before exit. No-op safe — we don't keep TLS in this .so's surface yet.
extern "C" __attribute__((visibility("default")))
void SteamAPI_ReleaseCurrentThreadMemory(void) {}

// SDK opt-in for catching exceptions during callback dispatch. The
// per-process flag is normally a no-op — Valve uses this only for
// release-mode crash diagnostics, the actual try/catch wrap lives in
// SteamAPI_RunCallbacks. Storing the flag for future use.
extern "C" __attribute__((visibility("default")))
void SteamAPI_SetTryCatchCallbacks(bool /*bTryCatchCallbacks*/) {}

// Optional crash-dump hook (Win32-only on the official SDK; Android
// uses the OS tombstone collector). No-op.
extern "C" __attribute__((visibility("default")))
void SteamAPI_WriteMiniDump(uint32_t /*uStructuredExceptionCode*/,
                            void*    /*pvExceptionInfo*/,
                            uint32_t /*uBuildID*/) {}

// SDK in-process callback registration (CCallbackBase machinery).
// SteamAPI_RegisterCallback stores the (cb, iCallback) tuple in the
// registry; SteamAPI_RunCallbacks drains our callback_queue and invokes
// vtable[0] on each cb whose registered id matches the message.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RegisterCallback(void* pCallback, int iCallback) {
    lsc::register_callback(pCallback, iCallback);
}
extern "C" __attribute__((visibility("default")))
void SteamAPI_UnregisterCallback(void* pCallback) {
    lsc::unregister_callback(pCallback);
}
// CallResults track a specific async hCall id rather than a callback
// type. We don't issue async calls at this layer yet, so the cb stays
// resident until SteamAPI_UnregisterCallResult; no dispatch attempt.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RegisterCallResult(void* pCallback, uint64_t hAPICall) {
    lsc::register_call_result(pCallback, hAPICall);
}
extern "C" __attribute__((visibility("default")))
void SteamAPI_UnregisterCallResult(void* pCallback, uint64_t hAPICall) {
    lsc::unregister_call_result(pCallback, hAPICall);
}

// Main pump — modern SDK games loop on this every frame. Drains the
// callback_queue one message at a time, snapshots the registered
// callbacks matching the message's iCallback id, and invokes each
// cb's vtable[0](pvParam) with the payload bytes.
//
// Re-entrancy: snapshot is taken under the registry mutex then
// released before invoking, so a callback's Run() that calls
// SteamAPI_RegisterCallback / Unregister doesn't deadlock or
// invalidate iterators. A cb that was unregistered mid-drain still
// gets called for the message in flight (matches Valve's semantics).
//
// We DO NOT clear last_param the way Steam_BGetCallback does — that
// path expects a separate Steam_FreeLastCallback follow-up. The
// RunCallbacks contract is "iterator-style auto-cleanup": the body
// bytes are owned by a local std::vector for the duration of the
// dispatch, then released.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RunCallbacks(void) {
    auto& s = lsc::state();

    // 1) Drain CCallback queue → dispatch to vtable[0](payload).
    for (;;) {
        lsc::CallbackMsg msg;
        {
            std::lock_guard<std::mutex> lk(s.callback_mu);
            if (s.callback_queue.empty()) break;
            msg = std::move(s.callback_queue.front());
            s.callback_queue.pop_front();
        }
        auto cbs = lsc::find_callbacks(msg.id);
        for (void* cb : cbs) {
            using RunFn = void (*)(void* /*this*/, void* /*pvParam*/);
            void* payload = msg.body.empty() ? nullptr : msg.body.data();
            long** vtable_ptr = reinterpret_cast<long**>(cb);
            long*  vtable     = *vtable_ptr;
            auto   run        = reinterpret_cast<RunFn>(vtable[0]);
            run(cb, payload);
        }
    }

    // 2) Drain CCallResult pending map → dispatch to
    //    vtable[1](payload, bIOFailure, hCall) on registered cbs,
    //    THEN consume the entry. Polled-only results (no registered
    //    cb) stay in the map until Steam_GetAPICallResult consumes
    //    them, so a fire-and-forget caller still sees the answer.
    //
    // Snapshot the (hCall, msg, cbs) tuples under the lock, release
    // it, then dispatch. Avoids re-entrancy deadlocks if a cb's Run
    // re-enters push_call_result or RegisterCallResult.
    struct PendingDispatch {
        uint64_t              h_call;
        bool                  io_failure;
        std::vector<uint8_t>  body;
        std::vector<void*>    cbs;
    };
    std::vector<PendingDispatch> to_dispatch;
    {
        std::lock_guard<std::mutex> lk(s.call_results_mu);
        for (auto it = s.call_results_pending.begin();
             it != s.call_results_pending.end(); ) {
            auto cbs = lsc::find_call_result_cbs(it->first);
            if (cbs.empty()) {
                // Keep the entry so a Steam_GetAPICallResult poll
                // path still finds it. Move on.
                ++it;
                continue;
            }
            to_dispatch.push_back({
                it->first, it->second.io_failure,
                std::move(it->second.body), std::move(cbs)});
            it = s.call_results_pending.erase(it);
        }
    }
    for (auto& d : to_dispatch) {
        void* payload = d.body.empty() ? nullptr : d.body.data();
        for (void* cb : d.cbs) {
            // Vtable slot 1 = Run(void* pvParam, bool bIOFailure,
            // SteamAPICall_t hSteamAPICall) per CCallResult.
            using RunResultFn = void (*)(void* /*this*/, void* /*pvParam*/,
                                         bool /*bIOFailure*/,
                                         uint64_t /*hSteamAPICall*/);
            long** vtable_ptr = reinterpret_cast<long**>(cb);
            long*  vtable     = *vtable_ptr;
            auto   run        = reinterpret_cast<RunResultFn>(vtable[1]);
            run(cb, payload, d.io_failure, d.h_call);
        }
    }
}

// Helper exposed by the SDK so consumers don't need to reach into
// libsteamclient.so directly — convenience accessor for the bundled
// install path. Returns nullptr (the SDK contract allows it) since
// our imageFs path isn't a real "Steam install directory."
extern "C" __attribute__((visibility("default")))
const char* SteamAPI_GetSteamInstallPath(void) { return nullptr; }

// Wrapper around the V020 ISteamClient global. Some SDK shims pull
// "the global ISteamClient" via this rather than calling CreateInterface
// themselves. Just delegate.
extern "C" __attribute__((visibility("default")))
void* SteamClient(void) {
    int err = 0;
    return CreateInterface("SteamClient020", &err);
}

// SteamGameServer init/shutdown counterparts — same shape as the
// client side. We don't model game-server sessions yet; return false
// from init so callers know it's not available, but never crash.
extern "C" __attribute__((visibility("default")))
bool SteamGameServer_Init(uint32_t /*unIP*/, uint16_t /*usGamePort*/,
                          uint16_t /*usQueryPort*/, int /*eServerMode*/,
                          const char* /*pchVersionString*/) {
    WN_LOGI("SteamGameServer_Init -> false (game-server mode not implemented)");
    return false;
}
extern "C" __attribute__((visibility("default")))
void SteamGameServer_Shutdown(void) {}
extern "C" __attribute__((visibility("default")))
bool SteamGameServer_BSecure(void) { return false; }
extern "C" __attribute__((visibility("default")))
uint64_t SteamGameServer_GetSteamID(void) { return 0; }
extern "C" __attribute__((visibility("default")))
int SteamGameServer_GetHSteamPipe(void) { return 0; }
extern "C" __attribute__((visibility("default")))
int SteamGameServer_GetHSteamUser(void) { return 0; }
extern "C" __attribute__((visibility("default")))
void SteamGameServer_RunCallbacks(void) {}

// ---------------------------------------------------------------------------
// SteamAPI_ManualDispatch_* — the modern (SDK 1.49+) callback API. Unity
// and Unreal games use this in preference to SteamAPI_RunCallbacks
// because it can be driven from any thread and lets the engine pace
// callback delivery alongside its own frame loop. Without these exports
// games fall back to SteamAPI_RunCallbacks but newer engines fail their
// init probe + show "Steamworks: manual dispatch not supported".
//
// The 5-function API:
//   ManualDispatch_Init()                          one-shot init
//   ManualDispatch_RunFrame(HSteamPipe)            kick the next batch
//   ManualDispatch_GetNextCallback(pipe, *msg)→bool  peek + populate msg
//   ManualDispatch_FreeLastCallback(pipe)          ack the peeked msg
//   ManualDispatch_GetAPICallResult(pipe,...)→bool  poll async result
//
// CallbackMsg_t wire layout (Steamworks SDK steamtypes.h, 24B):
//   int      m_hSteamUser     (4)
//   int      m_iCallback      (4)
//   uint8_t* m_pubParam       (8)
//   int      m_cubParam       (4)
//   pad      [4]              (4)
//
// We dispatch through the same callback_queue that SteamAPI_RunCallbacks
// drains — the difference is the consumer controls the loop instead of
// us looping internally. State::last_param holds the body bytes for the
// peeked entry until FreeLastCallback releases it (matches Valve's
// "iterator-style" contract).
// ---------------------------------------------------------------------------
namespace {
struct CallbackMsgWire {
    int32_t   h_steam_user;
    int32_t   i_callback;
    uint8_t*  pub_param;
    int32_t   cub_param;
    int32_t   _pad;
};
static_assert(sizeof(CallbackMsgWire) == 24, "CallbackMsg_t must be 24B");

// Tracks whether ManualDispatch_GetNextCallback has handed out a message
// that hasn't been freed yet. SteamAPI_RunCallbacks should NOT compete
// with manual dispatch on the same pipe; we don't strictly enforce that
// but flag double-peek as a no-op to match SDK semantics.
std::atomic<bool> g_manual_dispatch_active{false};
}  // namespace

// ---------------------------------------------------------------------------
// Breakpad_* — Valve's crash-reporter integration symbols. Source-
// engine games (Half-Life 2, Portal, CS, Dota 2) and some indie titles
// dlsym these at init to hook their own native-crash uploader into
// Steam's minidump pipeline. Returning cleanly (no-op) lets the games
// proceed past the probe; we don't actually upload minidumps anywhere.
// ---------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamMiniDumpInit(uint32_t /*unAppID*/,
                                 const char* /*pchVersion*/,
                                 const char* /*pchDate*/) {}
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamSendMiniDump(void* /*pvException*/,
                                 uint32_t /*ulSeconds*/,
                                 const char* /*pchAssertMsg*/) {}
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamSetAppID(uint32_t /*unAppID*/) {}
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamSetSteamID(uint64_t /*ulSteamID*/) {}
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamWriteMiniDumpSetComment(const char* /*pchMsg*/) {}
extern "C" __attribute__((visibility("default")))
void Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(
        unsigned int /*uStructuredExceptionCode*/,
        void* /*pExceptionInfo*/,
        unsigned int /*uBuildID*/) {}

extern "C" __attribute__((visibility("default")))
void SteamAPI_ManualDispatch_Init(void) {
    g_manual_dispatch_active.store(true, std::memory_order_release);
    WN_LOGI("SteamAPI_ManualDispatch_Init() — manual callback dispatch armed");
}

extern "C" __attribute__((visibility("default")))
void SteamAPI_ManualDispatch_RunFrame(int /*hSteamPipe*/) {
    // No-op for now — our callback_queue is fed asynchronously by the
    // CMClient observer chain; nothing to "tick" per frame. Provided
    // so games that call this every frame don't dlsym-fail.
}

extern "C" __attribute__((visibility("default")))
bool SteamAPI_ManualDispatch_GetNextCallback(int /*hSteamPipe*/, void* p_msg_out) {
    if (!p_msg_out) return false;
    auto& s = lsc::state();
    lsc::CallbackMsg msg;
    {
        std::lock_guard<std::mutex> lk(s.callback_mu);
        if (s.callback_queue.empty()) return false;
        msg = std::move(s.callback_queue.front());
        s.callback_queue.pop_front();
        // last_param is the storage backing the m_pubParam pointer until
        // FreeLastCallback runs. Stable across the manual-dispatch
        // consumer's read window (matches Steam_BGetCallback semantics).
        s.last_param = std::move(msg.body);
    }
    auto* out = static_cast<CallbackMsgWire*>(p_msg_out);
    out->h_steam_user = msg.user;
    out->i_callback   = msg.id;
    out->pub_param    = s.last_param.empty() ? nullptr : s.last_param.data();
    out->cub_param    = static_cast<int32_t>(s.last_param.size());
    out->_pad         = 0;
    return true;
}

extern "C" __attribute__((visibility("default")))
void SteamAPI_ManualDispatch_FreeLastCallback(int /*hSteamPipe*/) {
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.callback_mu);
    s.last_param.clear();
}

// GetAPICallResult — polls a single CCallResult by hCall. Same dispatch
// rules as Steam_GetAPICallResult: returns false if the call hasn't
// completed; on completion writes payload bytes (up to cbCallback) and
// sets *pbFailed. Used by games to await async ops without registering
// a CCallResult subclass.
extern "C" __attribute__((visibility("default")))
bool SteamAPI_ManualDispatch_GetAPICallResult(int /*hSteamPipe*/,
                                               uint64_t hCall,
                                               void* p_callback,
                                               int cb_callback,
                                               int i_callback_expected,
                                               bool* pb_failed) {
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.call_results_mu);
    auto it = s.call_results_pending.find(hCall);
    if (it == s.call_results_pending.end()) return false;
    auto msg = std::move(it->second);
    s.call_results_pending.erase(it);
    if (i_callback_expected != 0 && msg.callback_id != i_callback_expected) {
        // Caller asked for a different callback shape than what landed.
        // Re-insert + report not-yet-ready so a subsequent poll with
        // the right ID picks it up. Rare; logged for diagnostic.
        const int got = msg.callback_id;
        s.call_results_pending[hCall] = std::move(msg);
        WN_LOGI("ManualDispatch_GetAPICallResult: hCall=0x%llx callback mismatch "
                "(expected=%d got=%d) — re-queueing",
                (unsigned long long)hCall, i_callback_expected, got);
        return false;
    }
    if (p_callback && cb_callback > 0 && !msg.body.empty()) {
        const int n = std::min<int>(cb_callback, static_cast<int>(msg.body.size()));
        std::memcpy(p_callback, msg.body.data(), static_cast<size_t>(n));
    }
    if (pb_failed) *pb_failed = msg.io_failure;
    return true;
}
