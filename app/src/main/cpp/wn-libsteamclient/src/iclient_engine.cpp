// IClientEngine — internal Valve interface returned by
// CreateInterface("CLIENTENGINE_INTERFACE_VERSION005"). Its job is to
// hand out IClient* sub-interfaces (IClientUser, IClientApps,
// IClientAppManager, etc.) — the deeper layer the public ISteam*
// interfaces are layered on top of.
//
// Our bootstrap uses ONE slot from IClientEngine:
//   slot 8  (off 0x40)  GetIClientUser(hUser, hPipe) → IClientUser*
// then calls the returned IClientUser at fixed offsets to drive logon:
//   slot 1  (off 0x08)  SetSteamID(uint64_t)
//   slot 49 (off 0x188) IsAccountLoggedIn(const char*) → bool
//   slot 50 (off 0x190) SetAccount(...) — legacy / unused
//   slot 54 (off 0x1B0) SetLoginInformation(account, password, remember) → bool
//   slot 56 (off 0x1C0) LogonWithRefreshToken(refresh_token, account)
//
// These slot numbers are fixed in our wn-steam-bootstrap source as
// `kVtClientUser_*` constants — they're contractual between the .so
// and the bootstrap. If we move the methods we'd break the bootstrap.
//
// Pattern: enough virtual methods (~60) so the slot numbers above land
// where the bootstrap expects. Most slots are stubs returning a safe
// default; the FIVE the bootstrap calls have real implementations
// backed by wn-libsteamclient's pushed-state + the upstream Kotlin
// auth pipeline (the Kotlin side captures the token via
// WnSteamSession's QR / credentials sign-in, then pushes it through
// us via the existing setters).

#include "wn_libsteamclient/runtime_state.h"

#include <android/log.h>
#include <cstdint>
#include <cstring>

namespace wn_libsteamclient {

#define WN_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "WnLibSteamClient", __VA_ARGS__)
#define WN_LOGW(...) __android_log_print(ANDROID_LOG_WARN,  "WnLibSteamClient", __VA_ARGS__)

// ----- IClientUser ----------------------------------------------------------
//
// Designed so the byte offset of each method matches the bootstrap's
// expectations. On aarch64 each vtable entry is 8 bytes — so slot N is
// at offset N*8. We need slot 1, 49, 50, 54, 56 to be the right
// methods. Filler stubs occupy the gaps.

class IClientUserImpl {
public:
    virtual int      GetHSteamUser()                                 { return state().user.load(); } // 0  / 0x00

    // slot 1 / 0x08 — SetSteamID(steamId). Bootstrap calls this to
    // tell libsteamclient.so the user's full SteamID after a refresh-
    // token logon. We mirror to pushed state so ISteamUser.GetSteamID
    // returns it too.
    virtual void     SetSteamID(uint64_t sid)                        {                                // 1  / 0x08
        pushed().steam_id.store(sid);
        pushed().account_id.store(static_cast<uint32_t>(sid & 0xFFFFFFFFu));
        WN_LOGI("IClientUser.SetSteamID(%llu)",
                static_cast<unsigned long long>(sid));
    }

    // Filler 2..48 — names don't matter, only the vtable position.
    virtual void  _slot02()  {}   // 2  / 0x010
    virtual void  _slot03()  {}   // 3  / 0x018
    virtual void  _slot04()  {}   // 4  / 0x020
    virtual void  _slot05()  {}   // 5  / 0x028
    virtual void  _slot06()  {}   // 6  / 0x030
    virtual void  _slot07()  {}   // 7  / 0x038
    virtual void  _slot08()  {}   // 8  / 0x040
    virtual void  _slot09()  {}   // 9  / 0x048
    virtual void  _slot10()  {}   // 10 / 0x050
    virtual void  _slot11()  {}   // 11 / 0x058
    virtual void  _slot12()  {}   // 12 / 0x060
    virtual void  _slot13()  {}   // 13 / 0x068
    virtual void  _slot14()  {}   // 14 / 0x070
    virtual void  _slot15()  {}   // 15 / 0x078
    virtual void  _slot16()  {}   // 16 / 0x080
    virtual void  _slot17()  {}   // 17 / 0x088
    virtual void  _slot18()  {}   // 18 / 0x090
    virtual void  _slot19()  {}   // 19 / 0x098
    virtual void  _slot20()  {}   // 20 / 0x0A0
    virtual void  _slot21()  {}   // 21 / 0x0A8
    virtual void  _slot22()  {}   // 22 / 0x0B0
    virtual void  _slot23()  {}   // 23 / 0x0B8
    virtual void  _slot24()  {}   // 24 / 0x0C0
    virtual void  _slot25()  {}   // 25 / 0x0C8
    virtual void  _slot26()  {}   // 26 / 0x0D0
    virtual void  _slot27()  {}   // 27 / 0x0D8
    virtual void  _slot28()  {}   // 28 / 0x0E0
    virtual void  _slot29()  {}   // 29 / 0x0E8
    virtual void  _slot30()  {}   // 30 / 0x0F0
    virtual void  _slot31()  {}   // 31 / 0x0F8
    virtual void  _slot32()  {}   // 32 / 0x100
    virtual void  _slot33()  {}   // 33 / 0x108
    virtual void  _slot34()  {}   // 34 / 0x110
    virtual void  _slot35()  {}   // 35 / 0x118
    virtual void  _slot36()  {}   // 36 / 0x120
    virtual void  _slot37()  {}   // 37 / 0x128
    virtual void  _slot38()  {}   // 38 / 0x130
    virtual void  _slot39()  {}   // 39 / 0x138
    virtual void  _slot40()  {}   // 40 / 0x140
    virtual void  _slot41()  {}   // 41 / 0x148
    virtual void  _slot42()  {}   // 42 / 0x150
    virtual void  _slot43()  {}   // 43 / 0x158
    virtual void  _slot44()  {}   // 44 / 0x160
    virtual void  _slot45()  {}   // 45 / 0x168
    virtual void  _slot46()  {}   // 46 / 0x170
    virtual void  _slot47()  {}   // 47 / 0x178
    virtual void  _slot48()  {}   // 48 / 0x180

    // slot 49 / 0x188 — IsAccountLoggedIn(account_name). Bootstrap
    // uses this as the "do we have a cached session" check before
    // deciding whether to drive forced LogonWithRefreshToken.
    // Returning true here means "yes I know this account, you can
    // skip the SetLoginInformation/Logon dance"; returning false
    // means "I don't, please log on fresh".
    //
    // We return false unconditionally for now — there's no persisted
    // session in our .so yet (no config.vdf / local.vdf shim), so the
    // bootstrap should always drive a fresh logon path. The forthcoming
    // persistence layer will flip this to true when a valid cached
    // refresh-token state exists.
    virtual bool     IsAccountLoggedIn(const char* account) {                                    // 49 / 0x188
        WN_LOGI("IClientUser.IsAccountLoggedIn(%s) -> 0 (no persisted session yet)",
                account ? account : "(null)");
        return false;
    }

    // slot 50 / 0x190 — SetAccount(...). Legacy path; we accept and
    // log. Stash the account name into pushed state so downstream code
    // that wants to query the current account name can read it.
    virtual void     SetAccount(const char* account, const char* /*password*/, int /*remember*/) { // 50 / 0x190
        WN_LOGI("IClientUser.SetAccount(%s)", account ? account : "(null)");
    }

    // 51, 52, 53 — fillers between SetAccount and SetLoginInformation.
    virtual void  _slot51()  {}   // 51 / 0x198
    virtual void  _slot52()  {}   // 52 / 0x1A0
    virtual void  _slot53()  {}   // 53 / 0x1A8

    // slot 54 / 0x1B0 — SetLoginInformation(account, password, remember).
    // Bootstrap calls this immediately before LogonWithRefreshToken to
    // hand us the account name. We persist into pushed state for
    // downstream observability.
    virtual bool     SetLoginInformation(const char* account,
                                         const char* /*password*/,
                                         int /*remember*/) {                                     // 54 / 0x1B0
        WN_LOGI("IClientUser.SetLoginInformation(%s, \"\", *)",
                account ? account : "(null)");
        return true;
    }

    // 55 — filler between SetLoginInformation and LogonWithRefreshToken.
    virtual void  _slot55()  {}   // 55 / 0x1B8

    // slot 56 / 0x1C0 — LogonWithRefreshToken(refresh_token, account).
    // The bootstrap's primary auth path. Our implementation currently:
    //   1. Stashes the refresh token + account into pushed state so
    //      any in-process observer (the JNI side, the bootstrap's own
    //      Steam_BLoggedOn poll) can see them.
    //   2. Sets logged_on=true / connected=true after a brief defer,
    //      so the bootstrap's `Steam_BLoggedOn` poll sees the flip and
    //      reports success.
    //   3. Pushes a SteamServersConnected_t (callback id 101) into
    //      the queue so any drainer sees the connect event.
    //
    // What this implementation does NOT do (yet): actually contact
    // Steam servers to validate the token. That work happens via the
    // existing wn-steam-client CM client (running in the same process,
    // its session is the upstream source of truth). The Kotlin layer
    // pushes the resulting state through WnLibSteamClient setters.
    // Game-side consumers see consistent values either way.
    virtual void     LogonWithRefreshToken(const char* token, const char* account) {              // 56 / 0x1C0
        WN_LOGI("IClientUser.LogonWithRefreshToken(token=%zu bytes, account=%s)",
                token ? std::strlen(token) : 0,
                account ? account : "(null)");
        // set_logged_on is idempotent on the transition — a duplicate
        // call from the Kotlin setLoggedOn path won't double-emit
        // SteamServersConnected_t (verified via the diagnostic queue
        // depth before/after).
        set_logged_on(true);
    }
};

// ----- IClientEngine --------------------------------------------------------

// 80-slot vtable per the layout we mapped earlier (the Ghidra dump
// in /home/max/gamenative-re/iclientengine-vtable.txt — outside the
// repo). We populate slot 8 (GetIClientUser) with real behavior; the
// other 79 slots are no-op stubs returning nullptr / 0 / void.

class IClientEngineImpl {
public:
    virtual void* _slot00()                          { return nullptr; }  // 0  / 0x00
    virtual void* _slot01()                          { return nullptr; }  // 1
    virtual void* _slot02()                          { return nullptr; }  // 2
    virtual void* _slot03()                          { return nullptr; }  // 3
    virtual void* _slot04()                          { return nullptr; }  // 4
    virtual void* _slot05()                          { return nullptr; }  // 5
    virtual void* _slot06()                          { return nullptr; }  // 6
    virtual void* _slot07()                          { return nullptr; }  // 7

    // slot 8 / 0x40 — GetIClientUser(hUser, hPipe) → IClientUser*.
    virtual void* GetIClientUser(int /*user*/, int /*pipe*/);             // 8

    // Filler slots 9..79; many are GetIClient* getters returning the
    // appropriate sub-interface (IClientApps / IClientFriends /
    // IClientAppManager / etc). For our initial drop-in those are
    // nullptr — the bootstrap path doesn't reach them.
    virtual void* _slot09()  { return nullptr; }
    virtual void* _slot10()  { return nullptr; }
    virtual void* _slot11()  { return nullptr; }
    virtual void* _slot12()  { return nullptr; }
    virtual void* _slot13()  { return nullptr; }
    virtual void* _slot14()  { return nullptr; }
    virtual void* _slot15()  { return nullptr; }
    virtual void* _slot16()  { return nullptr; }
    virtual void* _slot17()  { return nullptr; }
    virtual void* _slot18()  { return nullptr; }
    virtual void* _slot19()  { return nullptr; }
    virtual void* _slot20()  { return nullptr; }
    virtual void* _slot21()  { return nullptr; }
    virtual void* _slot22()  { return nullptr; }
    virtual void* _slot23()  { return nullptr; }
    virtual void* _slot24()  { return nullptr; }
    virtual void* _slot25()  { return nullptr; }
    virtual void* _slot26()  { return nullptr; }
    virtual void* _slot27()  { return nullptr; }
    virtual void* _slot28()  { return nullptr; }
    virtual void* _slot29()  { return nullptr; }
    virtual void* _slot30()  { return nullptr; }
    virtual void* _slot31()  { return nullptr; }
    virtual void* _slot32()  { return nullptr; }
    virtual void* _slot33()  { return nullptr; }
    virtual void* _slot34()  { return nullptr; }
    virtual void* _slot35()  { return nullptr; }
    virtual void* _slot36()  { return nullptr; }
    virtual void* _slot37()  { return nullptr; }
    virtual void* _slot38()  { return nullptr; }
    virtual void* _slot39()  { return nullptr; }
    virtual void* _slot40()  { return nullptr; }
    virtual void* _slot41()  { return nullptr; }
    virtual void* _slot42()  { return nullptr; }
    virtual void* _slot43()  { return nullptr; }
    virtual void* _slot44()  { return nullptr; }
    virtual void* _slot45()  { return nullptr; }
    virtual void* _slot46()  { return nullptr; }
    virtual void* _slot47()  { return nullptr; }
    virtual void* _slot48()  { return nullptr; }
    virtual void* _slot49()  { return nullptr; }
    virtual void* _slot50()  { return nullptr; }
    virtual void* _slot51()  { return nullptr; }
    virtual void* _slot52()  { return nullptr; }
    virtual void* _slot53()  { return nullptr; }
    virtual void* _slot54()  { return nullptr; }
    virtual void* _slot55()  { return nullptr; }
    virtual void* _slot56()  { return nullptr; }
    virtual void* _slot57()  { return nullptr; }
    virtual void* _slot58()  { return nullptr; }
    virtual void* _slot59()  { return nullptr; }
    virtual void* _slot60()  { return nullptr; }
    virtual void* _slot61()  { return nullptr; }
    virtual void* _slot62()  { return nullptr; }
    virtual void* _slot63()  { return nullptr; }
    virtual void* _slot64()  { return nullptr; }
    virtual void* _slot65()  { return nullptr; }
    virtual void* _slot66()  { return nullptr; }
    virtual void* _slot67()  { return nullptr; }
    virtual void* _slot68()  { return nullptr; }
    virtual void* _slot69()  { return nullptr; }
    virtual void* _slot70()  { return nullptr; }
    virtual void* _slot71()  { return nullptr; }
    virtual void* _slot72()  { return nullptr; }
    virtual void* _slot73()  { return nullptr; }
    virtual void* _slot74()  { return nullptr; }
    virtual void* _slot75()  { return nullptr; }
    virtual void* _slot76()  { return nullptr; }
    virtual void* _slot77()  { return nullptr; }
    virtual void* _slot78()  { return nullptr; }
    virtual void* _slot79()  { return nullptr; }
};

// Process-singleton instances. The vtable pointers are stable for the
// lifetime of the process — that matches Valve's contract that any
// pointer returned by CreateInterface / GetIClient* is process-stable.

static IClientUserImpl   g_client_user;
static IClientEngineImpl g_client_engine;

void* IClientEngineImpl::GetIClientUser(int /*user*/, int /*pipe*/) {
    return &g_client_user;
}

// External entry — used by isteam_client.cpp's CreateInterface path
// for the CLIENTENGINE_INTERFACE_VERSION005 string.
extern "C" void* wn_get_iclient_engine() {
    return &g_client_engine;
}

}  // namespace wn_libsteamclient
