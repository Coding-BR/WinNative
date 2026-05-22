#include "wn_libsteamclient/runtime_state.h"
#include "wn_libsteamclient/callbacks.h"
#include "wn_libsteamclient/tcp_services.h"

#include <android/log.h>
#include <cstdlib>
#include <cstring>

namespace wn_libsteamclient {

namespace {

// Seed pushed_state from env vars when this libsteamclient.so instance
// lives in a guest process (Wine game's TheForest.exe) that doesn't get
// nativeSet* calls from the WinNative-app side. WinNative exports
// STEAMID / SteamAppId / SteamUser / SteamGameId on every game launch
// before exec'ing Wine; the guest's libsteamclient.so reads them here
// at first state access so BLoggedOn / GetSteamID / GetAppID /
// GetPersonaName all answer truthfully without needing cross-process
// IPC. Idempotent + thread-safe via std::call_once.
//
// Background: each process that dlopens libsteamclient.so gets its own
// PushedState singleton — the WinNative-app instance has the live
// CMClient feeding it via cm_bridge, but the Wine guest's instance is
// a separate address space starting empty. Without this env-bootstrap,
// Forest's main-menu MULTIPLAYER click hits a BLoggedOn==false branch
// and surfaces "STEAM NOT INITIALIZED. TRY LAUNCHING THE GAME FROM
// STEAM" before ever attempting a SteamMatchmaking call (verified live
// 2026-05-20: wine_stderr.log shows zero outbound lobby_get_list
// despite Forest's MULTIPLAYER UI advancing to the lobby-browser
// scene).
void seed_state_from_env_once() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        const char* env_sid = std::getenv("STEAMID");
        const char* env_app = std::getenv("SteamAppId");
        const char* env_usr = std::getenv("SteamUser");
        uint64_t sid = 0;
        if (env_sid && *env_sid) {
            sid = std::strtoull(env_sid, nullptr, 10);
        }
        uint32_t app = 0;
        if (env_app && *env_app) {
            app = static_cast<uint32_t>(std::strtoul(env_app, nullptr, 10));
        }
        // Stamp the state up front. Holding state_mutex is overkill at
        // module-init time (no other thread is in there yet) but keeps
        // the writes paired with the atomic stores used elsewhere so
        // a static analyzer doesn't flag false-sharing.
        if (sid != 0) {
            pushed().steam_id.store(sid);
            // Logged-on / connected live on State (interface singleton),
            // not PushedState (data singleton). Forest's main-menu
            // MULTIPLAYER button gates on BLoggedOn → state().logged_on.
            // Steam wouldn't have exported STEAMID unless WinNative's
            // CMClient was actually LoggedOn at the moment it spawned
            // this Wine process; treating the env presence as proxy
            // authority is strictly less wrong than returning false.
            state().user.store(1);
            state().logged_on.store(true);
            state().connected.store(true);
            __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
                "guest seed: STEAMID=%llu logged_on=true",
                static_cast<unsigned long long>(sid));
        }
        if (app != 0) {
            pushed().app_id.store(app);
        }
        if (env_usr && *env_usr) {
            // persona_name is std::string protected by state_mutex
            // (PushedState is mostly atomic-bool/uint, strings are
            // mutex-guarded).
            std::lock_guard<std::mutex> lk(state_mutex());
            if (pushed().persona_name.empty()) {
                pushed().persona_name = env_usr;
            }
        }
    });
}

// Library-load constructor: run the env-seed as early as possible. This
// makes the seed fire even when the calling chain never reaches our
// exported alloc_pipe — e.g. when LD_PRELOAD'd into a Wine game process
// where Valve's steam_api64.dll short-circuits before LoadLibrary'ing
// steamclient64.dll. The seed is std::call_once-guarded, so the later
// alloc_pipe call is still safe + idempotent.
__attribute__((constructor))
static void wn_libsteamclient_so_loaded() {
    seed_state_from_env_once();
}

}  // namespace

// File-scope globals — guaranteed single-instance under -fvisibility=
// hidden. The previous function-local-static pattern was producing
// per-TU copies of the State / PushedState across hidden-visibility
// translation units, which caused setters in one TU to write a
// different instance than the getters in another TU read. (Diagnosed
// 2026-05-19 from logcat: set_persona_name's &persona_name was
// 0x7d000baa18 but GetPersonaName's &persona_name was 0x7db75eca18
// — DIFFERENT — proving the singleton wasn't actually a singleton.)
// File-scope globals + accessor functions resolve the issue: every
// TU that calls state()/pushed() goes through the same exported
// function and gets the same global instance.
namespace {
State        g_state_singleton;
PushedState  g_pushed_singleton;
std::mutex   g_state_mutex_singleton;
}  // namespace

State&       state()       { return g_state_singleton; }
std::mutex&  state_mutex() { return g_state_mutex_singleton; }
PushedState& pushed()      { return g_pushed_singleton; }

HSteamPipe alloc_pipe() {
    // First pipe creation is the natural cue to bring up the TCP IPC
    // listeners — by this point the bootstrap has finished its env-var
    // pass and the Steam3Master/SteamClientService addresses are set.
    // start_tcp_services() is idempotent; subsequent pipe creations
    // are no-ops on the listener side.
    start_tcp_services();
    // In the WinNative-app process, the Kotlin SteamService will populate
    // pushed_state via nativeSet* JNI calls; the env seed here is a no-op
    // because steam_id has already been pushed before alloc_pipe runs.
    // In a Wine guest process (TheForest.exe), no JNI side exists — this
    // is the only path that fills pushed_state. Either way, idempotent.
    seed_state_from_env_once();
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    HSteamPipe cur = s.pipe.load();
    if (cur != 0) return 0;  // already allocated; caller can read s.pipe
    s.pipe.store(1);
    return 1;
}

bool release_pipe(HSteamPipe pipe) {
    // Clear the logged-on state first via the shared helper so a
    // SteamServersDisconnected_t fires for any consumer that was
    // mid-pump when the pipe got released. set_logged_on takes
    // callback_mu (not state_mutex) so calling it from outside the
    // state_mutex critical section is fine — and necessary, because
    // its own push_callback path would deadlock against state_mutex
    // if we held it here.
    set_logged_on(false);
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    if (pipe == 0 || s.pipe.load() != pipe) return false;
    s.pipe.store(0);
    s.user.store(0);
    // logged_on / connected already cleared by set_logged_on above.
    return true;
}

HSteamUser alloc_global_user(HSteamPipe pipe) {
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    if (pipe == 0 || s.pipe.load() != pipe) return 0;
    HSteamUser cur = s.user.load();
    if (cur != 0) return cur;  // idempotent: same global user across calls
    s.user.store(1);
    return 1;
}

void release_user(HSteamPipe pipe, HSteamUser user) {
    std::lock_guard<std::mutex> lk(state_mutex());
    auto& s = state();
    if (pipe == 0 || user == 0) return;
    if (s.pipe.load() != pipe || s.user.load() != user) return;
    s.user.store(0);
    s.logged_on.store(false);
}

void push_callback(int user, int id, const void* data, size_t n) {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.callback_mu);
    CallbackMsg m;
    m.user = user;
    m.id   = id;
    if (data && n > 0) {
        m.body.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + n);
    }
    s.callback_queue.push_back(std::move(m));
}

uint64_t alloc_api_call_handle() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.call_results_mu);
    // Skip 0 (sentinel) and never wrap to it. ~9.2 quintillion handles
    // before overflow — comfortable lifetime cap.
    uint64_t h = s.next_api_call_handle++;
    if (s.next_api_call_handle == 0) s.next_api_call_handle = 1;
    return h;
}

void push_call_result(uint64_t h_call, int callback_id,
                      const void* data, size_t n, bool io_failure) {
    if (h_call == 0) return;
    auto& s = state();
    CallResultMsg m;
    m.h_call      = h_call;
    m.callback_id = callback_id;
    m.io_failure  = io_failure;
    if (data && n > 0) {
        m.body.assign(static_cast<const uint8_t*>(data),
                      static_cast<const uint8_t*>(data) + n);
    }
    {
        std::lock_guard<std::mutex> lk(s.call_results_mu);
        s.call_results_pending[h_call] = std::move(m);
    }
    // SDK contract: also fire SteamAPICallCompleted_t into the regular
    // callback queue so games that hook generic completions via
    // STEAM_CALLBACK(SteamAPICallCompleted_t, …) see every async
    // hCall complete — not just the ones with typed CCallResults
    // registered. Real Steam Client emits both; without this, our
    // 28+ async slots complete via Steam_GetAPICallResult polling
    // but the generic listener pattern never fires.
    callbacks::SteamAPICallCompleted ev{};
    ev.m_hAsyncCall = h_call;
    ev.m_iCallback  = callback_id;
    ev.m_cubParam   = static_cast<uint32_t>(n);
    push_callback(s.user.load(),
                  callbacks::kSteamAPICallCompleted,
                  &ev, sizeof(ev));
}

void set_logged_on(bool logged_on, int eresult_on_disconnect) {
    auto& s = state();
    bool prev = s.logged_on.exchange(logged_on);
    s.connected.store(logged_on);
    if (prev == logged_on) return;  // idempotent — no transition
    int h_user = s.user.load();
    if (logged_on) {
        // SteamServersConnected_t is an empty marker payload — cubParam=0,
        // pubParam=nullptr per the Steamworks SDK contract for empty
        // callback structs.
        push_callback(h_user, callbacks::kSteamServersConnected, nullptr, 0);
    } else {
        callbacks::SteamServersDisconnected payload{};
        payload.m_eResult = eresult_on_disconnect;
        push_callback(h_user, callbacks::kSteamServersDisconnected,
                      &payload, sizeof(payload));
    }
}

}  // namespace wn_libsteamclient
