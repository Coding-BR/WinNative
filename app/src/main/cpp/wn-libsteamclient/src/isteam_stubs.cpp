// Vtable-backed stub implementations of the major ISteam* interfaces.
//
// Each interface is a C++ class with virtual methods matching the
// public Steamworks SDK's declared method order (so a consumer that
// indexes `vtable[N]` gets the expected method). Returns are safe
// defaults (false / 0 / null / empty); real backend wiring against
// `wn-steam-client` lands incrementally.
//
// Method order MUST match the Steamworks SDK headers exactly — that's
// the implicit ABI consumers rely on. The slot maps embedded in
// app/src/main/cpp/wn-steam-bootstrap/src/steam_bootstrap.cpp (e.g.
// ISteamApps slot 6 = BIsSubscribedApp, ISteamFriends slot 0 =
// GetPersonaName) are the source of truth.

#include "wn_libsteamclient/runtime_state.h"
#include "wn_libsteamclient/callbacks.h"
// cm_bridge: extern "C" routing into the live wn-steam-client CMClient
// (resolved by the dynamic linker against libwnsteam.so at app load).
#include "wn_steam/cm_bridge.h"

#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstdlib>     // std::getenv, std::strtoul — GetAppID env fallback
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lsc_cb = wn_libsteamclient::callbacks;

namespace wn_libsteamclient {

// Async-read buffer cache for FileReadAsync (slot 3) → FileReadAsync
// Complete (slot 4) handoff. Single-shot per hCall; FileReadAsync
// Complete erases the entry on successful copy-out.
static std::mutex& async_read_mu() {
    static std::mutex m;
    return m;
}
static std::unordered_map<uint64_t, std::vector<uint8_t>>& async_read_buffers() {
    static std::unordered_map<uint64_t, std::vector<uint8_t>> m;
    return m;
}

// Per-handle slot for FileWriteStream* (slots 9-12). The stream lives
// from Open through Close/Cancel; bytes pile up into a tempfile, then
// Close renames it onto the final path atomically.
struct StreamSlot {
    int         fd        = -1;
    std::string tempPath;
    std::string finalPath;
    std::string name;     // original pchFile passed to Open
    int64_t     bytes     = 0;
};
static std::mutex& stream_mu() { static std::mutex m; return m; }
static std::unordered_map<uint64_t, StreamSlot>& streams() {
    static std::unordered_map<uint64_t, StreamSlot> m;
    return m;
}

// ---------------------------------------------------------------------------
// ISteamUtils (version "SteamUtils010") — pipe-only, no user. Public
// surface order from isteamutils.h. Methods we already exercise from
// the bootstrap-side stage-2 self-test: slot 3 GetServerRealTime,
// slot 4 GetIPCountry, slot 5/6 GetImage* , slot 8 GetCurrentBattery
// Power, slot 9 GetAppID, slot 23 GetSteamUILanguage.
class ISteamUtilsStub {
public:
    virtual uint32_t GetSecondsSinceAppActive()              { return 0; }                // 0
    virtual uint32_t GetSecondsSinceComputerActive()         { return 0; }                // 1
    virtual int      GetConnectedUniverse()                  { return 1; /*Public*/ }     // 2
    virtual uint32_t GetServerRealTime()                     {                              // 3
        // Advance the CM-anchored server clock by however much
        // steady_clock has elapsed since the anchor was captured.
        // Without this, the returned epoch would freeze at logon time
        // and games doing date-based gating (daily-quest timers,
        // achievement-unlock timestamps, ban-expiry checks) misbehave.
        auto anchor   = pushed().server_realtime.load();
        auto anchor_local_ms = pushed().server_realtime_anchor_local_ms.load();
        if (anchor != 0 && anchor_local_ms != 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            auto elapsed_s = (now_ms - anchor_local_ms) / 1000;
            if (elapsed_s < 0) elapsed_s = 0;  // clock went backwards
            return static_cast<uint32_t>(anchor + static_cast<uint32_t>(elapsed_s));
        }
        // No anchor yet — fall back to local clock so games see a
        // reasonable epoch from the first call.
        return static_cast<uint32_t>(::time(nullptr));
    }
    virtual const char* GetIPCountry()                       {                              // 4
        auto& p = pushed();
        if (p.ip_country_set.load() == 0) return "US";
        // Storage is std::string; safe to return c_str() while caller
        // doesn't outlive the next set call (Steamworks contract is
        // the pointer is process-stable until SDK shutdown).
        return p.ip_country.c_str();
    }
    // 5 — GetImageSize(iImage, *pnWidth, *pnHeight) — looks up the
    //   pushed image and fills its dimensions. False = unknown handle.
    virtual bool GetImageSize(int iImage, uint32_t* pnWidth, uint32_t* pnHeight) {
        if (iImage <= 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().image_registry.find(iImage);
        if (it == pushed().image_registry.end()) return false;
        if (pnWidth)  *pnWidth  = static_cast<uint32_t>(it->second.width);
        if (pnHeight) *pnHeight = static_cast<uint32_t>(it->second.height);
        return true;
    }
    // 6 — GetImageRGBA(iImage, pubDest, nDestBufferSize) — copies the
    //   RGBA bytes into the caller's buffer. SDK contract:
    //   nDestBufferSize must be == width*height*4 exactly; we don't
    //   partial-copy. Returns false on undersized buffer or unknown
    //   handle.
    virtual bool GetImageRGBA(int iImage, uint8_t* pubDest, int nDestBufferSize) {
        if (iImage <= 0 || !pubDest || nDestBufferSize <= 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().image_registry.find(iImage);
        if (it == pushed().image_registry.end()) return false;
        const auto& img = it->second;
        if (static_cast<int>(img.rgba.size()) > nDestBufferSize) return false;
        std::memcpy(pubDest, img.rgba.data(), img.rgba.size());
        return true;
    }
    virtual bool     GetCSERIPPort(uint32_t*, uint16_t*)     { return false; }            // 7
    virtual uint8_t  GetCurrentBatteryPower()                { return 255; /*AC*/ }       // 8
    // 9 — GetAppID. Prefer the explicitly pushed appId (set by Kotlin
    //   SteamService.prepareLibSteamClientForLaunch → nativeSetAppId).
    //   Fall back to the SteamAppId env var that WnSteamBootstrap sets
    //   before dlopen — covers the early-boot window where game-side
    //   SteamAPI_Init calls GetAppID before Kotlin's setter has fired
    //   (observed in the bootstrap Stage2 diagnostic at logon time).
    virtual uint32_t GetAppID() {
        uint32_t app = pushed().app_id.load();
        if (app != 0) return app;
        const char* env = std::getenv("SteamAppId");
        if (env && *env) {
            char* end = nullptr;
            unsigned long v = std::strtoul(env, &end, 10);
            if (end != env && v != 0 && v <= 0x7fffffffu) {
                return static_cast<uint32_t>(v);
            }
        }
        return 0;
    } // 9
    virtual void     SetOverlayNotificationPosition(int)     {}                           // 10
    // 11 — IsAPICallCompleted(hCall, *pbFailed). Delegates to the
    //   flat-C Steam_IsAPICallCompleted so this vtable path returns
    //   the same answer the SDK shim's flat-call path does. *pbFailed
    //   reflects msg.io_failure on a hit; left untouched on miss.
    virtual bool     IsAPICallCompleted(uint64_t hCall, bool* pbFailed) {
        if (hCall == 0) return false;
        auto& s = state();
        std::lock_guard<std::mutex> lk(s.call_results_mu);
        auto it = s.call_results_pending.find(hCall);
        if (it == s.call_results_pending.end()) return false;
        if (pbFailed) *pbFailed = it->second.io_failure;
        return true;
    }
    // 12 — GetAPICallFailureReason(hCall). The SDK reserves codes
    //   ESteamAPICallFailure: -1 None / 0 SteamGone / 1 NetworkFailure
    //   / 2 InvalidHandle / 3 MismatchedCallback. We don't synthesize
    //   transport failures yet — return -1 (None) when present, -1
    //   when absent (the SDK contract treats -1 as "no failure /
    //   not completed yet" and the caller checks IsAPICallCompleted
    //   separately, so a single -1 is safe).
    virtual int      GetAPICallFailureReason(uint64_t /*hCall*/)       { return -1; }
    // 13 — GetAPICallResult(hCall, pCallback, cubCallback, iCallbackExpected,
    //   *pbFailed). Delegates to Steam_GetAPICallResult — copies the
    //   cached body bytes (capped at cubCallback), sets *pbFailed,
    //   and consumes the entry. iCallbackExpected mismatch keeps the
    //   entry and returns false (SDK contract).
    virtual bool     GetAPICallResult(uint64_t hCall, void* pCallback,
                                       int cubCallback, int iCallbackExpected,
                                       bool* pbFailed) {
        if (hCall == 0) return false;
        auto& s = state();
        std::lock_guard<std::mutex> lk(s.call_results_mu);
        auto it = s.call_results_pending.find(hCall);
        if (it == s.call_results_pending.end()) return false;
        const auto& msg = it->second;
        if (iCallbackExpected != 0 && msg.callback_id != iCallbackExpected) {
            return false;  // keep the entry — caller asked wrong type
        }
        if (pCallback && cubCallback > 0 && !msg.body.empty()) {
            size_t n = std::min<size_t>(static_cast<size_t>(cubCallback), msg.body.size());
            std::memcpy(pCallback, msg.body.data(), n);
        }
        if (pbFailed) *pbFailed = msg.io_failure;
        s.call_results_pending.erase(it);
        return true;
    }
    virtual void     RunFrame()                              {}                           // 14
    virtual uint32_t GetIPCCallCount()                       { return 0; }                // 15
    virtual void     SetWarningMessageHook(void*)            {}                           // 16
    // 17 — IsOverlayEnabled. Games gate "Open Steam profile" / "Browse
    //   Workshop" buttons on this — when false they hide or grey-out
    //   the controls so the user can't click into a broken overlay
    //   flow. Our ActivateGameOverlayTo{WebPage,Store,User,InviteDialog}
    //   slots are wired (commit 142) and route to the Android system
    //   browser via Intent.ACTION_VIEW. Return true so games SHOW the
    //   overlay-entry controls — clicking them opens Chrome/Firefox to
    //   the Steam URL instead of failing silently.
    virtual bool     IsOverlayEnabled()                      { return true; }             // 17
    // 18 — BOverlayNeedsPresent. Renderer-integration hook: signals
    //   the engine to issue a present/flip so the in-process overlay
    //   compositor can blit. We don't ship an in-process overlay (the
    //   ActivateGameOverlay* path is out-of-process browser intents),
    //   so the engine has nothing to composite — false is correct.
    virtual bool     BOverlayNeedsPresent()                  { return false; }            // 18
    // 19 — CheckFileSignature(pszFileName). Async anti-tamper hook
    //   called by Source-engine and some competitive titles. Returns
    //   SteamAPICall_t; result via CheckFileSignature_t (k_iCallback=
    //   705) with ECheckFileSignature: 0 InvalidSignature, 1 Valid
    //   Signature, 2 FileNotFound, 3 NoSignaturesFoundForThisApp,
    //   4 NoSignaturesFoundForThisFile. We don't ship Steam-signed
    //   binaries, so the honest answer is 4 — games that gate on
    //   ValidSignature branch into "untrusted client" UI but proceed
    //   (vs. the previous hCall=0 silently dropping the request,
    //   which deadlocks callers polling CCallResult.IsActive).
    virtual uint64_t CheckFileSignature(const char* /*pszFileName*/) {
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::CheckFileSignature cb{};
        cb.m_eCheckFileSignature = 4; // NoSignaturesFoundForThisFile
        push_call_result(hCall, lsc_cb::kCheckFileSignature,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    virtual bool     ShowGamepadTextInput(int, int, const char*, uint32_t, const char*) { return false; }       // 20
    virtual uint32_t GetEnteredGamepadTextLength()           { return 0; }                // 21
    virtual bool     GetEnteredGamepadTextInput(char*, uint32_t) { return false; }        // 22
    virtual const char* GetSteamUILanguage()                 {                              // 23
        auto& p = pushed();
        if (p.ui_language.empty()) return "english";
        return p.ui_language.c_str();
    }
    virtual bool     IsSteamRunningInVR()                    { return false; }            // 24
    virtual void     SetOverlayNotificationInset(int, int)   {}                           // 25
    virtual bool     IsSteamInBigPictureMode()               { return false; }            // 26
    virtual void     StartVRDashboard()                      {}                           // 27
    virtual bool     IsVRHeadsetStreamingEnabled()           { return false; }            // 28
    virtual void     SetVRHeadsetStreamingEnabled(bool)      {}                           // 29
    // 30 — IsSteamChinaLauncher → false (Android isn't the China client).
    virtual bool     IsSteamChinaLauncher()                  { return false; }
    // 31 — InitFilterText(unFilterOptions). Steam's profanity filter.
    //   Returns false = filter unavailable; games fall back to a
    //   pass-through filter or skip the call entirely.
    virtual bool     InitFilterText(uint32_t)                { return false; }
    // 32 — FilterText(eContext, sourceSteamID, *pchInputMessage,
    //                  *pchOutFilteredText, nByteSizeOutFilteredText)
    //   Returns the number of bytes copied (capped at the buffer).
    //   We don't filter — copy input to output verbatim (truncated).
    virtual int      FilterText(int /*eContext*/, uint64_t /*srcSid*/,
                                  const char* in, char* out, uint32_t outSize) {
        if (!out || outSize == 0) return 0;
        if (!in)  { out[0] = '\0'; return 0; }
        uint32_t n = static_cast<uint32_t>(std::strlen(in));
        uint32_t copy = std::min<uint32_t>(n, outSize - 1);
        if (copy > 0) std::memcpy(out, in, copy);
        out[copy] = '\0';
        return static_cast<int>(copy);
    }
    // 33 — GetIPv6ConnectivityState(eProtocol) → ESteamIPv6Connectivity
    //   State (0=Unknown). The SDK distinguishes Public/Private; we
    //   don't probe, so Unknown is the truthful answer.
    virtual int      GetIPv6ConnectivityState(int)           { return 0; }
    // 34 — IsSteamRunningOnSteamDeck → false (Android isn't a Deck).
    virtual bool     IsSteamRunningOnSteamDeck()             { return false; }
    // 35 — ShowFloatingGamepadTextInput(eMode, x, y, w, h)
    //   Steam Deck's floating IME. Returns false = not shown; games
    //   fall back to their own text input. We could route this to
    //   Android's IME later but it's a UX-only improvement.
    virtual bool     ShowFloatingGamepadTextInput(int, int, int, int, int) { return false; }
    // 36 — SetGameLauncherMode(bLauncherMode). Tells Steam the app is
    //   acting as a launcher (e.g. ARMA Launcher). No-op.
    virtual void     SetGameLauncherMode(bool)               {}
    // 37 — DismissFloatingGamepadTextInput → false (no input shown to
    //   dismiss).
    virtual bool     DismissFloatingGamepadTextInput()       { return false; }
};

// ---------------------------------------------------------------------------
// ISteamUser (version "SteamUser023") — auth-gated. Slot map from
// isteamuser.h. Selected methods consumed by our bootstrap stage-2:
// slot 1 BLoggedOn, slot 2 GetSteamID, slot 18 UserHasLicenseForApp.
class ISteamUserStub {
public:
    virtual int       GetHSteamUser()                                { return state().user.load(); } // 0
    virtual bool      BLoggedOn()                                    { return state().logged_on.load(); } // 1
    virtual uint64_t  GetSteamID()                                   { return pushed().steam_id.load(); } // 2
    virtual int       InitiateGameConnection_DEPRECATED(void*, int, uint64_t, uint32_t, uint16_t, bool) { return 0; } // 3
    virtual void      TerminateGameConnection_DEPRECATED(uint32_t, uint16_t) {} // 4
    virtual void      TrackAppUsageEvent(uint64_t, int, const char*) {}              // 5
    // 6 — GetUserDataFolder(pchBuffer, cubBuffer). Writes the absolute
    //   user-data folder path for the bound app — Steam Cloud lives
    //   under <user-data>/remote/. We derive it by stripping the
    //   trailing "/remote" from the cloud remote dir we already push.
    //   NUL-terminated; truncated if cubBuffer is too small (the SDK
    //   contract permits a partial-fit path but always NUL-terminates).
    virtual bool      GetUserDataFolder(char* pchBuffer, int cubBuffer) {
        if (!pchBuffer || cubBuffer <= 0) return false;
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        std::string ud;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().app_cloud_remote_dirs.find(app);
            if (it == pushed().app_cloud_remote_dirs.end()) return false;
            ud = it->second;
        }
        // Trim a trailing /remote (or remote/) — what we push is the
        // remote dir; the user-data dir is its parent.
        if (ud.size() > 7 && ud.compare(ud.size() - 7, 7, "/remote") == 0) {
            ud.resize(ud.size() - 7);
        } else if (ud.size() > 8 && ud.compare(ud.size() - 8, 8, "/remote/") == 0) {
            ud.resize(ud.size() - 8);
        }
        size_t copy = std::min<size_t>(ud.size(),
                                       static_cast<size_t>(cubBuffer - 1));
        std::memcpy(pchBuffer, ud.data(), copy);
        pchBuffer[copy] = '\0';
        return true;
    }
    virtual void      StartVoiceRecording()                          {}              // 7
    virtual void      StopVoiceRecording()                           {}              // 8
    virtual int       GetAvailableVoice(uint32_t*, uint32_t*, uint32_t) { return 0; }// 9
    virtual int       GetVoice(bool, void*, uint32_t, uint32_t*, bool, void*, uint32_t, uint32_t*, uint32_t) { return 0; } // 10
    virtual int       DecompressVoice(const void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t) { return 0; } // 11
    virtual uint32_t  GetVoiceOptimalSampleRate()                    { return 11025; } // 12
    // 13 — GetAuthSessionTicket(buf, maxLen, *pcbTicket, *pSteamNetworkingIdentity)
    //
    //   Two paths:
    //   1. CM-backed (cache hit): cm_bridge has a Steam-signed ownership
    //      ticket for the bound app_id. We wrap it with the standard
    //      24-byte SteamKit2-style header (see below) before handing
    //      it to the caller so Steam Web API ISteamUserAuth.Authenticate
    //      UserTicket accepts the bytes. Real eresult=1.
    //   2. Fallback (cache miss): emit a 32-byte synthetic WNAT token so
    //      single-player / LAN flows that don't validate server-side
    //      still see a non-empty ticket. eresult=1 cosmetically, but
    //      a real CSGO matchmaking server would reject.
    //
    //   Both paths allocate a fresh HAuthTicket, cache under
    //   auth_tickets so EndAuthSession / CancelAuthTicket can clean up,
    //   copy bytes to caller's buffer, set *pcbTicket, emit
    //   GetAuthSessionTicketResponse_t.
    virtual uint64_t  GetAuthSessionTicket(void* buf, int maxLen,
                                            uint32_t* pcbTicket,
                                            const void* /*pSteamNetworkingIdentity*/) {
        uint32_t h = pushed().next_auth_ticket_handle.fetch_add(1);
        if (h == 0) h = pushed().next_auth_ticket_handle.fetch_add(1);  // skip 0
        uint32_t app_id = pushed().app_id.load();
        std::vector<uint8_t> body;
        bool cm_backed = false;
        if (app_id != 0) {
            // Probe the cm_bridge cache for a real ownership ticket.
            // First call: size-only (null buf, max=0) → returns required
            // length via out_len.
            size_t need = 0;
            wn_cm_get_cached_app_ownership_ticket(app_id, nullptr, 0, &need);
            if (need > 0 && need <= 16 * 1024) {  // sanity-cap at 16KB
                std::vector<uint8_t> ownership(need);
                size_t got = 0;
                if (wn_cm_get_cached_app_ownership_ticket(
                        app_id, ownership.data(), ownership.size(), &got)
                    && got == need) {
                    // Wrap the ownership ticket with the standard
                    // 24-byte SteamKit2-style header so real Steam-side
                    // validation (via Steam Web API ISteamUserAuth.
                    // AuthenticateUserTicket) accepts the bytes. Layout
                    // mirrors what every multiplayer game's lsteamclient.
                    // dll produces on Linux/Windows. Documented in
                    // SteamKit2 SteamUser.cs (BuildAuthSessionTicket).
                    //
                    //   offset 0   uint32 LE  fixed prefix size = 0x14 (20)
                    //   offset 4   uint32 LE  unknown / version (0)
                    //   offset 8   uint32 LE  unknown / version (0)
                    //   offset 12  uint32 LE  ConnectionID — per-ticket
                    //                          handle. We reuse our
                    //                          allocated HAuthTicket so
                    //                          the EndAuthSession lookup
                    //                          chain stays consistent.
                    //   offset 16  uint32 LE  ConnectTime (unix32) — local
                    //                          timestamp; server rejects
                    //                          tickets too far in the
                    //                          past/future.
                    //   offset 20  uint32 LE  ConnectionCount = 1 — server
                    //                          uses this to dedupe.
                    //   offset 24  …          ownership ticket bytes,
                    //                          length = (header_outer +
                    //                          ownership.size()), no
                    //                          inner length prefix —
                    //                          length is implicit from
                    //                          the outer pcbTicket.
                    body.reserve(24 + ownership.size());
                    body.resize(24, 0);
                    auto put_u32 = [&](size_t off, uint32_t v) {
                        body[off + 0] = static_cast<uint8_t>(v       & 0xFF);
                        body[off + 1] = static_cast<uint8_t>((v >> 8)  & 0xFF);
                        body[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
                        body[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
                    };
                    put_u32(0,  20);                            // fixed prefix
                    put_u32(4,  0);                             // padding
                    put_u32(8,  0);                             // padding
                    put_u32(12, h);                             // ConnectionID
                    put_u32(16, static_cast<uint32_t>(::time(nullptr)));
                    put_u32(20, 1);                             // ConnectionCount
                    body.insert(body.end(), ownership.begin(), ownership.end());
                    cm_backed = true;
                }
            }
        }
        if (!cm_backed) {
            // Synthetic body: 4B magic "WNAT" + 4B handle + 8B steam_id +
            // 8B unix-ts + 8B reserved. 32 bytes.
            body.assign(32, 0);
            body[0] = 'W'; body[1] = 'N'; body[2] = 'A'; body[3] = 'T';
            std::memcpy(body.data() + 4,  &h, sizeof(h));
            uint64_t sid = pushed().steam_id.load();
            std::memcpy(body.data() + 8,  &sid, sizeof(sid));
            uint64_t ts = static_cast<uint64_t>(::time(nullptr));
            std::memcpy(body.data() + 16, &ts, sizeof(ts));
        }
        // Cache the ticket so EndAuthSession / CancelAuthTicket can
        // clean up by handle.
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            pushed().auth_tickets[h] = {h, app_id, body};
        }
        // Copy bytes back to caller. cap to maxLen (SDK contract:
        // never overrun the buffer; pcbTicket carries the truth).
        if (buf && maxLen > 0) {
            uint32_t copy = std::min<uint32_t>(body.size(), static_cast<uint32_t>(maxLen));
            std::memcpy(buf, body.data(), copy);
        }
        if (pcbTicket) *pcbTicket = static_cast<uint32_t>(body.size());
        // Emit GetAuthSessionTicketResponse_t(h_ticket, eresult=1 OK).
        lsc_cb::GetAuthSessionTicketResponse cb{};
        cb.m_hAuthTicket = h;
        cb.m_eResult     = 1;  // k_EResultOK
        push_callback(state().user.load(),
                      lsc_cb::kGetAuthSessionTicketResponse,
                      &cb, sizeof(cb));
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "GetAuthSessionTicket(app=%u) → h=%u size=%zu (%s)",
            app_id, h, body.size(), cm_backed ? "CM-backed" : "synthetic");
        return h;
    }
    // 14 — GetAuthTicketForWebApi(pchIdentity). Web-API variant of
    //   GetAuthSessionTicket. Same generation pipeline (CM-cached
    //   ownership ticket → 24-byte SteamKit2 header → emit), but the
    //   ticket bytes are returned inline in the
    //   GetTicketForWebApiResponse_t callback (k_iCallback=168) rather
    //   than copied through an out-buffer the way slot 13 does. The
    //   pchIdentity binds the ticket to a specific destination (e.g.
    //   "myWebApi"); games use it to scope tickets server-side.
    virtual uint64_t  GetAuthTicketForWebApi(const char* pchIdentity) {
        uint32_t app_id = pushed().app_id.load();
        // Don't gate on app_id == 0 — web-API tickets can be account-
        // scoped (no app binding). Use 0 as the app key in that case.
        std::vector<uint8_t> ownership;
        if (app_id != 0) {
            size_t need = 0;
            wn_cm_get_cached_app_ownership_ticket(app_id, nullptr, 0, &need);
            if (need > 0 && need <= 16 * 1024) {
                ownership.resize(need);
                size_t got = 0;
                if (!wn_cm_get_cached_app_ownership_ticket(
                        app_id, ownership.data(), ownership.size(), &got)
                    || got != need) {
                    ownership.clear();
                }
            }
        }
        bool have_cm = !ownership.empty();
        uint32_t h = static_cast<uint32_t>(alloc_api_call_handle() & 0xFFFFFFFF);
        if (h == 0) h = static_cast<uint32_t>(alloc_api_call_handle() & 0xFFFFFFFF);
        std::vector<uint8_t> body;
        if (have_cm) {
            body.reserve(24 + ownership.size());
            body.resize(24, 0);
            auto put_u32 = [&](size_t off, uint32_t v) {
                body[off + 0] = static_cast<uint8_t>(v       & 0xFF);
                body[off + 1] = static_cast<uint8_t>((v >> 8)  & 0xFF);
                body[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
                body[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
            };
            put_u32(0,  20);                            // fixed prefix
            put_u32(12, h);                             // ConnectionID
            put_u32(16, static_cast<uint32_t>(::time(nullptr)));
            put_u32(20, 1);                             // ConnectionCount
            body.insert(body.end(), ownership.begin(), ownership.end());
        } else {
            // Synthetic body — same shape as GetAuthSessionTicket's
            // fallback path: 4B magic "WNAW" + 4B handle + 8B steamid
            // + 8B unix-ts + 8B reserved (32 bytes). Distinct magic so
            // a debugger can tell which slot the tickets came from.
            body.assign(32, 0);
            body[0] = 'W'; body[1] = 'N'; body[2] = 'A'; body[3] = 'W';
            std::memcpy(body.data() + 4,  &h, sizeof(h));
            uint64_t sid = pushed().steam_id.load();
            std::memcpy(body.data() + 8,  &sid, sizeof(sid));
            uint64_t ts = static_cast<uint64_t>(::time(nullptr));
            std::memcpy(body.data() + 16, &ts, sizeof(ts));
        }
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            pushed().auth_tickets[h] = {h, app_id, body};
        }
        // Emit GetTicketForWebApiResponse_t with the bytes inline. We
        // cap to the struct's 2560-byte ticket field (the SDK caps
        // there too — anything longer is rejected by Steam server-side).
        lsc_cb::GetTicketForWebApiResponse cb{};
        cb.m_hAuthTicket = h;
        cb.m_eResult     = 1; // k_EResultOK
        size_t copy = std::min<size_t>(body.size(), sizeof(cb.m_rgubTicket));
        cb.m_cubTicket = static_cast<int32_t>(copy);
        std::memcpy(cb.m_rgubTicket, body.data(), copy);
        push_callback(state().user.load(),
                      lsc_cb::kGetTicketForWebApiResponse,
                      &cb, sizeof(cb));
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "GetAuthTicketForWebApi(identity=\"%s\") → h=%u size=%zu (%s)",
            pchIdentity ? pchIdentity : "(null)", h, body.size(),
            have_cm ? "CM-backed" : "synthetic");
        return h;
    }
    // 15 — BeginAuthSession(ticket, cbTicket, steamID) — game-server
    //   path. Validates an inbound ticket. We don't have a real CM
    //   validation surface yet; emit ValidateAuthTicketResponse_t with
    //   eAuthSessionResponse=0 (OK) so callers that gate on the
    //   callback proceed, but log the call so a future real-validation
    //   wiring can find the call sites. Returns 0 (k_EBeginAuthSessionResultOK).
    virtual int       BeginAuthSession(const void* /*ticket*/, int cbTicket,
                                        uint64_t steamID) {
        lsc_cb::ValidateAuthTicketResponse cb{};
        cb.m_SteamID              = steamID;
        cb.m_eAuthSessionResponse = 0;  // k_EAuthSessionResponseOK
        cb.m_OwnerSteamID         = steamID;  // owner == user when not family-shared
        push_callback(state().user.load(),
                      lsc_cb::kValidateAuthTicketResponse,
                      &cb, sizeof(cb));
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "BeginAuthSession(cbTicket=%d, steamID=%llu) -> OK (synthetic validation)",
            cbTicket, static_cast<unsigned long long>(steamID));
        return 0;
    }
    // 16 — EndAuthSession(steamID) — release server-side state for the
    //   given user. No-op here (we don't track server-side state).
    virtual void      EndAuthSession(uint64_t)                       {}
    // 17 — CancelAuthTicket(hAuthTicket) — drop the cached client-side
    //   ticket so subsequent BeginAuthSession against it fails.
    virtual void      CancelAuthTicket(uint64_t hAuthTicket) {
        std::lock_guard<std::mutex> lk(state_mutex());
        pushed().auth_tickets.erase(static_cast<uint32_t>(hAuthTicket));
    }
    // 18 — UserHasLicenseForApp(steamID, appID). EUserHasLicenseForApp
    //   Result: 0=HasLicense, 1=DoesNotHaveLicense, 2=NoAuth.
    //   When steamID matches the signed-in user we can answer
    //   authoritatively from owned_apps; for other users we have no
    //   ownership signal so return NoAuth (the SDK's "we have not
    //   verified" answer — different semantically from
    //   DoesNotHaveLicense, which is a positive negative).
    virtual int       UserHasLicenseForApp(uint64_t steamID, uint32_t appID) {
        if (appID == 0) return 2;
        uint64_t self = pushed().steam_id.load();
        if (steamID != 0 && steamID == self) {
            std::lock_guard<std::mutex> lk(state_mutex());
            return pushed().owned_apps.count(appID) > 0 ? 0 : 1;
        }
        return 2; /*NoAuth — we can't speak for other users*/
    }
    // 19 — BIsBehindNAT. Modern multiplayer games use this to decide
    //   whether to surface a "Steam Datagram Relay required" hint or
    //   to attempt direct P2P first. Returning false makes games think
    //   they can reach peers via direct UDP without traversal, which
    //   is wrong for every Android device we ship to (always on
    //   carrier/Wi-Fi NAT). Return true unconditionally so games
    //   default to relay-aware code paths. A future enhancement could
    //   probe the local default-gateway address via ConnectivityManager
    //   and only return true when the wan address is RFC-1918 / CGNAT;
    //   for now the always-true answer is safer than always-false.
    virtual bool      BIsBehindNAT()                                 { return true; }  // 19
    virtual void      AdvertiseGame(uint64_t, uint32_t, uint16_t)    {}              // 20
    // 21 — RequestEncryptedAppTicket(rgubDataToInclude, cbDataToInclude)
    //   Async fetch of the per-app encrypted ticket Steam issues for
    //   server-side validation. Returns SteamAPICall_t hCall; the
    //   matching EncryptedAppTicketResponse_t (id 154) fires after
    //   the result lands. Today the body bytes come from one of two
    //   sources:
    //     (a) Kotlin pre-pushed via nativeSetEncryptedAppTicket (wn-
    //         session's real CMsgClientRequestEncryptedAppTicket
    //         round-trip wrote them).
    //     (b) Synthetic placeholder when (a) isn't populated yet —
    //         lets offline / pre-logon callers exercise the call-result
    //         dispatch chain.
    //   Cached in pushed.encrypted_app_tickets[app_id] so a follow-on
    //   GetEncryptedAppTicket serves the same bytes.
    virtual uint64_t  RequestEncryptedAppTicket(void* rgubData, int cbData) {
        uint32_t app = pushed().app_id.load();
        uint64_t h   = alloc_api_call_handle();
        bool     have_real_bytes = false;
        std::vector<uint8_t> body;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().encrypted_app_tickets.find(app);
            if (it != pushed().encrypted_app_tickets.end() && !it->second.empty()) {
                body = it->second;
                have_real_bytes = true;
            } else {
                // Synthetic placeholder: 16B "WNETKT" + 4B app_id +
                // 4B handle (hCall low) + 8B steam_id + cbData copy.
                body.resize(32);
                std::memcpy(body.data(),      "WNETKT\0\0\0\0\0\0\0\0\0\0", 16);
                std::memcpy(body.data() + 16, &app, sizeof(app));
                uint32_t h32 = static_cast<uint32_t>(h);
                std::memcpy(body.data() + 20, &h32, sizeof(h32));
                uint64_t sid = pushed().steam_id.load();
                std::memcpy(body.data() + 24, &sid, sizeof(sid));
                pushed().encrypted_app_tickets[app] = body;
            }
        }
        int32_t eresult = have_real_bytes ? 1 : 1;  // SDK contract: synthetic returns OK
        pushed().encrypted_app_ticket_eresult.store(eresult);
        // Push the EncryptedAppTicketResponse_t as a CallResult body
        // so observers registered via SteamAPI_RegisterCallResult
        // get the answer on the next RunCallbacks. (void)rgubData/
        // cbData — Steam tracks them in the request but our synthetic
        // path doesn't echo them back.
        (void)rgubData; (void)cbData;
        lsc_cb::EncryptedAppTicketResponse cb{};
        cb.m_eResult = eresult;
        push_call_result(h, lsc_cb::kEncryptedAppTicketResponse,
                         &cb, sizeof(cb), /*io_failure=*/false);
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "RequestEncryptedAppTicket(app=%u) -> hCall=%llu (body=%zu B, %s)",
            app, static_cast<unsigned long long>(h), body.size(),
            have_real_bytes ? "real" : "synthetic");
        return h;
    }
    // 22 — GetEncryptedAppTicket(buf, cbMax, *pcbTicket)
    //   Serve back the cached ticket bytes for the currently-bound app.
    //   Returns false if no ticket has landed yet (the typical pattern
    //   has the caller wait for EncryptedAppTicketResponse_t first).
    virtual bool      GetEncryptedAppTicket(void* buf, int cbMax, uint32_t* pcbTicket) {
        uint32_t app = pushed().app_id.load();
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().encrypted_app_tickets.find(app);
        if (it == pushed().encrypted_app_tickets.end() || it->second.empty()) {
            if (pcbTicket) *pcbTicket = 0;
            return false;
        }
        const auto& body = it->second;
        uint32_t copy = std::min<uint32_t>(body.size(),
                                            static_cast<uint32_t>(std::max(0, cbMax)));
        if (buf && copy > 0) std::memcpy(buf, body.data(), copy);
        if (pcbTicket) *pcbTicket = static_cast<uint32_t>(body.size());
        return true;
    }
    // 23 — GetGameBadgeLevel(nSeries, bFoil). Steam Trading-Card badge
    //   tier for the bound app. nSeries indexes the badge edition
    //   (most apps only have series=1; bFoil distinguishes the foil
    //   variant). Encode key as (series<<1)|foil so both series and
    //   the foil flag survive the int32 map key. 0 = no badge owned
    //   (matches SDK contract).
    virtual int       GetGameBadgeLevel(int nSeries, bool bFoil) {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return 0;
        int32_t key = (static_cast<int32_t>(app) & 0x0FFFFFFF)
                    | ((nSeries & 0x07) << 28)
                    | (bFoil ? (1 << 31) : 0);
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().self_game_badges.find(key);
        return it == pushed().self_game_badges.end() ? 0 : it->second;
    }
    // 24 — GetPlayerSteamLevel. Self profile XP level pushed from
    //   CPlayer.GetSteamLevel response. 0 = not yet queried.
    virtual int       GetPlayerSteamLevel() {
        return pushed().self_player_level.load();
    }
    // 25 — RequestStoreAuthURL(pchRedirectURL). Async — fetches a
    //   short-lived store-auth URL the game can hand off to a webview
    //   to land the user logged-in on store.steampowered.com. Result
    //   delivered via StoreAuthURLResponse_t (k_iCallback=165, 512B
    //   fixed-buf URL).
    //
    //   We don't have a CM round-trip for this yet; emit a synthetic
    //   response with a URL pattern that includes the redirect target.
    //   Games that gate on the callback firing will proceed; the URL
    //   won't actually log the user in but the cb shape is honest.
    virtual uint64_t RequestStoreAuthURL(const char* pchRedirectURL) {
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::StoreAuthURLResponse cb{};
        const char* redirect = pchRedirectURL ? pchRedirectURL : "";
        std::snprintf(cb.m_szURL, sizeof(cb.m_szURL),
                      "https://store.steampowered.com/login/?redir=%s",
                      redirect);
        push_call_result(hCall, lsc_cb::kStoreAuthURLResponse,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 26-29 — account-info flags driven from CMsgClientAccountInfo. The
    //   store overlay's marketplace flow + family-share / parental
    //   controls UI gate on these; returning the wrong value pretends
    //   the user can't trade / participate even when they actually can.
    virtual bool      BIsPhoneVerified() {
        return pushed().account_phone_verified.load();
    }
    virtual bool      BIsTwoFactorEnabled() {
        return pushed().account_two_factor_enabled.load();
    }
    virtual bool      BIsPhoneIdentifying() {
        return pushed().account_phone_identifying.load();
    }
    virtual bool      BIsPhoneRequiringVerification() {
        return pushed().account_phone_requires_verification.load();
    }
    // 30 — GetMarketEligibility. Async — server-side checks whether
    //   the user can trade on the marketplace. Result via
    //   MarketEligibilityResponse_t (k_iCallback=166).
    //
    //   We post an "allowed if 2FA on and phone verified" approximation
    //   from pushed account flags. Real Steam checks against
    //   server-side account-state (trade holds, parental locks, etc.)
    //   we don't track; this is the most honest local approximation.
    virtual uint64_t GetMarketEligibility() {
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::MarketEligibilityResponse cb{};
        bool twoFA = pushed().account_two_factor_enabled.load();
        bool phone = pushed().account_phone_verified.load();
        cb.m_bAllowed = (twoFA && phone);
        // EMarketNotAllowedReasons bitfield. 0 = none. We set
        // k_EMarketNotAllowedReason_AccountNotTrusted (bit 1) when 2FA
        // is off — a meaningful signal so the UI can prompt the user.
        cb.m_eNotAllowedReason             = cb.m_bAllowed ? 0 : 2;
        cb.m_rtAllowedAtTime               = 0;
        cb.m_cdaySteamGuardRequiredDays    = cb.m_bAllowed ? 0 : 15;
        cb.m_cdayNewDeviceCooldown         = 0;
        push_call_result(hCall, lsc_cb::kMarketEligibilityResponse,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 31 — GetDurationControl. China duration-control async query.
    //   Result via DurationControl_t (k_iCallback=167). For non-CN
    //   accounts the SDK contract is m_bApplicable=false + all counters
    //   zero — the UI then knows to hide its CN-only banners.
    virtual uint64_t GetDurationControl() {
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::DurationControl cb{};
        cb.m_eResult        = 1; // k_EResultOK
        cb.m_appid          = pushed().app_id.load();
        cb.m_bApplicable    = false; // non-CN
        cb.m_csecsLast5h    = 0;
        cb.m_progress       = 0;     // k_EDurationControlProgress_Full
        cb.m_notification   = 0;     // k_EDurationControlNotification_None
        cb.m_csecsToday     = 0;
        cb.m_csecsRemaining = 0;
        push_call_result(hCall, lsc_cb::kDurationControl,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 32 — BSetDurationControlOnlineState. China duration-control hook.
    //   Returning false marks the call as failed and games show an
    //   "online-mode unavailable" banner; non-CN clients always succeed
    //   (the call is essentially a no-op outside CN). Return true.
    virtual bool      BSetDurationControlOnlineState(int /*state*/)  { return true; }  // 32
};

// ---------------------------------------------------------------------------
// ISteamApps (version "STEAMAPPS_INTERFACE_VERSION009"). Slot 6 =
// BIsSubscribedApp, slot 17 = GetInstalledDepots, slot 18 =
// GetAppInstallDir, slot 19 = BIsAppInstalled, slot 20 = GetAppOwner.
class ISteamAppsStub {
public:
    // Helper: does the SteamAppId env var (set by WnSteamBootstrap pre-
    // dlopen) match the queried app id? Used by BIsSubscribed / BIs
    // SubscribedApp as a fallback when the async CMsgClientLicenseList
    // hasn't yet populated pushed().owned_apps. Read the env each call
    // — cheap (one POSIX-getenv + atoi), and a future relaunch in the
    // same process would change it.
    static bool env_app_id_matches(uint32_t app) {
        if (app == 0) return false;
        const char* env = std::getenv("SteamAppId");
        if (!env || !*env) return false;
        char* end = nullptr;
        unsigned long v = std::strtoul(env, &end, 10);
        return (end != env && v == app);
    }

    // 0 — BIsSubscribed. "Does the user own the bound app?" Anti-piracy
    //   gates fire on a false return — games that boot, immediately
    //   check this, and quit. Resolves via owned_apps set.
    //
    //   Env fallback: when owned_apps doesn't yet contain the bound app
    //   (the CMsgClientLicenseList push lands asynchronously, racing
    //   with early-boot game queries — observed in Bionic bootstrap
    //   Stage2 diagnostic returning 0 immediately after LogonWith
    //   RefreshToken), accept the SteamAppId env var as proof. The
    //   bootstrap set that env BEFORE dlopen, so its presence is a
    //   reliable signal that the user owns the game (we wouldn't have
    //   launched it otherwise). Only triggers when the env value
    //   matches the bound app id — never lies about apps the game
    //   didn't actually request.
    virtual bool      BIsSubscribed() {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            if (pushed().owned_apps.count(app) > 0) return true;
        }
        return env_app_id_matches(app);
    }
    // 1 — BIsLowViolence. Region-specific low-violence content gate
    //   (German market historically). Flag-driven from pushed state.
    virtual bool      BIsLowViolence() {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        return pushed().app_low_violence.count(app) > 0;
    }
    // 2 — BIsCybercafe. Legacy cybercafe-license flag; the licensing
    //   model retired around 2015 but the SDK retains the slot.
    //   Always false — we never issue cybercafe licenses.
    virtual bool      BIsCybercafe()                                 { return false; } // 2
    // 3 — BIsVACBanned. VAC-ban status for the bound app's anti-cheat
    //   module. Set via app_vac_banned (pushed at logon from CMsgClient
    //   VACResponse if the account ever lit one). Default false.
    virtual bool      BIsVACBanned() {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        return pushed().app_vac_banned.count(app) > 0;
    }
    // 4 — GetCurrentGameLanguage. Reads pushed.ui_language; default
    //   "english" when empty (the Steam Client's documented fallback).
    //   Thread-local stable buffer so the returned const char* survives
    //   until the next call on the same thread.
    virtual const char* GetCurrentGameLanguage() {
        static thread_local std::string tls_lang;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            tls_lang = pushed().ui_language;
        }
        if (tls_lang.empty()) tls_lang = "english";
        return tls_lang.c_str();
    }
    virtual const char* GetAvailableGameLanguages()                  { return "english"; } // 5
    virtual bool      BIsSubscribedApp(uint32_t appId)               {                 // 6
        {
            auto& p = pushed();
            std::lock_guard<std::mutex> lk(state_mutex());
            if (p.owned_apps.count(appId) > 0) return true;
        }
        return env_app_id_matches(appId);
    }
    virtual bool      BIsDlcInstalled(uint32_t appId)                {                 // 7
        // DLC contract: report true iff we OWN the DLC AND its content
        // is on disk. Content presence has two shapes — DLCs that ship
        // as their own appid land in installed_apps directly, while
        // DLCs that piggyback on the parent's depots are "installed"
        // whenever the parent base app is installed. Search app_dlcs
        // for any parent that lists this id; if that parent is
        // installed, treat the DLC as installed too. Anything else
        // (unowned, or installed via neither path) → false.
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        if (p.installed_apps.count(appId) > 0 &&
            p.owned_apps.count(appId) > 0) {
            return true;
        }
        if (p.owned_apps.count(appId) == 0) return false;
        for (const auto& kv : p.app_dlcs) {
            for (const auto& d : kv.second) {
                if (d.app_id == appId &&
                    p.installed_apps.count(kv.first) > 0) {
                    return true;
                }
            }
        }
        return false;
    }
    // 8 — GetEarliestPurchaseUnixTime(app_id). Returns unix32 of the
    //   earliest license time_created across the app's source packages.
    //   0 = no licenses found (app not owned OR licenses not yet
    //   ingested). Games that gate veteran-rewards on age check this.
    virtual uint32_t GetEarliestPurchaseUnixTime(uint32_t app_id) {
        if (app_id == 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto pit = pushed().app_source_packages.find(app_id);
        if (pit == pushed().app_source_packages.end()) return 0;
        uint32_t earliest = 0;
        for (uint32_t pkg : pit->second) {
            auto lit = pushed().licenses.find(pkg);
            if (lit == pushed().licenses.end()) continue;
            uint32_t t = lit->second.time_created;
            if (t == 0) continue;
            if (earliest == 0 || t < earliest) earliest = t;
        }
        return earliest;
    }
    // 9 — BIsSubscribedFromFreeWeekend. Checks bound app's source
    //   packages for any with license_type==FreeWeekend (ELicenseType=11).
    //   Used by games to lock save-progress or DLC during free weekends.
    virtual bool BIsSubscribedFromFreeWeekend() {
        uint32_t app_id = pushed().app_id.load();
        if (app_id == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto pit = pushed().app_source_packages.find(app_id);
        if (pit == pushed().app_source_packages.end()) return false;
        for (uint32_t pkg : pit->second) {
            auto lit = pushed().licenses.find(pkg);
            if (lit == pushed().licenses.end()) continue;
            if (lit->second.license_type == 11 /*FreeWeekend*/) return true;
        }
        return false;
    }
    // 10 — GetDLCCount(appId). Returns the number of DLC entries we
    //   know for the parent app. Source: pushed.app_dlcs (PICS-cached
    //   listofdlc + wn-session library snapshot).
    virtual int       GetDLCCount(uint32_t appId) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_dlcs.find(appId);
        return it == pushed().app_dlcs.end() ? 0 : static_cast<int>(it->second.size());
    }
    // 11 — BGetDLCDataByIndex(appId, iDLC, *pAppID, *pbAvailable,
    //                         pchName, cchNameBufferSize)
    //   Fill the out-params with the DLC at [iDLC]. Returns false on
    //   bounds error. name buffer is null-terminated and truncated to
    //   cchNameBufferSize-1 — the SDK contract.
    virtual bool      BGetDLCDataByIndex(uint32_t appId, int iDLC,
                                          uint32_t* pAppID, bool* pbAvailable,
                                          char* pchName, int cchNameBufferSize) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_dlcs.find(appId);
        if (it == pushed().app_dlcs.end()) return false;
        const auto& dlcs = it->second;
        if (iDLC < 0 || static_cast<size_t>(iDLC) >= dlcs.size()) return false;
        const auto& d = dlcs[static_cast<size_t>(iDLC)];
        if (pAppID)      *pAppID      = d.app_id;
        if (pbAvailable) *pbAvailable = d.available;
        if (pchName && cchNameBufferSize > 0) {
            int copy = std::min<int>(static_cast<int>(d.name.size()),
                                      cchNameBufferSize - 1);
            if (copy > 0) std::memcpy(pchName, d.name.data(), copy);
            pchName[copy] = '\0';
        }
        return true;
    }
    virtual void      InstallDLC(uint32_t)                           {}                // 12
    virtual void      UninstallDLC(uint32_t)                         {}                // 13
    virtual void      RequestAppProofOfPurchaseKey(uint32_t)         {}                // 14
    // 15 — GetCurrentBetaName(pchName, cchNameBufferSize). Returns true
    //   if the bound app has a non-empty beta branch selected, writing
    //   a NUL-terminated name into the caller's buffer (truncated to
    //   fit, always NUL-terminated). Returns false on no bound app, no
    //   beta selected, or invalid buffer.
    virtual bool      GetCurrentBetaName(char* pchName, int cchNameBufferSize) {
        if (!pchName || cchNameBufferSize <= 0) return false;
        uint32_t app_id = pushed().app_id.load();
        if (app_id == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_current_beta.find(app_id);
        if (it == pushed().app_current_beta.end()) return false;
        const std::string& name = it->second;
        if (name.empty()) return false;
        size_t copy = std::min<size_t>(name.size(),
                                       static_cast<size_t>(cchNameBufferSize - 1));
        std::memcpy(pchName, name.data(), copy);
        pchName[copy] = '\0';
        return true;
    }
    // 16 — MarkContentCorrupt(bMissingFilesOnly). Asks Steam to
    //   revalidate the bound app's depots. We don't trigger an
    //   immediate revalidation here (that's a downloader job the
    //   wn-session layer owns), but we record the request in
    //   apps_marked_corrupt so a future SteamService poll can schedule
    //   one — and we return true (matches "request accepted") rather
    //   than silently dropping it.
    virtual bool      MarkContentCorrupt(bool /*bMissingFilesOnly*/) {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        pushed().apps_marked_corrupt.insert(app);
        return true;
    }
    // 17 — GetInstalledDepots(appID, *pvecDepots, cMaxDepots)
    //   Fills the caller's array with depot ids for the bound app.
    //   Returns the number actually written. Caller can probe-size
    //   by calling with cMaxDepots=0 (or pvecDepots=null) — we still
    //   return 0 in that case, matching Valve's "you have to ask for
    //   at least one slot" contract.
    virtual uint32_t  GetInstalledDepots(uint32_t appID, uint32_t* pvecDepots,
                                          uint32_t cMaxDepots) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_installed_depots.find(appID);
        if (it == pushed().app_installed_depots.end()) return 0;
        const auto& depots = it->second;
        uint32_t copy = std::min<uint32_t>(static_cast<uint32_t>(depots.size()),
                                            cMaxDepots);
        if (pvecDepots && copy > 0) {
            for (uint32_t i = 0; i < copy; ++i) pvecDepots[i] = depots[i];
        }
        return copy;
    }
    virtual uint32_t  GetAppInstallDir(uint32_t appId, char* buf, uint32_t cap) {       // 18
        if (!buf || cap == 0) return 0;
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = p.app_install_dirs.find(appId);
        if (it == p.app_install_dirs.end()) {
            buf[0] = '\0';
            return 0;
        }
        const std::string& d = it->second;
        uint32_t n = static_cast<uint32_t>(d.size());
        uint32_t copy = (n + 1 < cap) ? n : cap - 1;
        std::memcpy(buf, d.data(), copy);
        buf[copy] = '\0';
        return n + 1;  // documented: returns length INCLUDING null
    }
    virtual bool      BIsAppInstalled(uint32_t appId)                {                 // 19
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        return p.installed_apps.count(appId) > 0;
    }
    // 20 — GetAppOwner. Returns the SteamID of the user who owns the
    //   bound app. For directly-licensed apps this equals self
    //   (pushed.steam_id). For family-shared apps this is the owner's
    //   SteamID composed from the license owner_id + standard
    //   individual-account CSteamID bits (universe=Public(1),
    //   account_type=Individual(1), instance=Desktop(1)):
    //     SteamID64 = 0x0110000100000000 | (account_id & 0xFFFFFFFF)
    //
    //   Resolution: walk source packages, prefer a self-owned match
    //   if any (game is directly owned through some package), else
    //   return the first non-self owner's SteamID. Falls back to self
    //   when license data isn't loaded yet.
    //
    //   Returns 0 when neither owned_apps nor app_source_packages
    //   knows about the bound app — matches Steam's "not owned"
    //   sentinel.
    virtual uint64_t GetAppOwner() {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return 0;
        uint64_t self_sid = pushed().steam_id.load();
        if (self_sid == 0) return 0;
        uint32_t self_account = static_cast<uint32_t>(self_sid & 0xFFFFFFFFu);
        std::lock_guard<std::mutex> lk(state_mutex());
        bool in_owned = pushed().owned_apps.count(app) > 0;
        auto pit = pushed().app_source_packages.find(app);
        bool has_pkgs = (pit != pushed().app_source_packages.end());
        if (!in_owned && !has_pkgs) return 0;
        if (!has_pkgs) return self_sid;  // owned but no pkg map — assume self
        // Walk source packages for the first non-self owner; if any
        // package is self-owned, the game is direct-licensed.
        uint32_t fallback_owner = 0;
        bool has_self_match     = false;
        for (uint32_t pkg : pit->second) {
            auto lit = pushed().licenses.find(pkg);
            if (lit == pushed().licenses.end()) continue;
            if (lit->second.owner_id == self_account) {
                has_self_match = true;
                break;
            }
            if (fallback_owner == 0) fallback_owner = lit->second.owner_id;
        }
        if (has_self_match) return self_sid;
        if (fallback_owner != 0) {
            return 0x0110000100000000ULL |
                   static_cast<uint64_t>(fallback_owner);
        }
        // License data not yet loaded for these packages — fall back to
        // self so games that read this BEFORE the owner gets resolved
        // see "you own this" rather than "not owned" (better degradation;
        // re-read after CMsgClientLicenseList arrives picks up truth).
        return self_sid;
    }
    virtual const char* GetLaunchQueryParam(const char*)             { return ""; }    // 21
    // 22 — GetDlcDownloadProgress(appID, *bytesDownloaded, *bytesTotal).
    //   Returns true while a download is actively in flight for the
    //   given app id, writing the progress counters. Returns false when
    //   no download exists, the download has finished, or bytesTotal
    //   is zero. Steam launcher overlays and progress bars poll this
    //   during install / DLC fetch.
    virtual bool      GetDlcDownloadProgress(uint32_t appID, uint64_t* pBytesDownloaded,
                                              uint64_t* pBytesTotal) {
        if (appID == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_dl_progress.find(appID);
        if (it == pushed().app_dl_progress.end()) return false;
        if (it->second.bytes_total == 0) return false;
        if (pBytesDownloaded) *pBytesDownloaded = it->second.bytes_downloaded;
        if (pBytesTotal)      *pBytesTotal      = it->second.bytes_total;
        return true;
    }
    // 23 — GetAppBuildId. Returns the PICS public-branch buildid for
    //   the bound app, or 0 if PICS hasn't resolved one. Sourced from
    //   pushed.app_build_ids (populated at game-launch from
    //   wn-session's library snapshot).
    virtual int       GetAppBuildId() {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_build_ids.find(app);
        return it == pushed().app_build_ids.end() ? 0 : static_cast<int>(it->second);
    }
    virtual void      RequestAllProofOfPurchaseKeys()                {}                // 24
    // 25 — GetFileDetails(filename). Async — fills FileDetailsResult_t
    //   with the bound app's binary file's size + SHA-1 digest.
    //   Anti-tamper code paths use this to verify game executables
    //   haven't been modified. The path is relative to the app's
    //   install dir (NOT the cloud remote dir — different ISteam*).
    //
    //   Missing file / no bound app / no install dir → FileNotFound.
    //   The SHA we report is a FNV-1a 64 of the bytes splayed into
    //   the 20-byte buffer — a stable per-content fingerprint games
    //   can use for change detection without us shipping a real SHA-1
    //   implementation. Real Steam-side validation needs true SHA-1,
    //   but the SDK-contract behavior (cb fires + CCallResult
    //   resolves) is preserved.
    virtual uint64_t GetFileDetails(const char* pchFile) {
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::FileDetailsResult cb{};
        uint32_t app = pushed().app_id.load();
        std::string base;
        if (app != 0 && pchFile && *pchFile) {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().app_install_dirs.find(app);
            if (it != pushed().app_install_dirs.end()) base = it->second;
        }
        if (base.empty()) {
            cb.m_eResult = 9; // k_EResultFileNotFound
            push_call_result(hCall, lsc_cb::kFileDetailsResult,
                             &cb, sizeof(cb), /*io_failure=*/false);
            return hCall;
        }
        // Reject path-traversal — game binaries live under base, no
        // escape outside the install dir.
        std::string fname(pchFile);
        if (fname.find("..") != std::string::npos || fname[0] == '/') {
            cb.m_eResult = 9;
            push_call_result(hCall, lsc_cb::kFileDetailsResult,
                             &cb, sizeof(cb), /*io_failure=*/false);
            return hCall;
        }
        std::string path = base;
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path.append(fname);
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            cb.m_eResult = 9;
            push_call_result(hCall, lsc_cb::kFileDetailsResult,
                             &cb, sizeof(cb), /*io_failure=*/false);
            return hCall;
        }
        // Read whole file into memory + hash. Cloud files are bounded
        // by Steam's per-file size limit (256 MB hard cap) but we cap
        // at 64 MB defensively to avoid OOM on a malformed mirror.
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size > 64LL * 1024 * 1024) {
            ::close(fd);
            cb.m_eResult = 2; // k_EResultFail
            push_call_result(hCall, lsc_cb::kFileDetailsResult,
                             &cb, sizeof(cb), /*io_failure=*/false);
            return hCall;
        }
        uint64_t h64 = 0xcbf29ce484222325ULL;
        uint8_t buf[8192];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                h64 ^= buf[i];
                h64 *= 0x100000001b3ULL;
            }
        }
        ::close(fd);
        cb.m_eResult    = 1; // k_EResultOK
        cb.m_ulFileSize = static_cast<uint64_t>(st.st_size);
        // Splay the FNV-1a hash across the 20-byte SHA field. First
        // 8 bytes = the hash itself, next 8 bytes = its rotation,
        // last 4 bytes = the high u32. Stable per-content; not a
        // cryptographic SHA-1.
        std::memcpy(cb.m_FileSHA + 0,  &h64, sizeof(h64));
        uint64_t h64_rot = (h64 << 32) | (h64 >> 32);
        std::memcpy(cb.m_FileSHA + 8,  &h64_rot, sizeof(h64_rot));
        uint32_t h32 = static_cast<uint32_t>(h64 ^ h64_rot);
        std::memcpy(cb.m_FileSHA + 16, &h32, sizeof(h32));
        cb.m_unFlags = 0;
        push_call_result(hCall, lsc_cb::kFileDetailsResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 26 — GetLaunchCommandLine(buf, cubMax). Returns the bound app's
    //   command-line as a null-terminated string. Returns the byte
    //   count copied (matches the SDK signature — int, not size_t).
    //   Empty when no command-line has been pushed.
    virtual int       GetLaunchCommandLine(char* buf, int cubMax) {
        if (!buf || cubMax <= 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        const std::string& cl = pushed().launch_command_line;
        int n = static_cast<int>(cl.size());
        int copy = std::min(n, cubMax - 1);
        if (copy > 0) std::memcpy(buf, cl.data(), copy);
        buf[copy] = '\0';
        return copy;
    }
    // 27 — BIsSubscribedFromFamilySharing. Dynamic resolution through
    //   the license-list observer's data:
    //     for each source package of pushed.app_id:
    //       if licenses[pkg].owner_id == self_account_id → directly owned
    //     return true iff NONE were self-owned
    //   Falls back to the Kotlin-set pushed.app_is_family_shared boolean
    //   when license data isn't loaded yet (cold boot, signed-out, or
    //   the app's source packages aren't in the map). The boolean was
    //   the pre-license-observer wiring; the new path is preferred
    //   because it's per-call live (handles family-share toggles
    //   without a fresh game launch).
    virtual bool BIsSubscribedFromFamilySharing() {
        uint32_t app_id = pushed().app_id.load();
        if (app_id == 0) return pushed().app_is_family_shared.load();
        uint64_t sid = pushed().steam_id.load();
        if (sid == 0) return pushed().app_is_family_shared.load();
        uint32_t self_account = static_cast<uint32_t>(sid & 0xFFFFFFFFu);
        std::lock_guard<std::mutex> lk(state_mutex());
        auto pit = pushed().app_source_packages.find(app_id);
        if (pit == pushed().app_source_packages.end()) {
            return pushed().app_is_family_shared.load();
        }
        bool any_license_match = false;
        bool any_self_owned    = false;
        for (uint32_t pkg : pit->second) {
            auto lit = pushed().licenses.find(pkg);
            if (lit == pushed().licenses.end()) continue;
            any_license_match = true;
            if (lit->second.owner_id == self_account) {
                any_self_owned = true;
                break;
            }
        }
        if (!any_license_match) {
            // License data hasn't arrived for any source package yet —
            // fall back to the boolean while waiting on CMsgClientLicenseList.
            return pushed().app_is_family_shared.load();
        }
        return !any_self_owned;
    }
    // 28 — BIsTimedTrial(*pcSecondsAllowed, *pcSecondsPlayed). True
    //   iff bound app's any source package has a minute_limit > 0
    //   (timed-trial license). Out params get the time in seconds
    //   (CMsgClientLicenseList carries minutes; multiply by 60).
    virtual bool BIsTimedTrial(uint32_t* pcSecondsAllowed,
                                uint32_t* pcSecondsPlayed) {
        uint32_t app = pushed().app_id.load();
        if (app == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto pit = pushed().app_source_packages.find(app);
        if (pit == pushed().app_source_packages.end()) return false;
        for (uint32_t pkg : pit->second) {
            auto lit = pushed().licenses.find(pkg);
            if (lit == pushed().licenses.end()) continue;
            if (lit->second.minute_limit > 0) {
                if (pcSecondsAllowed) {
                    *pcSecondsAllowed = static_cast<uint32_t>(
                        lit->second.minute_limit * 60);
                }
                if (pcSecondsPlayed) {
                    *pcSecondsPlayed = static_cast<uint32_t>(
                        std::max(0, lit->second.minutes_used) * 60);
                }
                return true;
            }
        }
        return false;
    }
    // 29 — SetDlcContext(appID). Scopes future ISteamApps DLC queries
    //   to a specific DLC's app id (vs the bound app). The current
    //   surface (slot 11 BGetDLCDataByIndex etc.) already takes appId
    //   explicitly, so this is a no-op for our pipeline — just return
    //   true so callers don't treat it as an error.
    virtual bool      SetDlcContext(uint32_t /*appID*/)              { return true; }  // 29
};

// ---------------------------------------------------------------------------
// ISteamFriends (version "SteamFriends017"). Slot 0 = GetPersonaName,
// 2 = GetPersonaState, 3 = GetFriendCount, 4 = GetFriendByIndex,
// 7 = GetFriendPersonaName. Real Steam exposes ~85 slots; we stub
// the first 40 to cover what the SDK headers' public ordering needs.
class ISteamFriendsStub {
public:
    virtual const char* GetPersonaName()                             {                    // 0
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        return p.persona_name.empty() ? "Player" : p.persona_name.c_str();
    }
    // 1 — SetPersonaName(name). Async — returns SteamAPICall_t hCall.
    //   Updates pushed.persona_name immediately (local-success path),
    //   fires PersonaStateChange_t(kPersonaChangeName), schedules the
    //   matching SetPersonaNameResponse_t (id 1332) as a CallResult,
    //   AND drives CMsgClientChangeStatus(player_name) through the
    //   cm_bridge so Steam friends actually see the rename. The CM
    //   call is fire-and-forget — the response comes back as a
    //   server-pushed CMsgClientPersonaState that CMClient's
    //   route_inbound_ caches and SteamService's persona observer
    //   re-pushes through nativeSetPersonaName.
    //
    //   bridge returns false → no active CMClient (cold boot / signed-
    //   out / hand-off-suspended); we keep the local cache + report
    //   m_bSuccess=logged_on so the caller's CallResult observer
    //   doesn't incorrectly think the CM rejected.
    virtual uint64_t  SetPersonaName(const char* pchPersonaName) {
        if (!pchPersonaName) return 0;
        uint64_t h = alloc_api_call_handle();
        std::string name(pchPersonaName);
        uint64_t self;
        bool name_changed;
        int current_state;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            name_changed = (pushed().persona_name != name);
            pushed().persona_name = name;  // copy: keep one for the bridge call below
            self = pushed().steam_id.load();
            current_state = pushed().persona_state.load();
        }
        if (name_changed && self != 0) {
            lsc_cb::PersonaStateChange psc{};
            psc.m_ulSteamID    = self;
            psc.m_nChangeFlags = lsc_cb::kPersonaChangeName;
            push_callback(state().user.load(),
                          lsc_cb::kPersonaStateChange, &psc, sizeof(psc));
        }
        // Broadcast to Steam via the cm_bridge. Best-effort — returns
        // false when no live CMClient. Persona state is required by
        // the CM API; pass the current cached state (default Online).
        wn_cm_set_persona_name(name.c_str(),
                               current_state > 0 ? current_state : 1);
        lsc_cb::SetPersonaNameResponse resp{};
        resp.m_bSuccess      = state().logged_on.load();
        resp.m_bLocalSuccess = true;
        resp.m_result        = state().logged_on.load() ? 1 : 6;  // OK / NoConnection
        push_call_result(h, lsc_cb::kSetPersonaNameResponse,
                         &resp, sizeof(resp), /*io_failure=*/false);
        return h;
    }
    virtual int       GetPersonaState()                              { return pushed().persona_state.load(); } // 2
    virtual int       GetFriendCount(int /*flags*/)                  {                    // 3
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        return static_cast<int>(p.friends.size());
    }
    virtual uint64_t  GetFriendByIndex(int idx, int /*flags*/)       {                    // 4
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        if (idx < 0 || static_cast<size_t>(idx) >= p.friends.size()) return 0;
        return p.friends[idx];
    }
    // 5 — GetFriendRelationship(sid). EFriendRelationship enum:
    //   0=None, 1=Blocked, 2=RequestRecipient, 3=Friend,
    //   4=RequestInitiator, 5=Ignored, 6=IgnoredFriend.
    //   We only track the mutual-friends list (CMsgClientFriendsList
    //   m_efriendrelationship==Friend entries), so the resolution is
    //   binary: in the list → 3/Friend; not in the list → 0/None.
    //   Pre-fix, every SID resolved to Friend — games that gate
    //   in-game invites or messaging on this would happily try to
    //   invite the wrong account.
    virtual int       GetFriendRelationship(uint64_t sid) {
        if (sid == 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (uint64_t f : pushed().friends) {
            if (f == sid) return 3 /*Friend*/;
        }
        return 0 /*None*/;
    }
    // 6 — GetFriendPersonaState(sid). Returns the EPersonaState
    //   (0=Offline … 7=Invisible) for the given friend. Reads from
    //   pushed.friend_persona_states; defaults to 0 (Offline) when
    //   no observation has arrived yet.
    virtual int       GetFriendPersonaState(uint64_t sid) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().friend_persona_states.find(sid);
        return it == pushed().friend_persona_states.end() ? 0 : static_cast<int>(it->second);
    }
    virtual const char* GetFriendPersonaName(uint64_t sid)           {                    // 7
        auto& p = pushed();
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = p.friend_persona_names.find(sid);
        return it == p.friend_persona_names.end() ? "" : it->second.c_str();
    }
    // 8 — GetFriendGamePlayed(sid, FriendGameInfo_t*). Fills the out
    //   struct's CGameID with the friend's currently-played appId (or
    //   0 when not in-game), zeroes the IP / port / lobby members.
    //   FriendGameInfo_t layout (24B, pack=8):
    //     CGameID  m_gameID         @ 0   (8B; low 32 bits = appId)
    //     uint32   m_unGameIP       @ 8
    //     uint16   m_usGamePort     @ 12
    //     uint16   m_usQueryPort    @ 14
    //     CSteamID m_steamIDLobby   @ 16  (8B)
    //   Returns true iff the friend is in a game (non-zero app).
    virtual bool      GetFriendGamePlayed(uint64_t sid, void* pFriendGameInfo) {
        if (!pFriendGameInfo) return false;
        std::memset(pFriendGameInfo, 0, 24);
        uint32_t app;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().friend_game_played_app.find(sid);
            if (it == pushed().friend_game_played_app.end()) return false;
            app = it->second;
        }
        if (app == 0) return false;
        // CGameID's low 32 bits encode the appId; high 32 bits encode
        // the type (mod vs base). Plain games have type=0.
        uint64_t gameID = static_cast<uint64_t>(app);
        std::memcpy(pFriendGameInfo, &gameID, sizeof(gameID));
        return true;
    }
    virtual const char* GetFriendPersonaNameHistory(uint64_t, int)   { return ""; }      // 9
    // 10 — GetFriendSteamLevel(sid). EXPpfL value, 0=unknown. Backed by
    //   pushed.friend_steam_levels; CPlayer_GetSteamLevel responses
    //   populate the map. Default 0 (never observed).
    virtual int       GetFriendSteamLevel(uint64_t sid) {
        if (sid == 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().friend_steam_levels.find(sid);
        return it == pushed().friend_steam_levels.end() ? 0 : it->second;
    }
    // 11 — GetPlayerNickname(sid). Returns the LOCAL nickname the
    //   user assigned to that account via Steam Community → Edit
    //   Nickname. Backed by pushed.player_nicknames; null when the
    //   user hasn't set one (matches SDK contract — Goldberg-emu does
    //   the same). Thread-local stable buffer so the const char* stays
    //   valid until the next call on the same thread.
    virtual const char* GetPlayerNickname(uint64_t sid) {
        if (sid == 0) return nullptr;
        static thread_local std::string tls;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().player_nicknames.find(sid);
            if (it == pushed().player_nicknames.end()) return nullptr;
            tls = it->second;
        }
        return tls.empty() ? nullptr : tls.c_str();
    }
    virtual int       GetFriendsGroupCount()                         { return 0; }       // 12
    virtual int16_t   GetFriendsGroupIDByIndex(int)                  { return 0; }       // 13
    virtual const char* GetFriendsGroupName(int16_t)                 { return ""; }      // 14
    virtual int       GetFriendsGroupMembersCount(int16_t)           { return 0; }       // 15
    virtual void      GetFriendsGroupMembersList(int16_t, uint64_t*, int) {}              // 16
    // 17 — HasFriend(sid, iFriendFlags). EFriendFlags bitmask: 0x1=
    //   None, 0x2=Blocked, 0x4=FriendshipRequested, 0x10=Immediate
    //   (regular friend), 0x20=ClanMember, 0x40=OnGameServer, 0x80=
    //   RequestingFriendship, 0x100=RequestingInfo, 0x200=Ignored,
    //   0x400=IgnoredFriend, 0x800=ChatMember, 0xFFFF=All. We only
    //   track regular friends (0x10/Immediate), so accept any mask
    //   that includes Immediate or All. Same membership check as
    //   GetFriendRelationship.
    virtual bool      HasFriend(uint64_t sid, int iFriendFlags) {
        if (sid == 0) return false;
        // We only track regular friends (k_EFriendFlagImmediate). The
        // caller's mask is an OR of relationship kinds — we report
        // true if the mask requests Immediate (either explicitly via
        // 0x10 or implicitly via 0xFFFF/All which is 0x10 | …). A mask
        // requesting *only* unrelated kinds (Blocked, ClanMember,
        // OnGameServer, …) reports false even when the SID is a
        // tracked friend, because we have no signal for those kinds.
        constexpr int kImmediate = 0x10;
        if ((iFriendFlags & kImmediate) == 0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (uint64_t f : pushed().friends) {
            if (f == sid) return true;
        }
        return false;
    }
    virtual int       GetClanCount()                                 { return 0; }       // 18
    virtual uint64_t  GetClanByIndex(int)                            { return 0; }       // 19
    virtual const char* GetClanName(uint64_t)                        { return ""; }      // 20
    virtual const char* GetClanTag(uint64_t)                         { return ""; }      // 21
    virtual bool      GetClanActivityCounts(uint64_t, int*, int*, int*) { return false; }// 22
    // 23 — DownloadClanActivityCounts(clanSids[], n) → SteamAPICall_t
    //   (DownloadClanActivityCountsResult_t). Empty success — games
    //   call this for friend-list clan-activity badges; no callback
    //   would hang the Steam Overlay's friends panel.
    virtual uint64_t DownloadClanActivityCounts(uint64_t* /*clans*/, int /*n*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::DownloadClanActivityCountsResult cb{};
        cb.m_bSuccess = 0; // no real clan data
        push_call_result(h, lsc_cb::kDownloadClanActivityCountsResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual int       GetFriendCountFromSource(uint64_t)             { return 0; }       // 24
    virtual uint64_t  GetFriendFromSourceByIndex(uint64_t, int)      { return 0; }       // 25
    virtual bool      IsUserInSource(uint64_t, uint64_t)             { return false; }   // 26
    virtual void      SetInGameVoiceSpeaking(uint64_t, bool)         {}                  // 27
    // 28..33 — Overlay-activation slots. The game runs inside Wine and
    //   has no path to fire an Android Intent itself. Each Activate*
    //   call enqueues a PushedState::OverlayRequest; SteamService's
    //   Kotlin overlay-poll coroutine reads via nativePollOverlayRequest
    //   and dispatches Intent.ACTION_VIEW to the system browser.
    //
    //   Drop oldest at 32 entries — defensive against games that call
    //   Activate* in a tight loop. Most games fire one Activate* per
    //   user-driven UI event.
    static void enqueue_overlay(PushedState::OverlayRequest req) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& q = pushed().overlay_request_queue;
        if (q.size() >= 32) q.pop_front();
        q.push_back(std::move(req));
    }
    virtual void      ActivateGameOverlay(const char* dialog) {                          // 28
        PushedState::OverlayRequest r;
        r.kind = "dialog";
        r.arg1 = dialog ? dialog : "";
        enqueue_overlay(std::move(r));
    }
    virtual void      ActivateGameOverlayToUser(const char* dialog, uint64_t sid) {       // 29
        PushedState::OverlayRequest r;
        r.kind = "user";
        r.arg1 = dialog ? dialog : "";
        r.sid  = sid;
        enqueue_overlay(std::move(r));
    }
    virtual void      ActivateGameOverlayToWebPage(const char* url, int /*mode*/) {       // 30
        if (!url || !*url) return;
        PushedState::OverlayRequest r;
        r.kind = "webpage";
        r.arg1 = url;
        enqueue_overlay(std::move(r));
    }
    virtual void      ActivateGameOverlayToStore(uint32_t appid, int /*flag*/) {          // 31
        PushedState::OverlayRequest r;
        r.kind   = "store";
        r.app_id = appid;
        enqueue_overlay(std::move(r));
    }
    virtual void      SetPlayedWith(uint64_t)                        {}                  // 32
    virtual void      ActivateGameOverlayInviteDialog(uint64_t lobby_sid) {                // 33
        PushedState::OverlayRequest r;
        r.kind = "invite";
        r.sid  = lobby_sid;
        enqueue_overlay(std::move(r));
    }
    // 34/35/36 — Get{Small,Medium,Large}FriendAvatar(steamID) → int handle.
    //   Returns the pushed image handle for the given size tier, or 0
    //   if not loaded. SDK contract: 0 = "request fired, listen for
    //   AvatarImageLoaded_t"; we don't auto-fetch, so 0 means
    //   "wn-session hasn't pushed an avatar for this friend yet".
    virtual int GetSmallFriendAvatar(uint64_t steamID) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().friend_avatars.find(steamID);
        return (it == pushed().friend_avatars.end()) ? 0 : it->second.small;
    }
    virtual int GetMediumFriendAvatar(uint64_t steamID) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().friend_avatars.find(steamID);
        return (it == pushed().friend_avatars.end()) ? 0 : it->second.medium;
    }
    virtual int GetLargeFriendAvatar(uint64_t steamID) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().friend_avatars.find(steamID);
        return (it == pushed().friend_avatars.end()) ? 0 : it->second.large;
    }
    // 37 — RequestUserInformation(steamID, bRequireNameOnly).
    //   SDK contract: returns true if the data is being requested
    //   (caller should wait for a PersonaStateChange_t callback), false
    //   if the data is already available locally — in which case the
    //   caller can immediately query GetFriendPersonaName / *Avatar.
    //
    //   We check pushed.friend_persona_names for a non-empty entry as
    //   the "already available" indicator. When data is missing, we
    //   drive CMsgClientRequestFriendData through the cm_bridge —
    //   reply arrives as a server-pushed CMsgClientPersonaState that
    //   SteamService's persona observer re-pushes through
    //   nativeSetFriendPersonaName + friends. bRequireNameOnly toggles
    //   the flag set (1 = name-only; full set otherwise).
    virtual bool RequestUserInformation(uint64_t steamID, bool bRequireNameOnly) {
        if (steamID == 0) return false;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().friend_persona_names.find(steamID);
            if (it != pushed().friend_persona_names.end() && !it->second.empty()) {
                // Data already cached; SDK contract says return false.
                return false;
            }
        }
        int32_t flags = bRequireNameOnly ? 0x01 : 0x47;  // PlayerName / std set
        wn_cm_request_user_info(steamID, flags);
        return true;
    }
    // 38 — RequestClanOfficerList(clanSid) → SteamAPICall_t. Posts a
    //   ClanOfficerListResponse_t with 0 officers + success=0 so the
    //   Steam Overlay's clan-officer panel doesn't hang.
    virtual uint64_t RequestClanOfficerList(uint64_t clanSid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::ClanOfficerListResponse cb{};
        cb.m_steamIDClan = clanSid;
        cb.m_cOfficers   = 0;
        cb.m_bSuccess    = 0;
        push_call_result(h, lsc_cb::kClanOfficerListResponse,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t  GetClanOwner(uint64_t)                         { return 0; }       // 39
    // 40-41 — Clan officer iteration. 0 = no officers exposed.
    virtual int       GetClanOfficerCount(uint64_t)                  { return 0; }
    virtual uint64_t  GetClanOfficerByIndex(uint64_t, int)           { return 0; }
    // 42 — GetUserRestrictions → uint32 bitmask (k_nUserRestriction*).
    //   0 = no restrictions (matches a fully-logged-on adult account).
    virtual uint32_t  GetUserRestrictions()                          { return 0; }
    // 43 — SetRichPresence(key, value). Writes to the self-RP entry
    //   keyed by pushed().steam_id. Empty/null value REMOVES the key
    //   (SDK semantics). Emits FriendRichPresenceUpdate_t for self so
    //   the game can echo its own RP changes (most games rely on this
    //   for HUD sync). Returns false on invalid args or when self
    //   steam_id is unset (game not logged in yet).
    //
    //   ALSO drives Player.SetRichPresence#1 through cm_bridge —
    //   broadcasts the full updated map to Steam so friends see the
    //   real RP strings in their overlay ("in lobby X", "+connect
    //   ...", etc.). Sends the WHOLE current map per call because
    //   that's the proto semantics (one Player.SetRichPresence#1 call
    //   = full replace of the user's RP set).
    virtual bool SetRichPresence(const char* pchKey, const char* pchValue) {
        if (!pchKey || !*pchKey) return false;
        uint64_t self = pushed().steam_id.load();
        if (self == 0) return false;
        // Snapshot the post-update map so the bridge call uses the
        // committed local state (avoids racing a subsequent local-only
        // read against the live-CM upload).
        std::vector<std::string> keys, values;
        uint32_t app_id;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto& rp = pushed().rich_presence[self];
            auto it = std::find_if(rp.begin(), rp.end(),
                [&](const auto& kv) { return kv.first == pchKey; });
            if (!pchValue || !*pchValue) {
                if (it != rp.end()) rp.erase(it);
            } else if (it == rp.end()) {
                rp.emplace_back(pchKey, pchValue);
            } else {
                it->second = pchValue;
            }
            keys.reserve(rp.size());
            values.reserve(rp.size());
            for (const auto& kv : rp) {
                keys.push_back(kv.first);
                values.push_back(kv.second);
            }
            app_id = pushed().app_id.load();
        }
        // Build the const char* arrays the bridge wants (parallel pointers
        // into our snapshot std::string vectors — valid for the call only).
        std::vector<const char*> ck(keys.size()), cv(values.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            ck[i] = keys[i].c_str();
            cv[i] = values[i].c_str();
        }
        wn_cm_set_rich_presence(app_id,
                                 ck.empty() ? nullptr : ck.data(),
                                 cv.empty() ? nullptr : cv.data(),
                                 ck.size());
        lsc_cb::FriendRichPresenceUpdate ev{};
        ev.m_steamIDFriend = self;
        ev.m_nAppID        = app_id;
        push_callback(state().user.load(),
                      lsc_cb::kFriendRichPresenceUpdate,
                      &ev, sizeof(ev));
        return true;
    }
    // 44 — ClearRichPresence. Removes the local-user's RP entry
    //   wholesale + emits FriendRichPresenceUpdate_t + drives
    //   Player.SetRichPresence#1 with an empty map so Steam clears
    //   the server-side RP set. Friends' overlay "Join Game" buttons
    //   collapse + connect-string flow drops.
    virtual void ClearRichPresence() {
        uint64_t self = pushed().steam_id.load();
        if (self == 0) return;
        uint32_t app_id;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            pushed().rich_presence.erase(self);
            app_id = pushed().app_id.load();
        }
        // Broadcast empty map = clear all RP for this user@app.
        wn_cm_set_rich_presence(app_id, nullptr, nullptr, 0);
        lsc_cb::FriendRichPresenceUpdate ev{};
        ev.m_steamIDFriend = self;
        ev.m_nAppID        = app_id;
        push_callback(state().user.load(),
                      lsc_cb::kFriendRichPresenceUpdate,
                      &ev, sizeof(ev));
    }
    // 45 — GetFriendRichPresence(steamID, key) → const char* (TLS).
    //   Steam's contract: pointer valid until the next call on the
    //   same thread on this interface.
    virtual const char* GetFriendRichPresence(uint64_t steamID, const char* pchKey) {
        static thread_local std::string tls_rp;
        tls_rp.clear();
        if (!pchKey) return "";
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().rich_presence.find(steamID);
        if (it == pushed().rich_presence.end()) return "";
        auto kv = std::find_if(it->second.begin(), it->second.end(),
            [&](const auto& p) { return p.first == pchKey; });
        if (kv == it->second.end()) return "";
        tls_rp = kv->second;
        return tls_rp.c_str();
    }
    // 46 — GetFriendRichPresenceKeyCount(steamID).
    virtual int GetFriendRichPresenceKeyCount(uint64_t steamID) {
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().rich_presence.find(steamID);
        if (it == pushed().rich_presence.end()) return 0;
        return static_cast<int>(it->second.size());
    }
    // 47 — GetFriendRichPresenceKeyByIndex(steamID, idx) → const char*.
    virtual const char* GetFriendRichPresenceKeyByIndex(uint64_t steamID, int idx) {
        static thread_local std::string tls_key;
        tls_key.clear();
        if (idx < 0) return "";
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().rich_presence.find(steamID);
        if (it == pushed().rich_presence.end()) return "";
        if (static_cast<size_t>(idx) >= it->second.size()) return "";
        tls_key = it->second[idx].first;
        return tls_key.c_str();
    }
    // 48 — RequestFriendRichPresence(steamID). SDK contract: async
    //   request, eventually fires FriendRichPresenceUpdate_t with the
    //   latest data. Drives CMsgClientRequestFriendData via cm_bridge
    //   with EClientPersonaStateFlag_RichPresence (0x800) set — Steam
    //   replies via CMsgClientPersonaState carrying the friend's
    //   rich_presence map, which the persona observer re-mirrors into
    //   pushed.rich_presence and emits FriendRichPresenceUpdate_t.
    //
    //   We ALSO emit a synthetic FriendRichPresenceUpdate_t with
    //   whatever's currently cached — overlay UIs that gate on the
    //   first callback render immediately; the real one re-renders
    //   on top a few hundred ms later.
    virtual void RequestFriendRichPresence(uint64_t steamID) {
        if (steamID == 0) return;
        // Kick a real CM round-trip for the up-to-date RP. Best-effort —
        // returns false when no active CMClient (cold boot / signed-out).
        wn_cm_request_user_info(steamID, 0x800);  // RichPresence flag bit
        // Emit a synthetic immediate callback with cached data (which
        // may be empty if we've never observed this friend). Overlay
        // gates on the callback firing; cached-empty is fine because
        // the real response re-fires a moment later with real data.
        lsc_cb::FriendRichPresenceUpdate ev{};
        ev.m_steamIDFriend = steamID;
        ev.m_nAppID        = pushed().app_id.load();
        push_callback(state().user.load(),
                      lsc_cb::kFriendRichPresenceUpdate,
                      &ev, sizeof(ev));
    }
    // 49 — InviteUserToGame(friend, connectStr). False = invite not sent.
    virtual bool      InviteUserToGame(uint64_t, const char*)        { return false; }
    // 50-53 — Coplay friends iteration.
    virtual int       GetCoplayFriendCount()                         { return 0; }
    virtual uint64_t  GetCoplayFriend(int)                           { return 0; }
    virtual int       GetFriendCoplayTime(uint64_t)                  { return 0; }
    virtual uint32_t  GetFriendCoplayGame(uint64_t)                  { return 0; }
    // 54-63 — Clan chat (legacy; mostly Source-engine community panel).
    // JoinClanChatRoom(clanSid) → SteamAPICall_t. Posts a
    //   JoinClanChatRoomCompletionResult_t with chatroom-enter-response=
    //   k_EChatRoomEnterResponseError(=2) so games that integrate
    //   with Steam group chat (e.g. Killing Floor 2 clan rooms)
    //   show "couldn't join" rather than hanging.
    virtual uint64_t JoinClanChatRoom(uint64_t clanSid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::JoinClanChatRoomCompletionResult cb{};
        cb.m_steamIDClanChat        = clanSid;
        cb.m_eChatRoomEnterResponse = 2; // k_EChatRoomEnterResponseError
        push_call_result(h, lsc_cb::kJoinClanChatRoomCompletion,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual bool      LeaveClanChatRoom(uint64_t)                    { return false; }
    virtual int       GetClanChatMemberCount(uint64_t)               { return 0; }
    virtual uint64_t  GetChatMemberByIndex(uint64_t, int)            { return 0; }
    virtual bool      SendClanChatMessage(uint64_t, const char*)     { return false; }
    virtual int       GetClanChatMessage(uint64_t, int, void*, int, int*, uint64_t*) { return 0; }
    virtual bool      IsClanChatAdmin(uint64_t, uint64_t)            { return false; }
    virtual bool      IsClanChatWindowOpenInSteam(uint64_t)          { return false; }
    virtual bool      OpenClanChatWindowInSteam(uint64_t)            { return false; }
    virtual bool      CloseClanChatWindowInSteam(uint64_t)           { return false; }
    // 64-66 — Friend messaging.
    virtual bool      SetListenForFriendsMessages(bool)              { return false; }
    virtual bool      ReplyToFriendMessage(uint64_t, const char*)    { return false; }
    virtual int       GetFriendMessage(uint64_t, int, void*, int, int*) { return 0; }
    // 67-69 — Follower API (async).
    // Async followers slots. Real social-graph queries require server-
    // side state we don't host; fire the callback with EResult=Fail so
    // the game's CCallResult unblocks. Games that gate on these (Steam
    // overlay friend-suggestions, social-game friend-recommend lists)
    // proceed past the wait with "0 followers / not following" UI.
    virtual uint64_t GetFollowerCount(uint64_t sid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::FriendsGetFollowerCount cb{};
        cb.m_eResult = 2; // k_EResultFail
        cb.m_steamID = sid;
        cb.m_nCount  = 0;
        push_call_result(h, lsc_cb::kFriendsGetFollowerCount,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t IsFollowing(uint64_t sid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::FriendsIsFollowing cb{};
        cb.m_eResult      = 2;
        cb.m_steamID      = sid;
        cb.m_bIsFollowing = 0;
        push_call_result(h, lsc_cb::kFriendsIsFollowing,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t EnumerateFollowingList(uint32_t /*unStartIndex*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::FriendsEnumerateFollowingList cb{};
        cb.m_eResult           = 2;
        cb.m_nResultsReturned  = 0;
        cb.m_nTotalResultCount = 0;
        push_call_result(h, lsc_cb::kFriendsEnumerateFollowingList,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 70-71 — Clan visibility flags.
    virtual bool      IsClanPublic(uint64_t)                         { return false; }
    virtual bool      IsClanOfficialGameGroup(uint64_t)              { return false; }
    // 72 — GetNumChatsWithUnreadPriorityMessages.
    virtual int       GetNumChatsWithUnreadPriorityMessages()        { return 0; }
    // 73-75 — Modern Remote-Play / overlay dialogs.
    virtual void      ActivateGameOverlayRemotePlayTogetherInviteDialog(uint64_t) {}
    virtual bool      RegisterProtocolInOverlayBrowser(const char*)  { return false; }
    virtual void      ActivateGameOverlayInviteDialogConnectString(const char*) {}
    // 76-79 — Equipped-profile-items (avatar frames, profile backgrounds).
    //   Async kicks off CMsgClient*EquippedProfileItems; we don't fetch.
    // RequestEquippedProfileItems(sid) → SteamAPICall_t. The Steam
    //   Overlay friend-profile popup gates on EquippedProfileItems_t
    //   to decide whether to render animated avatar / avatar-frame /
    //   profile-modifier showcases. We have no Trading Cards backend,
    //   so EResult=Fail + all bools=false. Overlay shows a plain
    //   static avatar instead of the animated showcase.
    virtual uint64_t RequestEquippedProfileItems(uint64_t sid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::EquippedProfileItems cb{};
        cb.m_eResult                    = 2; // k_EResultFail
        cb.m_steamID                    = sid;
        cb.m_bHasAnimatedAvatar         = 0;
        cb.m_bHasAvatarFrame            = 0;
        cb.m_bHasProfileModifier        = 0;
        cb.m_bHasProfileBackground      = 0;
        cb.m_bHasMiniProfileBackground  = 0;
        push_call_result(h, lsc_cb::kEquippedProfileItems,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual bool      BHasEquippedProfileItem(uint64_t, int)         { return false; }
    virtual const char* GetProfileItemPropertyString(uint64_t, int, int) { return ""; }
    virtual uint32_t  GetProfileItemPropertyUint(uint64_t, int, int) { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamRemoteStorage (version 016). Slot 0 FileWrite, 1 FileRead,
// 6 FileDelete, 13 FileExists, 15 GetFileSize, 18 GetFileCount,
// 19 GetFileNameAndSize, 20 GetQuota, 21/22 IsCloudEnabled*,
// 23 SetCloudEnabledForApp.
class ISteamRemoteStorageStub {
public:
    // Resolve "<remote-dir-of-bound-app>/<filename>" under state_mutex.
    // Returns empty string if no bound app, no pushed dir for that app,
    // a null/empty pchFile, or a pchFile containing path-separator
    // traversal attempts ("..", absolute paths) — Cloud filenames are
    // expected to be plain leaf-name relative paths.
    static std::string resolve_cloud_path(const char* pchFile) {
        if (!pchFile || !*pchFile) return {};
        // Reject backslashes too — keep this defensive even on Linux.
        for (const char* p = pchFile; *p; ++p) {
            if (*p == '\\') return {};
        }
        if (pchFile[0] == '/') return {};
        // Forbid ".." segments anywhere in the path.
        std::string fname(pchFile);
        if (fname.find("..") != std::string::npos) return {};
        uint32_t app = pushed().app_id.load();
        if (app == 0) return {};
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_cloud_remote_dirs.find(app);
        if (it == pushed().app_cloud_remote_dirs.end()) return {};
        std::string out = it->second;
        if (!out.empty() && out.back() != '/') out.push_back('/');
        out.append(fname);
        return out;
    }
    // 0 — FileWrite(filename, bytes, cubData). Writes the buffer to
    //   the bound app's remote dir, replacing any existing file. The
    //   wn-session cloud-sync layer picks up the new mtime on its next
    //   batch and uploads via Cloud.{Begin,Commit}FileUpload#1. Returns
    //   false on resolve-failure (no bound app, no dir, malformed
    //   name), open/write errors, or 0-byte writes (matches SDK
    //   contract — empty FileWrite is not allowed).
    virtual bool FileWrite(const char* pchFile, const void* pvData, int cubData) {
        if (!pvData || cubData <= 0) return false;
        std::string path = resolve_cloud_path(pchFile);
        if (path.empty()) return false;
        // Ensure parent dir exists — caller's responsibility per SDK,
        // but real Steam silently mkdirs the remote subdir tree.
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            std::string dir = path.substr(0, slash);
            mkdir(dir.c_str(), 0755);
        }
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;
        ssize_t total = 0;
        const char* p = static_cast<const char*>(pvData);
        while (total < cubData) {
            ssize_t n = ::write(fd, p + total, cubData - total);
            if (n < 0) {
                ::close(fd);
                ::unlink(path.c_str());
                return false;
            }
            total += n;
        }
        ::close(fd);
        // Mirror into cloud_files so the next FileExists / GetFileSize
        // / GetFileTimestamp returns coherent values immediately, even
        // before the next CM cloud batch refreshes the list.
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& files = pushed().cloud_files;
        std::string name(pchFile);
        bool patched = false;
        for (auto& f : files) {
            if (f.name == name) {
                f.size      = static_cast<int32_t>(cubData);
                f.timestamp = static_cast<int64_t>(::time(nullptr));
                patched = true;
                break;
            }
        }
        if (!patched) {
            wn_libsteamclient::PushedState::CloudFileEntry e;
            e.name      = std::move(name);
            e.size      = static_cast<int32_t>(cubData);
            e.timestamp = static_cast<int64_t>(::time(nullptr));
            files.push_back(std::move(e));
        }
        return true;
    }
    // 1 — FileRead(filename, *pvData, cubDataToRead). Reads up to
    //   cubDataToRead bytes from the remote-dir file. Returns the byte
    //   count read (0 on any error / missing file / 0-size buffer).
    virtual int FileRead(const char* pchFile, void* pvData, int cubDataToRead) {
        if (!pvData || cubDataToRead <= 0) return 0;
        std::string path = resolve_cloud_path(pchFile);
        if (path.empty()) return 0;
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return 0;
        ssize_t total = 0;
        char* p = static_cast<char*>(pvData);
        while (total < cubDataToRead) {
            ssize_t n = ::read(fd, p + total, cubDataToRead - total);
            if (n < 0) { ::close(fd); return 0; }
            if (n == 0) break;  // EOF
            total += n;
        }
        ::close(fd);
        return static_cast<int>(total);
    }
    // 2 — FileWriteAsync(filename, pvData, cubData). Allocates a fresh
    //   SteamAPICall_t, performs the disk write synchronously (cheap on
    //   tmpfs / sdcard), and posts the result as a CallResult so the
    //   game's CCallResult dispatch in SteamAPI_RunCallbacks receives a
    //   RemoteStorageFileWriteAsyncComplete_t (k_iCallback=1331) on its
    //   next pump. 0-byte writes and resolve-failures return 0 (no
    //   call posted), matching the SDK contract.
    virtual uint64_t FileWriteAsync(const char* pchFile, const void* pvData, uint32_t cubData) {
        if (!pvData || cubData == 0) return 0;
        std::string path = resolve_cloud_path(pchFile);
        if (path.empty()) return 0;
        // Reuse the sync FileWrite — same disk path, same mirror update.
        bool ok = FileWrite(pchFile, pvData, static_cast<int>(cubData));
        uint64_t hCall = alloc_api_call_handle();
        wn_libsteamclient::callbacks::RemoteStorageFileWriteAsyncComplete cb{};
        cb.m_eResult = ok ? 1 /*k_EResultOK*/ : 2 /*k_EResultFail*/;
        push_call_result(hCall,
                         lsc_cb::kRemoteStorageFileWriteAsyncComplete,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 3 — FileReadAsync(filename, nOffset, cubToRead). Allocates a
    //   SteamAPICall_t, reads from disk into an internal per-handle
    //   buffer, and posts a RemoteStorageFileReadAsyncComplete_t (k_i
    //   Callback=1332) CallResult. The game retrieves the buffered
    //   bytes via FileReadAsyncComplete(hCall, dst, n).
    virtual uint64_t FileReadAsync(const char* pchFile, uint32_t nOffset, uint32_t cubToRead) {
        if (cubToRead == 0) return 0;
        std::string path = resolve_cloud_path(pchFile);
        if (path.empty()) return 0;
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return 0;
        if (nOffset > 0 && ::lseek(fd, nOffset, SEEK_SET) == (off_t)-1) {
            ::close(fd);
            return 0;
        }
        std::vector<uint8_t> buf(cubToRead);
        ssize_t total = 0;
        while (total < (ssize_t)cubToRead) {
            ssize_t n = ::read(fd, buf.data() + total, cubToRead - total);
            if (n < 0) { ::close(fd); return 0; }
            if (n == 0) break;
            total += n;
        }
        ::close(fd);
        buf.resize(total);
        uint64_t hCall = alloc_api_call_handle();
        // Stash for FileReadAsyncComplete pickup. Mutex protects the
        // map; the buffer itself is moved into the entry so no copy.
        {
            std::lock_guard<std::mutex> lk(async_read_mu());
            async_read_buffers()[hCall] = std::move(buf);
        }
        wn_libsteamclient::callbacks::RemoteStorageFileReadAsyncComplete cb{};
        cb.m_hFileReadAsync = hCall;
        cb.m_eResult        = 1 /*k_EResultOK*/;
        cb.m_nOffset        = nOffset;
        cb.m_cubRead        = static_cast<uint32_t>(total);
        push_call_result(hCall,
                         lsc_cb::kRemoteStorageFileReadAsyncComplete,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 4 — FileReadAsyncComplete(hCall, pvBuffer, cubToRead). Game-side
    //   pickup of the bytes stashed during FileReadAsync. Removes the
    //   buffer from the map (each handle is single-shot). Returns false
    //   if the handle is unknown / already consumed / buffer too small.
    virtual bool FileReadAsyncComplete(uint64_t hCall, void* pvBuffer, uint32_t cubToRead) {
        if (!pvBuffer || cubToRead == 0 || hCall == 0) return false;
        std::lock_guard<std::mutex> lk(async_read_mu());
        auto& m = async_read_buffers();
        auto it = m.find(hCall);
        if (it == m.end()) return false;
        const auto& buf = it->second;
        if (cubToRead < buf.size()) {
            // Caller's buffer too small — SDK contract says return
            // false without consuming the buffer (so they can re-try
            // with a bigger one).
            return false;
        }
        std::memcpy(pvBuffer, buf.data(), buf.size());
        m.erase(it);
        return true;
    }
    // 5 — FileForget. "Untrack this file from cloud — leave it on
    //   disk." Effectively the same disk-level outcome as FileDelete
    //   except the local bytes are kept. We pull the file from the
    //   cloud_files mirror so the wn-session sync layer's next batch
    //   dispatches a Cloud.Delete; the on-disk copy is untouched.
    virtual bool FileForget(const char* pchFile) {
        if (!pchFile || !*pchFile) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& files = pushed().cloud_files;
        std::string name(pchFile);
        bool found = false;
        for (auto it = files.begin(); it != files.end(); ) {
            if (it->name == name) { it = files.erase(it); found = true; }
            else ++it;
        }
        return found;
    }
    // 6 — FileDelete(filename). Unlinks the file from the remote dir
    //   and removes it from the cloud_files mirror. The wn-session
    //   sync layer's next batch picks up the absence and dispatches a
    //   Cloud.Delete request. Returns false on resolve-failure or if
    //   the file didn't exist locally (unlink errno=ENOENT).
    virtual bool FileDelete(const char* pchFile) {
        std::string path = resolve_cloud_path(pchFile);
        if (path.empty()) return false;
        int rc = ::unlink(path.c_str());
        // Always purge the mirror entry even if disk unlink failed —
        // a missing file in the mirror but present on disk is fine
        // (next CM batch refreshes the mirror), but the opposite leaks
        // ghost entries into FileExists.
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& files = pushed().cloud_files;
        std::string name(pchFile);
        for (auto it = files.begin(); it != files.end(); ) {
            if (it->name == name) it = files.erase(it);
            else ++it;
        }
        return rc == 0;
    }
    // 7 — FileShare(filename). Async — marks the file as shareable via
    //   UGC.Download by other accounts and returns a SteamAPICall_t
    //   that resolves to a RemoteStorageFileShareResult_t with a
    //   UGCHandle_t handle. We don't have CDN-backed UGC publishing,
    //   but we allocate a stable per-(app, filename) handle from the
    //   file's bytes + mtime so games that cache the handle across
    //   sessions get a consistent one. Filename must exist in the
    //   cloud_files mirror; missing = k_EResultFileNotFound.
    virtual uint64_t FileShare(const char* pchFile) {
        if (!pchFile || !*pchFile) return 0;
        uint64_t hCall = alloc_api_call_handle();
        lsc_cb::RemoteStorageFileShareResult cb{};
        std::strncpy(cb.m_rgchFilename, pchFile, sizeof(cb.m_rgchFilename) - 1);
        bool found = false;
        int32_t size = 0;
        int64_t ts   = 0;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            for (const auto& f : pushed().cloud_files) {
                if (f.name == pchFile) {
                    found = true;
                    size  = f.size;
                    ts    = f.timestamp;
                    break;
                }
            }
        }
        if (!found) {
            cb.m_eResult = 9; // k_EResultFileNotFound
            cb.m_hFile   = 0;
        } else {
            cb.m_eResult = 1; // k_EResultOK
            // Stable handle: hash(appId, filename, size, timestamp).
            // Reused across sessions because all inputs persist.
            uint64_t h = 0xcbf29ce484222325ULL; // FNV-1a 64 seed
            auto mix = [&](const void* d, size_t n) {
                const uint8_t* p = static_cast<const uint8_t*>(d);
                for (size_t i = 0; i < n; ++i) {
                    h ^= p[i];
                    h *= 0x100000001b3ULL;
                }
            };
            uint32_t app = pushed().app_id.load();
            mix(&app,  sizeof(app));
            mix(pchFile, std::strlen(pchFile));
            mix(&size, sizeof(size));
            mix(&ts,   sizeof(ts));
            cb.m_hFile = h | (1ULL << 63);  // set hi bit so it never collides with hCall
        }
        push_call_result(hCall, lsc_cb::kRemoteStorageFileShareResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return hCall;
    }
    // 8 — SetSyncPlatforms(filename, eRemoteStoragePlatform). Per-file
    //   cloud-sync platform restriction (e.g. mark a Windows-only save
    //   so the Steam Cloud doesn't sync it down to a Mac install).
    //   We don't enforce per-platform restrictions — every cloud file
    //   syncs to every device that requests it — so this is a no-op
    //   that REPORTS SUCCESS instead of false. The SDK contract is
    //   \"true on success, false if file doesn't exist or platform
    //   value is invalid\"; games that gate further setup on the bool
    //   return previously dead-ended here.
    virtual bool      SetSyncPlatforms(const char* pchFile, int /*ePlatform*/) {
        if (!pchFile || !*pchFile) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return true;
        }
        return false;
    }
    // 9 — FileWriteStreamOpen(filename). Allocates a stream handle
    //   (UGCFileWriteStreamHandle_t = uint64) and stages the future
    //   commit target. We open a tempfile under the remote dir
    //   (<filename>.wnstream-<handle>) and write chunks to it; Close
    //   atomically renames it to the final name. Resolve-failure
    //   returns 0.
    virtual uint64_t FileWriteStreamOpen(const char* pchFile) {
        std::string finalPath = resolve_cloud_path(pchFile);
        if (finalPath.empty()) return 0;
        size_t slash = finalPath.find_last_of('/');
        if (slash != std::string::npos) {
            mkdir(finalPath.substr(0, slash).c_str(), 0755);
        }
        uint64_t h = alloc_api_call_handle();
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), ".wnstream-%llu",
                      static_cast<unsigned long long>(h));
        std::string tempPath = finalPath + suffix;
        int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 0;
        std::lock_guard<std::mutex> lk(stream_mu());
        StreamSlot s;
        s.fd        = fd;
        s.tempPath  = std::move(tempPath);
        s.finalPath = std::move(finalPath);
        s.name      = std::string(pchFile);
        s.bytes     = 0;
        streams()[h] = std::move(s);
        return h;
    }
    // 10 — FileWriteStreamWriteChunk(hStream, pvData, cubData). Appends
    //   bytes to the open stream's tempfile. Returns false on unknown
    //   handle / write error / 0-byte append.
    virtual bool FileWriteStreamWriteChunk(uint64_t h, const void* pvData, int cubData) {
        if (!pvData || cubData <= 0) return false;
        std::lock_guard<std::mutex> lk(stream_mu());
        auto it = streams().find(h);
        if (it == streams().end()) return false;
        const char* p = static_cast<const char*>(pvData);
        int total = 0;
        while (total < cubData) {
            ssize_t n = ::write(it->second.fd, p + total, cubData - total);
            if (n < 0) {
                // Don't auto-rollback — caller may try to recover or
                // explicitly Cancel; just report failure.
                return false;
            }
            total += n;
        }
        it->second.bytes += total;
        return true;
    }
    // 11 — FileWriteStreamClose(hStream). Commits the stream: fsync +
    //   rename tempfile → final, update cloud_files mirror. Returns
    //   false on unknown handle or rename failure.
    virtual bool FileWriteStreamClose(uint64_t h) {
        StreamSlot slot;
        {
            std::lock_guard<std::mutex> lk(stream_mu());
            auto it = streams().find(h);
            if (it == streams().end()) return false;
            slot = std::move(it->second);
            streams().erase(it);
        }
        ::fsync(slot.fd);
        ::close(slot.fd);
        if (::rename(slot.tempPath.c_str(), slot.finalPath.c_str()) != 0) {
            ::unlink(slot.tempPath.c_str());
            return false;
        }
        // Mirror into cloud_files.
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& files = pushed().cloud_files;
        bool patched = false;
        for (auto& f : files) {
            if (f.name == slot.name) {
                f.size      = static_cast<int32_t>(slot.bytes);
                f.timestamp = static_cast<int64_t>(::time(nullptr));
                patched = true;
                break;
            }
        }
        if (!patched) {
            wn_libsteamclient::PushedState::CloudFileEntry e;
            e.name      = slot.name;
            e.size      = static_cast<int32_t>(slot.bytes);
            e.timestamp = static_cast<int64_t>(::time(nullptr));
            files.push_back(std::move(e));
        }
        return true;
    }
    // 12 — FileWriteStreamCancel(hStream). Drops the stream — close fd,
    //   unlink tempfile, no commit, no mirror update.
    virtual bool FileWriteStreamCancel(uint64_t h) {
        std::lock_guard<std::mutex> lk(stream_mu());
        auto it = streams().find(h);
        if (it == streams().end()) return false;
        ::close(it->second.fd);
        ::unlink(it->second.tempPath.c_str());
        streams().erase(it);
        return true;
    }

    // 13 — FileExists. Match against the pushed cloud file list.
    virtual bool      FileExists(const char* pchFile) {
        if (!pchFile || !*pchFile) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return true;
        }
        return false;
    }
    // 14 — FilePersisted. "Has the most-recent FileWrite been committed
    //   to Steam Cloud?" The cloud_files mirror only contains entries
    //   that the wn-session sync layer has either uploaded or learned
    //   about from a Cloud.EnumerateUserFiles response — so presence
    //   there is a faithful "persisted" signal. Same lookup shape as
    //   FileExists; semantically distinguished by the SDK contract,
    //   not by our internal data.
    virtual bool FilePersisted(const char* pchFile) {
        if (!pchFile || !*pchFile) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return true;
        }
        return false;
    }

    // 15 — GetFileSize(name). 0 when unknown.
    virtual int       GetFileSize(const char* pchFile) {
        if (!pchFile || !*pchFile) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return static_cast<int>(f.size);
        }
        return 0;
    }
    // 16 — GetFileTimestamp(name).
    virtual int64_t   GetFileTimestamp(const char* pchFile) {
        if (!pchFile || !*pchFile) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return f.timestamp;
        }
        return 0;
    }
    // 17 — GetSyncPlatforms(filename) → ERemoteStoragePlatform
    //   bitmask. Mirror of slot 8. We don't track per-file platform
    //   restrictions; return ERemoteStoragePlatformAll (0xFFFFFFFF
    //   on 32-bit, or signed 0xFF) for known files — \"this file
    //   syncs to every platform\". 0 (k_ERemoteStoragePlatformNone)
    //   for unknown files. Games that check this before pushing
    //   platform-specific saves see the universal-sync answer.
    virtual int       GetSyncPlatforms(const char* pchFile) {
        if (!pchFile || !*pchFile) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        for (const auto& f : pushed().cloud_files) {
            if (f.name == pchFile) return -1; // k_ERemoteStoragePlatformAll
        }
        return 0;
    }

    // 18 — GetFileCount. Count of pushed file entries.
    virtual int       GetFileCount() {
        std::lock_guard<std::mutex> lk(state_mutex());
        return static_cast<int>(pushed().cloud_files.size());
    }
    // 19 — GetFileNameAndSize(iFile, pcb). Returns a pointer into a
    // process-local thread-local buffer that survives until the next
    // call on the same thread (matches Steam's contract — the engine
    // copies before the next call).
    virtual const char* GetFileNameAndSize(int iFile, int32_t* pnFileSizeInBytes) {
        static thread_local std::string tls_name;
        tls_name.clear();
        int32_t size = 0;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            const auto& files = pushed().cloud_files;
            if (iFile >= 0 && static_cast<size_t>(iFile) < files.size()) {
                tls_name = files[iFile].name;
                size = files[iFile].size;
            }
        }
        if (pnFileSizeInBytes) *pnFileSizeInBytes = size;
        return tls_name.c_str();
    }
    // 20 — GetQuota(total, avail). Pushed values; 0 when unknown.
    virtual void      GetQuota(uint64_t* total, uint64_t* avail) {
        if (total) *total = pushed().cloud_quota_total.load();
        if (avail) *avail = pushed().cloud_quota_available.load();
    }
    // 21 — IsCloudEnabledForAccount.
    virtual bool      IsCloudEnabledForAccount() {
        return pushed().cloud_enabled_account.load();
    }
    // 22 — IsCloudEnabledForApp.
    virtual bool      IsCloudEnabledForApp() {
        return pushed().cloud_enabled_app.load();
    }
    // 23 — SetCloudEnabledForApp(bool). Writes to pushed state; the
    // Kotlin side observes this via WnSteamBootstrap's setter and
    // mirrors it in PrefManager if needed.
    virtual void      SetCloudEnabledForApp(bool enabled) {
        pushed().cloud_enabled_app.store(enabled);
    }
    // 24-29 — UGCDownload + cache surface. Workshop file download
    //   API. We don't have a UGC backend pipeline of our own, so the
    //   download FAILS — but the SDK callback still needs to fire so
    //   the game's CCallResult.IsActive polling unblocks.
    //   Workshop-using games (Cities Skylines, RimWorld, Skyrim, etc.)
    //   call this on every mod fetch; the prior hCall=0 had them stuck
    //   at "Downloading mods..." indefinitely.
    virtual uint64_t UGCDownload(uint64_t hContent, uint32_t /*priority*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::RemoteStorageDownloadUGCResult cb{};
        cb.m_eResult        = 2; // k_EResultFail — no UGC backend
        cb.m_hFile          = hContent;
        cb.m_nAppID         = pushed().app_id.load();
        cb.m_nSizeInBytes   = 0;
        cb.m_pchFileName[0] = '\0';
        cb.m_ulSteamIDOwner = 0;
        push_call_result(h, lsc_cb::kRemoteStorageDownloadUGC,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual bool      GetUGCDownloadProgress(uint64_t, int32_t* d, int32_t* e) {
        if (d) *d = 0; if (e) *e = 0; return false;
    }
    virtual bool      GetUGCDetails(uint64_t /*content*/, uint32_t* appID,
                                     char** ppchName, int32_t* pcbFile, uint64_t* steamIDOwner) {
        if (appID) *appID = 0;
        if (ppchName) *ppchName = nullptr;
        if (pcbFile) *pcbFile = 0;
        if (steamIDOwner) *steamIDOwner = 0;
        return false;
    }
    virtual int32_t   UGCRead(uint64_t /*content*/, void* /*buf*/, int32_t /*cubData*/,
                               uint32_t /*offset*/, int /*action*/) { return 0; }
    virtual int32_t   GetCachedUGCCount()                            { return 0; }
    virtual uint64_t  GetCachedUGCHandle(int32_t /*idx*/)            { return 0; }
    // 30-53 — DEPRECATED published-file workflow. The interface still
    //   has the slots for ABI stability; games written against newer
    //   ISteamUGC should be calling that surface (which we stub too).
    //   All async slots return 0 hCall (no callback lands); sync
    //   slots return false / 0. Removing these breaks games that
    //   dlsym'd "STEAMREMOTESTORAGE_INTERFACE_VERSION016" and probe
    //   slot indices >= 30.
    virtual uint64_t  PublishWorkshopFile_DEPRECATED(const char*, const char*, uint32_t, const char*, const char*, int, void*, void*, int) { return 0; }
    virtual uint64_t  CreatePublishedFileUpdateRequest_DEPRECATED(uint64_t) { return 0; }
    virtual bool      UpdatePublishedFileFile_DEPRECATED(uint64_t, const char*)            { return false; }
    virtual bool      UpdatePublishedFilePreviewFile_DEPRECATED(uint64_t, const char*)     { return false; }
    virtual bool      UpdatePublishedFileTitle_DEPRECATED(uint64_t, const char*)           { return false; }
    virtual bool      UpdatePublishedFileDescription_DEPRECATED(uint64_t, const char*)     { return false; }
    virtual bool      UpdatePublishedFileVisibility_DEPRECATED(uint64_t, int)              { return false; }
    virtual bool      UpdatePublishedFileTags_DEPRECATED(uint64_t, void*)                  { return false; }
    virtual uint64_t  CommitPublishedFileUpdate_DEPRECATED(uint64_t)                       { return 0; }
    virtual uint64_t  GetPublishedFileDetails_DEPRECATED(uint64_t, uint32_t)               { return 0; }
    virtual uint64_t  DeletePublishedFile_DEPRECATED(uint64_t)                             { return 0; }
    virtual uint64_t  EnumerateUserPublishedFiles_DEPRECATED(uint32_t)                     { return 0; }
    virtual uint64_t  SubscribePublishedFile_DEPRECATED(uint64_t)                          { return 0; }
    virtual uint64_t  EnumerateUserSubscribedFiles_DEPRECATED(uint32_t)                    { return 0; }
    virtual uint64_t  UnsubscribePublishedFile_DEPRECATED(uint64_t)                        { return 0; }
    virtual bool      UpdatePublishedFileSetChangeDescription_DEPRECATED(uint64_t, const char*) { return false; }
    virtual uint64_t  GetPublishedItemVoteDetails_DEPRECATED(uint64_t)                     { return 0; }
    virtual uint64_t  UpdateUserPublishedItemVote_DEPRECATED(uint64_t, bool)               { return 0; }
    virtual uint64_t  GetUserPublishedItemVoteDetails_DEPRECATED(uint64_t)                 { return 0; }
    virtual uint64_t  EnumerateUserSharedWorkshopFiles_DEPRECATED(uint64_t, uint32_t, void*, void*) { return 0; }
    virtual uint64_t  PublishVideo_DEPRECATED(int, const char*, uint32_t, const char*, const char*, uint32_t, void*) { return 0; }
    virtual uint64_t  SetUserPublishedFileAction_DEPRECATED(uint64_t, int)                 { return 0; }
    virtual uint64_t  EnumeratePublishedFilesByUserAction_DEPRECATED(int, uint32_t)        { return 0; }
    virtual uint64_t  EnumeratePublishedWorkshopFiles_DEPRECATED(int, uint32_t, uint32_t, uint32_t, void*, void*) { return 0; }
    // 54 — UGCDownloadToLocation. Modern UGC download with on-disk
    //   destination. Returns 0 hCall (no download attempted).
    // UGCDownloadToLocation(hContent, location, priority) → SteamAPICall
    //   _t. Variant of UGCDownload that downloads to a specific path.
    //   Same callback shape; reuse the RemoteStorageDownloadUGCResult_t
    //   we built for slot 24, with the same Fail semantics — no UGC
    //   backend, callback delivered.
    virtual uint64_t UGCDownloadToLocation(uint64_t hContent, const char* /*location*/, uint32_t /*priority*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::RemoteStorageDownloadUGCResult cb{};
        cb.m_eResult        = 2;
        cb.m_hFile          = hContent;
        cb.m_nAppID         = pushed().app_id.load();
        cb.m_nSizeInBytes   = 0;
        cb.m_pchFileName[0] = '\0';
        cb.m_ulSteamIDOwner = 0;
        push_call_result(h, lsc_cb::kRemoteStorageDownloadUGC,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 55-56 — Local file change tracking (v016 addition). Returns 0 /
    //   nullptr (no local changes pending).
    virtual int32_t   GetLocalFileChangeCount()                                            { return 0; }
    virtual const char* GetLocalFileChange(int32_t /*idx*/, int* peChangeType, int* pePathType) {
        if (peChangeType) *peChangeType = 0;
        if (pePathType)   *pePathType   = 0;
        return "";
    }
    // 57-58 — File write batching (modern v016). The SDK contract is
    //   "wrap N FileWrite calls in Begin/End to atomically commit",
    //   but our backing pushed-state cache is already atomic, so the
    //   no-op return-true is correct semantically.
    virtual bool      BeginFileWriteBatch()                                                { return true; }
    virtual bool      EndFileWriteBatch()                                                  { return true; }
};

// ---------------------------------------------------------------------------
// ISteamUserStats (version 013).
class ISteamUserStatsStub {
public:
    // 0 — RequestCurrentStats. SDK contract: return value signals "did
    //   the request kick off?" (true on success, false only on
    //   not-initialized / not-logged-on). The actual data arrives via
    //   the async UserStatsReceived_t callback.
    //
    //   If the schema is already pushed (stats_ready=true), fire the
    //   callback synchronously with eResult=OK so re-querying mid-game
    //   sees a fresh trigger. If not, return true ANYWAY (so the game
    //   waits patiently) and let the Kotlin schema-push path's own
    //   UserStatsReceived_t emit deliver the data when it lands.
    //
    //   Prior behavior fired the cb synchronously with eResult=Fail
    //   during the early-boot race window — observed in bootstrap
    //   Stage2 diagnostic. Games gated on "did the cb succeed?"
    //   permanently disabled their stats subsystem on that false read.
    virtual bool RequestCurrentStats() {
        if (!state().logged_on.load()) return false;
        if (pushed().stats_ready.load()) {
            lsc_cb::UserStatsReceived payload{};
            payload.m_nGameID     = static_cast<uint64_t>(pushed().app_id.load());
            payload.m_eResult     = 1;  // k_EResultOK
            payload.m_steamIDUser = pushed().steam_id.load();
            push_callback(state().user.load(),
                          lsc_cb::kUserStatsReceived,
                          &payload, sizeof(payload));
        }
        return true;
    }
    // 1 — GetStatInt(name, *out).
    virtual bool GetStatInt(const char* pchName, int32_t* pData) {
        if (!pchName || !pData) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().stats_int.find(pchName);
        if (it == pushed().stats_int.end()) return false;
        *pData = it->second;
        return true;
    }
    // 2 — GetStatFloat(name, *out).
    virtual bool GetStatFloat(const char* pchName, float* pData) {
        if (!pchName || !pData) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().stats_float.find(pchName);
        if (it == pushed().stats_float.end()) return false;
        *pData = it->second;
        return true;
    }
    // 3 — SetStatInt. Stores locally + marks dirty so StoreStats()
    //   uploads to Steam (CMsgClientStoreUserStats2 with the value
    //   keyed by stat_name_to_id[pchName]).
    virtual bool SetStatInt(const char* pchName, int32_t nData) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        pushed().stats_int[pchName] = nData;
        pushed().dirty_stats_int.insert(pchName);
        return true;
    }
    // 4 — SetStatFloat. Same dirty-tracking; floats are sent as raw
    //   IEEE-754 bits packed into the uint32 stat_value field.
    virtual bool SetStatFloat(const char* pchName, float fData) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        pushed().stats_float[pchName] = fData;
        pushed().dirty_stats_float.insert(pchName);
        return true;
    }
    // 5 — UpdateAvgRateStat(name, countThisSession, sessionLength).
    //   Maintains a running average — visible stats_float[name] is
    //   total_count / total_time across all calls since the schema
    //   was loaded. Per Steam's contract, games call this with a
    //   per-session delta and read the average via GetStat(float).
    //   Names must match the schema's avg-rate stat declaration.
    virtual bool UpdateAvgRateStat(const char* pchName,
                                    float flCountThisSession,
                                    double dSessionLength) {
        if (!pchName || dSessionLength <= 0.0) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto& acc = pushed().stats_avg_rate[pchName];
        acc.total_count += static_cast<double>(flCountThisSession);
        acc.total_time  += dSessionLength;
        if (acc.total_time > 0.0) {
            pushed().stats_float[pchName] =
                static_cast<float>(acc.total_count / acc.total_time);
        }
        // Mark dirty so StoreStats uploads the new running average.
        pushed().dirty_stats_float.insert(pchName);
        return true;
    }
    // 6 — GetAchievement(name, *out).
    virtual bool GetAchievement(const char* pchName, bool* pbAchieved) {
        if (!pchName || !pbAchieved) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return false;
        *pbAchieved = pushed().achievements[it->second].achieved;
        return true;
    }
    // 7 — SetAchievement. Flip the local cache; CMsgClientStoreUserStats
    // upload is wired through the wn-session backend on StoreStats().
    // On a true false→true transition, mark the entry pending_store so
    // the next StoreStats() emits UserAchievementStored_t for it.
    // Setting an already-unlocked achievement is a no-op (matching SDK
    // behavior — Steam does NOT re-emit for redundant sets).
    virtual bool SetAchievement(const char* pchName) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return false;
        auto& a = pushed().achievements[it->second];
        if (!a.achieved) {
            a.achieved      = true;
            a.unlock_time   = static_cast<uint32_t>(::time(nullptr));
            a.pending_store = true;
        }
        return true;
    }
    // 8 — ClearAchievement. Marks the entry pending_store=true on a
    //   true→false transition so the next StoreStats AND-NOTs the bit
    //   in the parent stat — propagating the clear to Steam. If the
    //   achievement was never unlocked (a redundant clear), no
    //   state change → pending_store stays false. Within a single
    //   StoreStats cycle: a SetAchievement→ClearAchievement pair on
    //   the same name reads as cleared (last write wins on
    //   `achieved`), and StoreStats's pass reads `achieved` directly
    //   to decide OR-in vs. AND-NOT.
    virtual bool ClearAchievement(const char* pchName) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return false;
        auto& a = pushed().achievements[it->second];
        bool was = a.achieved;
        a.achieved      = false;
        a.unlock_time   = 0;
        if (was) a.pending_store = true;
        return true;
    }
    // 9 — GetAchievementAndUnlockTime(name, *achieved, *unlockTime).
    virtual bool GetAchievementAndUnlockTime(const char* pchName,
                                             bool* pbAchieved,
                                             uint32_t* punlockTime) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return false;
        const auto& a = pushed().achievements[it->second];
        if (pbAchieved)   *pbAchieved   = a.achieved;
        if (punlockTime)  *punlockTime  = a.unlock_time;
        return true;
    }
    // 10 — StoreStats. From the .so side we report success AND emit:
    //   • One UserAchievementStored_t per achievement whose unlock has
    //     not yet been "stored" (pending_store). Overlay HUD popups +
    //     achievement-celebration UIs gate on this callback. Order is
    //     vector order (which mirrors schema declaration order).
    //   • One aggregate UserStatsStored_t with eresult=1 OK (or 2 Fail
    //     if stats_ready hasn't fired yet).
    //
    // SERVER UPLOAD: cm_bridge.wn_cm_store_user_stats sends
    // CMsgClientStoreUserStats2 to Steam so achievements unlock both
    // locally AND on the user's Steam profile. We accumulate the dirty
    // achievements' bit-pack mapping (block_id, bit_index) — fetched
    // from pushed-state per-achievement — then group by block_id +
    // OR-in the bits to build the (stat_id, stat_value) pairs the
    // proto requires. Achievements without a bit-pack mapping (block
    // _id == -1) are dropped from the upload but still emit local
    // callbacks. crc_stats is 0 (Steam servers tolerate this for
    // most apps — schema-CRC checking is documented but rarely
    // enforced on clients).
    //
    // After emission the pending_store flags are cleared so a
    // subsequent StoreStats with no new unlocks emits ONLY the
    // aggregate callback (matching the SDK's no-redundancy contract).
    virtual bool StoreStats() {
        bool ready = pushed().stats_ready.load();
        uint32_t app_id = pushed().app_id.load();
        uint64_t game_id = static_cast<uint64_t>(app_id);

        // Snapshot dirty achievements under the lock, then release the
        // lock before invoking callbacks — push_callback acquires its
        // own mutex and the registry dispatch path could re-enter the
        // ISteamUserStats methods.
        struct DirtyAch { std::string name; int32_t block_id; int32_t bit_index; bool achieved; };
        std::vector<DirtyAch> dirty;
        // Also snapshot the FULL int-stat values so we can resolve
        // current bit-pack stat values for any block referenced by a
        // dirty achievement (block_id == stat numeric id, stored as
        // stringified key in stats_int).
        std::unordered_map<std::string, int32_t> stats_int_snapshot;
        // Dirty stats — drained on this StoreStats call. Resolved to
        // their numeric stat_id via stat_name_to_id; the int / float
        // values come from stats_int / stats_float (the running avg
        // for avg-rate stats already lives in stats_float).
        std::vector<std::tuple<uint32_t, uint32_t>> dirty_stat_uploads;
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            for (auto& a : pushed().achievements) {
                if (a.pending_store) {
                    dirty.push_back(DirtyAch{a.api_name, a.block_id,
                                              a.bit_index, a.achieved});
                    a.pending_store = false;
                }
            }
            stats_int_snapshot = pushed().stats_int;
            // Drain dirty int stats. Resolve each name to its numeric
            // id via stat_name_to_id; skip entries without a mapping
            // (legacy / not-yet-seeded schemas — same drop policy as
            // achievement bit-pack with no block_id).
            for (const auto& name : pushed().dirty_stats_int) {
                auto idIt = pushed().stat_name_to_id.find(name);
                if (idIt == pushed().stat_name_to_id.end()) continue;
                auto vIt = pushed().stats_int.find(name);
                uint32_t v = (vIt != pushed().stats_int.end())
                             ? static_cast<uint32_t>(vIt->second) : 0u;
                dirty_stat_uploads.emplace_back(idIt->second, v);
            }
            for (const auto& name : pushed().dirty_stats_float) {
                auto idIt = pushed().stat_name_to_id.find(name);
                if (idIt == pushed().stat_name_to_id.end()) continue;
                auto vIt = pushed().stats_float.find(name);
                // Steam packs float stats as their raw IEEE-754 bits
                // into the uint32 stat_value field.
                uint32_t bits = 0;
                if (vIt != pushed().stats_float.end()) {
                    float f = vIt->second;
                    std::memcpy(&bits, &f, sizeof(bits));
                }
                dirty_stat_uploads.emplace_back(idIt->second, bits);
            }
            pushed().dirty_stats_int.clear();
            pushed().dirty_stats_float.clear();
        }

        // Per-achievement callbacks. m_nCurProgress / m_nMaxProgress
        // are zeroed because unlocks (vs. progress-indicate) have no
        // progress dimension — the SDK semantics are "unlocked = full
        // progress"; HUDs treat 0/0 as "unlock event, not progress".
        for (const auto& d : dirty) {
            lsc_cb::UserAchievementStored ach{};
            ach.m_nGameID           = game_id;
            ach.m_bGroupAchievement = false;
            std::strncpy(ach.m_rgchAchievementName,
                         d.name.c_str(),
                         lsc_cb::kAchievementNameMax - 1);
            // strncpy leaves the buffer un-NUL-terminated if name is
            // longer than the buffer — force the trailing NUL.
            ach.m_rgchAchievementName[lsc_cb::kAchievementNameMax - 1] = '\0';
            ach.m_nCurProgress = 0;
            ach.m_nMaxProgress = 0;
            push_callback(state().user.load(),
                          lsc_cb::kUserAchievementStored,
                          &ach, sizeof(ach));
        }

        // Server upload — merge two sources:
        //   (a) Dirty bit-pack achievements: group by block_id, OR-in
        //       each achievement's bit onto the current stat_value.
        //   (b) Dirty direct stats (SetStatInt / SetStatFloat /
        //       UpdateAvgRateStat): take the post-update value
        //       verbatim (floats packed as raw IEEE-754 bits).
        // (b) wins if the same stat_id appears in both — the game
        // explicitly set a value, so the bit-pack OR-in is layered
        // on top of that value (preserving the explicit set + the
        // unlocks).
        if (app_id != 0 && (!dirty.empty() || !dirty_stat_uploads.empty())) {
            std::unordered_map<uint32_t, uint32_t> stat_id_to_value;
            // Seed with direct dirty-stat values FIRST so bit-pack
            // OR-in operates on the freshly-set base.
            for (const auto& [id, v] : dirty_stat_uploads) {
                stat_id_to_value[id] = v;
            }
            // Layer bit-pack achievement unlocks on top.
            for (const auto& d : dirty) {
                if (d.block_id < 0) continue;
                uint32_t stat_id = static_cast<uint32_t>(d.block_id);
                auto it = stat_id_to_value.find(stat_id);
                if (it == stat_id_to_value.end()) {
                    // No direct-stat seed — pull current value from
                    // stats_int_snapshot (keyed by stringified id).
                    char key[16];
                    std::snprintf(key, sizeof(key), "%u", stat_id);
                    auto sit = stats_int_snapshot.find(key);
                    uint32_t cur = (sit != stats_int_snapshot.end())
                                   ? static_cast<uint32_t>(sit->second)
                                   : 0u;
                    it = stat_id_to_value.emplace(stat_id, cur).first;
                }
                if (d.bit_index >= 0 && d.bit_index < 32) {
                    uint32_t mask = 1u << static_cast<uint32_t>(d.bit_index);
                    if (d.achieved) it->second |= mask;
                    else            it->second &= ~mask;
                }
            }
            if (!stat_id_to_value.empty()) {
                std::vector<uint32_t> ids;
                std::vector<uint32_t> vals;
                ids.reserve(stat_id_to_value.size());
                vals.reserve(stat_id_to_value.size());
                for (auto& [k, v] : stat_id_to_value) {
                    ids.push_back(k);
                    vals.push_back(v);
                }
                wn_cm_store_user_stats(app_id, /*crc_stats=*/0,
                                       ids.data(), vals.data(),
                                       ids.size());
                __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
                    "StoreStats: pushed %zu stat(s) to Steam (app=%u, "
                    "ach_dirty=%zu, stat_dirty=%zu)",
                    ids.size(), app_id, dirty.size(), dirty_stat_uploads.size());
            }
        }

        lsc_cb::UserStatsStored payload{};
        payload.m_nGameID = game_id;
        payload.m_eResult = ready ? 1 : 2;
        push_callback(state().user.load(),
                      lsc_cb::kUserStatsStored,
                      &payload, sizeof(payload));
        return ready;
    }
    // 11 — GetAchievementIcon. Returns a synthetic non-zero handle
    // (slot index + 1) so callers that gate on != 0 see a "have icon"
    // signal; binding the handle to actual bytes is a follow-up.
    virtual int GetAchievementIcon(const char* pchName) {
        if (!pchName) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return 0;
        return pushed().achievements[it->second].icon_handle;
    }
    // 12 — GetAchievementDisplayAttribute(name, "name"/"desc"/"hidden").
    // Steam's docs spell those out as the three legal keys. Returns
    // pointer into thread-local storage (Steam's contract is "valid
    // until the next call on the same thread on this interface").
    //
    // For "name" / "desc", picks per-locale from
    // AchievementEntry.{display_names,descriptions} using a fallback
    // chain: runtime ui_language → "english" → any-available locale.
    // Schemas that only shipped one locale still resolve via the
    // english seed key set by nativeSetAchievementSchema.
    virtual const char* GetAchievementDisplayAttribute(const char* pchName,
                                                       const char* pchKey) {
        static thread_local std::string tls_attr;
        tls_attr.clear();
        if (!pchName || !pchKey) return "";
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().achievement_index.find(pchName);
        if (it == pushed().achievement_index.end()) return "";
        const auto& a = pushed().achievements[it->second];

        auto pick_locale = [&](const std::unordered_map<std::string, std::string>& m)
                -> const std::string& {
            static const std::string kEmpty;
            // 1. runtime ui_language (whatever Kotlin pushed).
            const std::string& ui = pushed().ui_language;
            if (!ui.empty()) {
                auto h = m.find(ui);
                if (h != m.end() && !h->second.empty()) return h->second;
            }
            // 2. english.
            auto h = m.find("english");
            if (h != m.end() && !h->second.empty()) return h->second;
            // 3. any non-empty entry.
            for (const auto& kv : m) {
                if (!kv.second.empty()) return kv.second;
            }
            return kEmpty;
        };

        if (std::strcmp(pchKey, "name") == 0)         tls_attr = pick_locale(a.display_names);
        else if (std::strcmp(pchKey, "desc") == 0)    tls_attr = pick_locale(a.descriptions);
        else if (std::strcmp(pchKey, "hidden") == 0)  tls_attr = a.hidden ? "1" : "0";
        return tls_attr.c_str();
    }
    // 13 — IndicateAchievementProgress(name, curProgress, maxProgress).
    //   SDK contract: only valid for achievements with progress
    //   (not yet unlocked); does NOT unlock at maxProgress (game must
    //   still call SetAchievement). Emits UserAchievementStored_t with
    //   the current progress values so HUD popups show "X/Y" toast.
    //   Returns true on a known achievement that isn't already
    //   unlocked, false otherwise.
    virtual bool IndicateAchievementProgress(const char* pchName,
                                             uint32_t nCurProgress,
                                             uint32_t nMaxProgress) {
        if (!pchName) return false;
        uint64_t game_id = static_cast<uint64_t>(pushed().app_id.load());
        {
            std::lock_guard<std::mutex> lk(state_mutex());
            auto it = pushed().achievement_index.find(pchName);
            if (it == pushed().achievement_index.end()) return false;
            if (pushed().achievements[it->second].achieved) return false;
        }
        lsc_cb::UserAchievementStored ach{};
        ach.m_nGameID           = game_id;
        ach.m_bGroupAchievement = false;
        std::strncpy(ach.m_rgchAchievementName, pchName,
                     lsc_cb::kAchievementNameMax - 1);
        ach.m_rgchAchievementName[lsc_cb::kAchievementNameMax - 1] = '\0';
        ach.m_nCurProgress = nCurProgress;
        ach.m_nMaxProgress = nMaxProgress;
        push_callback(state().user.load(),
                      lsc_cb::kUserAchievementStored,
                      &ach, sizeof(ach));
        return true;
    }
    // 14 — GetNumAchievements.
    virtual uint32_t GetNumAchievements() {
        std::lock_guard<std::mutex> lk(state_mutex());
        return static_cast<uint32_t>(pushed().achievements.size());
    }
    // 15 — GetAchievementName(idx). Thread-local stable buffer.
    virtual const char* GetAchievementName(uint32_t idx) {
        static thread_local std::string tls_name;
        tls_name.clear();
        std::lock_guard<std::mutex> lk(state_mutex());
        const auto& a = pushed().achievements;
        if (idx < a.size()) tls_name = a[idx].api_name;
        return tls_name.c_str();
    }
    // 16 — RequestUserStats(steamID) → SteamAPICall_t (UserStatsReceived_t
    //   for the specified user). We don't track per-user remote stats;
    //   issue an hCall and immediately push a UserStatsReceived_t for
    //   the requested SteamID so a registered CCallResult fires with a
    //   reasonable "no data yet" answer.
    virtual uint64_t RequestUserStats(uint64_t steamID) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::UserStatsReceived payload{};
        payload.m_nGameID     = static_cast<uint64_t>(pushed().app_id.load());
        payload.m_eResult     = 6;  // k_EResultNoConnection — we never asked
        payload.m_steamIDUser = steamID;
        push_call_result(h, lsc_cb::kUserStatsReceived,
                         &payload, sizeof(payload), /*io_failure=*/false);
        return h;
    }
    // 17 — GetUserStat(steamID, name, *pInt). Self-only fast path:
    //   if steamID matches pushed.steam_id, read pushed.stats_int.
    //   Other users → false.
    virtual bool GetUserStatInt(uint64_t steamID, const char* pchName, int32_t* pData) {
        if (!pchName || !pData) return false;
        if (steamID != pushed().steam_id.load()) return false;
        return GetStatInt(pchName, pData);
    }
    // 18 — GetUserStat(steamID, name, *pFloat) — float variant.
    virtual bool GetUserStatFloat(uint64_t steamID, const char* pchName, float* pData) {
        if (!pchName || !pData) return false;
        if (steamID != pushed().steam_id.load()) return false;
        return GetStatFloat(pchName, pData);
    }
    // 19 — GetUserAchievement(steamID, name, *pbAchieved). Self-only.
    virtual bool GetUserAchievement(uint64_t steamID, const char* pchName, bool* pbAchieved) {
        if (steamID != pushed().steam_id.load()) return false;
        return GetAchievement(pchName, pbAchieved);
    }
    // 20 — GetUserAchievementAndUnlockTime(...). Self-only.
    virtual bool GetUserAchievementAndUnlockTime(uint64_t steamID, const char* pchName,
                                                  bool* pbAchieved, uint32_t* punlockTime) {
        if (steamID != pushed().steam_id.load()) return false;
        return GetAchievementAndUnlockTime(pchName, pbAchieved, punlockTime);
    }
    // 21 — ResetAllStats(bAchievementsToo). Clears the pushed-state
    //   stats / achievement-progress caches AND marks every known
    //   stat / achievement dirty so the next StoreStats propagates
    //   the reset to Steam. The schema (achievements list, stat-id
    //   map) is preserved — re-population on the next push doesn't
    //   lose layout. Wasted no-op stat uploads (entries that weren't
    //   set in this session) are tolerable; servers de-dup zero
    //   writes against the prior value.
    virtual bool ResetAllStats(bool bAchievementsToo) {
        std::lock_guard<std::mutex> lk(state_mutex());
        // Capture all currently-known stat names so the next
        // StoreStats sends an explicit 0/0.0 for each one — that's
        // how the cleared state reaches Steam.
        for (auto& [name, _] : pushed().stats_int) {
            pushed().dirty_stats_int.insert(name);
        }
        pushed().stats_int.clear();
        for (auto& [name, _] : pushed().stats_float) {
            pushed().dirty_stats_float.insert(name);
        }
        pushed().stats_float.clear();
        if (bAchievementsToo) {
            for (auto& a : pushed().achievements) {
                bool was = a.achieved;
                a.achieved    = false;
                a.unlock_time = 0;
                // Only mark dirty if it was actually unlocked — avoids
                // wasted AND-NOT passes on bits that were already
                // cleared.
                if (was) a.pending_store = true;
            }
        }
        return true;
    }
    // 22-32 — Leaderboards. Out of scope; all async slots return 0
    //   hCall (no callback lands) and sync getters return defaults.
    //   Real leaderboard support would back onto Steam's CMsgClientLBS*.
    // Leaderboard async slots. We don't back leaderboards onto a real
    // server today, but games that touch them on boot need callbacks
    // to fire so their CCallResult dispatch unblocks. Each slot here
    // returns a fresh hCall + posts a "not-found / no-entries / fail"
    // result. Single-player titles proceed; multiplayer leaderboard
    // features show as empty rather than hanging at "loading...".
    virtual uint64_t FindOrCreateLeaderboard(const char* /*name*/,
                                              int /*sortMethod*/, int /*displayType*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardFindResult cb{};
        cb.m_hSteamLeaderboard  = 0;
        cb.m_bLeaderboardFound  = 0;
        push_call_result(h, lsc_cb::kLeaderboardFindResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t FindLeaderboard(const char* /*name*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardFindResult cb{};
        cb.m_hSteamLeaderboard  = 0;
        cb.m_bLeaderboardFound  = 0;
        push_call_result(h, lsc_cb::kLeaderboardFindResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual const char* GetLeaderboardName(uint64_t)                 { return ""; }
    virtual int       GetLeaderboardEntryCount(uint64_t)             { return 0; }
    virtual int       GetLeaderboardSortMethod(uint64_t)             { return 0; }
    virtual int       GetLeaderboardDisplayType(uint64_t)            { return 0; }
    virtual uint64_t DownloadLeaderboardEntries(uint64_t hLeaderboard,
                                                  int /*eRange*/, int /*rangeStart*/,
                                                  int /*rangeEnd*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardScoresDownloaded cb{};
        cb.m_hSteamLeaderboard        = hLeaderboard;
        cb.m_hSteamLeaderboardEntries = 0;
        cb.m_cEntryCount              = 0;
        push_call_result(h, lsc_cb::kLeaderboardScoresDownloaded,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t DownloadLeaderboardEntriesForUsers(uint64_t hLeaderboard,
                                                          uint64_t* /*pUsers*/,
                                                          int /*cUsers*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardScoresDownloaded cb{};
        cb.m_hSteamLeaderboard        = hLeaderboard;
        cb.m_hSteamLeaderboardEntries = 0;
        cb.m_cEntryCount              = 0;
        push_call_result(h, lsc_cb::kLeaderboardScoresDownloaded,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual bool      GetDownloadedLeaderboardEntry(uint64_t, int, void*, int32_t*, int) { return false; }
    virtual uint64_t UploadLeaderboardScore(uint64_t hLeaderboard,
                                             int /*method*/, int32_t score,
                                             const int32_t* /*details*/,
                                             int /*cDetails*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardScoreUploaded cb{};
        cb.m_bSuccess            = 0; // server-side store not implemented
        cb.m_hSteamLeaderboard   = hLeaderboard;
        cb.m_nScore              = score;
        cb.m_bScoreChanged       = 0;
        cb.m_nGlobalRankNew      = 0;
        cb.m_nGlobalRankPrevious = 0;
        push_call_result(h, lsc_cb::kLeaderboardScoreUploaded,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t AttachLeaderboardUGC(uint64_t hLeaderboard, uint64_t /*hUGC*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::LeaderboardUGCSet cb{};
        cb.m_eResult           = 2; // k_EResultFail
        cb.m_hSteamLeaderboard = hLeaderboard;
        push_call_result(h, lsc_cb::kLeaderboardUGCSet,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 33 — GetNumberOfCurrentPlayers → SteamAPICall_t. Server-side
    //   only; we report m_bSuccess=true + m_cPlayers=0 so the UI shows
    //   "0 players online" instead of "—".
    virtual uint64_t GetNumberOfCurrentPlayers() {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::NumberOfCurrentPlayers cb{};
        cb.m_bSuccess = 1;
        cb.m_cPlayers = 0;
        push_call_result(h, lsc_cb::kNumberOfCurrentPlayers,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 34 — RequestGlobalAchievementPercentages → SteamAPICall_t.
    //   Post GameID=app_id, EResult=Fail. GetMostAchievedAchievementInfo
    //   etc. still return -1.
    virtual uint64_t RequestGlobalAchievementPercentages() {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::GlobalAchievementPercentagesReady cb{};
        cb.m_nGameID = static_cast<uint64_t>(pushed().app_id.load());
        cb.m_eResult = 2; // k_EResultFail
        push_call_result(h, lsc_cb::kGlobalAchievementPercentages,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 35-36 — Global achievement most-achieved iteration.
    virtual int       GetMostAchievedAchievementInfo(char*, uint32_t, float*, bool*) { return -1; }
    virtual int       GetNextMostAchievedAchievementInfo(int, char*, uint32_t, float*, bool*) { return -1; }
    // 37 — GetAchievementAchievedPercent(name, *pflPercent).
    virtual bool      GetAchievementAchievedPercent(const char*, float* p) {
        if (p) *p = 0.0f;
        return false;
    }
    // 38 — RequestGlobalStats(historicalDays) → SteamAPICall_t.
    //   Same shape as 34. GameID=app_id, EResult=Fail. The companion
    //   GetGlobalStatInt64/Float/History slots already return false.
    virtual uint64_t RequestGlobalStats(int /*historicalDays*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::GlobalStatsReceived cb{};
        cb.m_nGameID = static_cast<uint64_t>(pushed().app_id.load());
        cb.m_eResult = 2; // k_EResultFail
        push_call_result(h, lsc_cb::kGlobalStatsReceived,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 39-42 — global stat variants. Out of scope for our pushed-state
    //   surface (which tracks only per-app local stats).
    virtual bool      GetGlobalStatInt64(const char*, int64_t* p)    { if (p) *p = 0; return false; }
    virtual bool      GetGlobalStatDouble(const char*, double* p)    { if (p) *p = 0.0; return false; }
    virtual int       GetGlobalStatHistoryInt64(const char*, int64_t*, uint32_t) { return 0; }
    virtual int       GetGlobalStatHistoryDouble(const char*, double*, uint32_t) { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamInventory (version "STEAMINVENTORY_INTERFACE_V003"). Slot map
// from public/steam/isteaminventory.h. Stub returns safe defaults so
// games that probe ISteamInventory don't crash on a null vtable —
// LoadItemDefinitions reports success (no items), GetAllItems reports
// failure (no inventory), every read returns "nothing here".
//
// Real per-app item-def data is fetched separately by wn-session's
// existing InventoryItemsGenerator path and lives in steam_settings/
// items.json (Goldberg-compatible); wiring it through the vtable is
// future work.
class ISteamInventoryStub {
public:
    // 0 — GetResultStatus(SteamInventoryResult_t)
    //   k_EResultInvalidParam=8 when the result-handle isn't ours.
    virtual int       GetResultStatus(int /*resultHandle*/)         { return 8; /*InvalidParam*/ }
    // 1 — GetResultItems(handle, *items, *cb)
    virtual bool      GetResultItems(int, void*, uint32_t* pcb)     { if (pcb) *pcb = 0; return false; }
    // 2 — GetResultItemProperty
    virtual bool      GetResultItemProperty(int, uint32_t, const char*, char* buf, uint32_t* cb) {
        if (buf && cb && *cb > 0) buf[0] = '\0';
        if (cb) *cb = 0;
        return false;
    }
    // 3 — GetResultTimestamp(handle)
    virtual uint32_t  GetResultTimestamp(int)                       { return 0; }
    // 4 — CheckResultSteamID(handle, steamId)
    virtual bool      CheckResultSteamID(int, uint64_t)             { return false; }
    // 5 — DestroyResult(handle) — no-op, we have no result objects to free.
    virtual void      DestroyResult(int)                            {}
    // 6 — GetAllItems(*outHandle)
    virtual bool      GetAllItems(int* phRes)                       { if (phRes) *phRes = -1; return false; }
    // 7 — GetItemsByID(*outHandle, ids[], n)
    virtual bool      GetItemsByID(int* phRes, const uint64_t*, uint32_t) { if (phRes) *phRes = -1; return false; }
    // 8 — SerializeResult(handle, *outBuf, *cb)
    virtual bool      SerializeResult(int, void*, uint32_t* pcb)    { if (pcb) *pcb = 0; return false; }
    // 9 — DeserializeResult(*outHandle, *buf, cbBuf, reserved=true)
    virtual bool      DeserializeResult(int* phRes, const void*, uint32_t, bool) { if (phRes) *phRes = -1; return false; }
    // 10 — GenerateItems(*outHandle, defs[], qtys[], n)
    virtual bool      GenerateItems(int* phRes, const int32_t*, const uint32_t*, uint32_t) { if (phRes) *phRes = -1; return false; }
    // 11 — GrantPromoItems(*outHandle)
    virtual bool      GrantPromoItems(int* phRes)                   { if (phRes) *phRes = -1; return false; }
    // 12 — AddPromoItem(*outHandle, itemDef)
    virtual bool      AddPromoItem(int* phRes, int32_t)             { if (phRes) *phRes = -1; return false; }
    // 13 — AddPromoItems(*outHandle, defs[], n)
    virtual bool      AddPromoItems(int* phRes, const int32_t*, uint32_t) { if (phRes) *phRes = -1; return false; }
    // 14 — ConsumeItem(*outHandle, itemId, qty)
    virtual bool      ConsumeItem(int* phRes, uint64_t, uint32_t)   { if (phRes) *phRes = -1; return false; }
    // 15 — ExchangeItems(*outHandle, recipeDefs[], qtys[], n, takeDefs[], takeQtys[], n)
    virtual bool      ExchangeItems(int* phRes, const int32_t*, const uint32_t*, uint32_t,
                                     const uint64_t*, const uint32_t*, uint32_t) {
        if (phRes) *phRes = -1; return false;
    }
    // 16 — TransferItemQuantity(*outHandle, itemIdSrc, qty, itemIdDst)
    virtual bool      TransferItemQuantity(int* phRes, uint64_t, uint32_t, uint64_t) { if (phRes) *phRes = -1; return false; }
    // 17 — SendItemDropHeartbeat
    virtual void      SendItemDropHeartbeat()                       {}
    // 18 — TriggerItemDrop(*outHandle, dropListDef)
    virtual bool      TriggerItemDrop(int* phRes, int32_t)          { if (phRes) *phRes = -1; return false; }
    // 19 — TradeItems
    virtual bool      TradeItems(int* phRes, uint64_t, const uint64_t*, const uint32_t*,
                                  uint32_t, const uint64_t*, const uint32_t*, uint32_t) {
        if (phRes) *phRes = -1; return false;
    }
    // 20 — LoadItemDefinitions
    //   We don't load from disk on demand; SteamService pre-populates
    //   pushed().inventory_item_defs at game launch. Returns true so
    //   game-side code that probes "are defs available?" proceeds.
    //   Also emits SteamInventoryDefinitionUpdate_t (id 4707) so games
    //   that listen for the callback (rather than re-polling) get
    //   notified. Cost is one tiny callback per call — cheap.
    virtual bool      LoadItemDefinitions() {
        push_callback(state().user.load(), /*kSteamInventoryDefinitionUpdate*/ 4707,
                      nullptr, 0);
        return true;
    }
    // 21 — GetItemDefinitionIDs(defs[], *cb).
    //   When called with defs=nullptr, fills *cb with the count needed.
    //   When called with defs!=nullptr, fills up to *cb entries and
    //   sets *cb to the actual count written. Matches Steamworks SDK
    //   contract for two-pass enumeration.
    virtual bool      GetItemDefinitionIDs(int32_t* defs, uint32_t* pcb) {
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().inventory_item_defs.find(app);
        if (it == pushed().inventory_item_defs.end()) {
            if (pcb) *pcb = 0;
            return true;
        }
        const auto& table = it->second;
        if (!defs) {
            if (pcb) *pcb = static_cast<uint32_t>(table.size());
            return true;
        }
        const uint32_t cap = pcb ? *pcb : 0;
        uint32_t n = 0;
        for (const auto& kv : table) {
            if (n >= cap) break;
            defs[n++] = kv.first;
        }
        if (pcb) *pcb = n;
        return true;
    }
    // 22 — GetItemDefinitionProperty(iDefinition, pchPropertyName,
    //                                pchValueBuffer, *cbValueLength)
    //   pchPropertyName=nullptr → return comma-separated list of all
    //   known property names (SDK contract). Otherwise look up the
    //   single property. *cbValueLength is BOTH the capacity on input
    //   and the bytes-needed on output (including the null terminator).
    virtual bool      GetItemDefinitionProperty(int32_t iDef, const char* propName,
                                                 char* buf, uint32_t* cb) {
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto ait = pushed().inventory_item_defs.find(app);
        if (ait == pushed().inventory_item_defs.end()) {
            if (buf && cb && *cb > 0) buf[0] = '\0';
            if (cb) *cb = 0;
            return false;
        }
        auto dit = ait->second.find(iDef);
        if (dit == ait->second.end()) {
            if (buf && cb && *cb > 0) buf[0] = '\0';
            if (cb) *cb = 0;
            return false;
        }
        std::string value;
        if (!propName || propName[0] == '\0') {
            // Caller wants the full property list. Build comma-joined.
            for (const auto& kv : dit->second) {
                if (!value.empty()) value.push_back(',');
                value.append(kv.first);
            }
        } else {
            auto pit = dit->second.find(propName);
            if (pit == dit->second.end()) {
                if (buf && cb && *cb > 0) buf[0] = '\0';
                if (cb) *cb = 0;
                return false;
            }
            value = pit->second;
        }
        const uint32_t needed = static_cast<uint32_t>(value.size()) + 1; // include NUL
        const uint32_t cap    = cb ? *cb : 0;
        if (buf && cap > 0) {
            const uint32_t copy = (needed <= cap ? needed : cap) - 1;
            std::memcpy(buf, value.data(), copy);
            buf[copy] = '\0';
        }
        if (cb) *cb = needed;
        return true;
    }
    // 23 — RequestEligiblePromoItemDefinitionsIDs(steamId) →
    //   SteamAPICall_t. Used by F2P games to enumerate promo items the
    //   user is eligible for. No backend → result=Fail + 0 defs.
    virtual uint64_t RequestEligiblePromoItemDefinitionsIDs(uint64_t sid) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::SteamInventoryEligiblePromoItemDefIDs cb{};
        cb.m_result                   = 2; // k_EResultFail
        cb.m_steamID                  = sid;
        cb.m_numEligiblePromoItemDefs = 0;
        cb.m_bCachedData              = 0;
        push_call_result(h, lsc_cb::kSteamInventoryEligiblePromoItemDefIDs,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 24 — GetEligiblePromoItemDefinitionIDs(steamId, defs[], *cb)
    virtual bool      GetEligiblePromoItemDefinitionIDs(uint64_t, int32_t*, uint32_t* pcb) { if (pcb) *pcb = 0; return false; }
    // 25 — StartPurchase(defs[], qtys[], n) → SteamAPICall_t. Posts
    //   SteamInventoryStartPurchaseResult_t with Fail so the
    //   microtransaction flow exits cleanly. We don't run a payment
    //   pipeline.
    virtual uint64_t StartPurchase(const int32_t* /*defs*/, const uint32_t* /*qtys*/, uint32_t /*n*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::SteamInventoryStartPurchaseResult cb{};
        cb.m_result     = 2;
        cb.m_ulOrderID  = 0;
        cb.m_ulTransID  = 0;
        push_call_result(h, lsc_cb::kSteamInventoryStartPurchaseResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 26 — RequestPrices() → SteamAPICall_t. Most games gate the
    //   microtransaction UI on the result. Report Fail + USD currency
    //   default so the UI shows "prices unavailable" rather than
    //   hanging.
    virtual uint64_t RequestPrices() {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::SteamInventoryRequestPricesResult cb{};
        cb.m_result = 2;
        cb.m_rgchCurrency[0] = 'U';
        cb.m_rgchCurrency[1] = 'S';
        cb.m_rgchCurrency[2] = 'D';
        cb.m_rgchCurrency[3] = '\0';
        push_call_result(h, lsc_cb::kSteamInventoryRequestPricesResult,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 27 — GetNumItemsWithPrices
    virtual uint32_t  GetNumItemsWithPrices()                       { return 0; }
    // 28 — GetItemsWithPrices(defs[], prices[], basePrices[], n)
    virtual bool      GetItemsWithPrices(int32_t*, uint64_t*, uint64_t*, uint32_t) { return false; }
    // 29 — GetItemPrice(def, *price, *basePrice)
    virtual bool      GetItemPrice(int32_t, uint64_t* p, uint64_t* bp) {
        if (p) *p = 0; if (bp) *bp = 0; return false;
    }
    // 30 — StartUpdateProperties → SteamInventoryUpdateHandle_t
    virtual uint64_t  StartUpdateProperties()                       { return 0; }
    // 31 — RemoveProperty(updateHandle, itemId, propName)
    virtual bool      RemoveProperty(uint64_t, uint64_t, const char*) { return false; }
    // 32-35 — SetProperty (string / bool / int64 / float overloads)
    virtual bool      SetProperty_String(uint64_t, uint64_t, const char*, const char*) { return false; }
    virtual bool      SetProperty_Bool  (uint64_t, uint64_t, const char*, bool)        { return false; }
    virtual bool      SetProperty_Int64 (uint64_t, uint64_t, const char*, int64_t)     { return false; }
    virtual bool      SetProperty_Float (uint64_t, uint64_t, const char*, float)       { return false; }
    // 36 — SubmitUpdateProperties(updateHandle, *resultHandle)
    virtual bool      SubmitUpdateProperties(uint64_t, int* phRes)  { if (phRes) *phRes = -1; return false; }
    // 37 — InspectItem(*resultHandle, encodedItemBuf)
    virtual bool      InspectItem(int* phRes, const char*)          { if (phRes) *phRes = -1; return false; }
};

// ---------------------------------------------------------------------------
// ISteamScreenshots (version "STEAMSCREENSHOTS_INTERFACE_VERSION003").
// Slot map from public/steam/isteamscreenshots.h. Stubs return safe
// defaults — F12 still works at the OS level (Steam overlay would
// normally service it; without one, the screenshot goes to the OS's
// default capture path). Games that *hook* screenshots so they can
// composite their own UI before the snapshot get a no-op here.
class ISteamScreenshotsStub {
public:
    // 0 — WriteScreenshot(rgb, cubRGB, w, h) → ScreenshotHandle_t (uint32)
    //   0 is the documented "invalid handle" sentinel.
    virtual uint32_t  WriteScreenshot(const void*, uint32_t, int, int) { return 0; }
    // 1 — AddScreenshotToLibrary(filename, thumbnail, w, h) → handle
    virtual uint32_t  AddScreenshotToLibrary(const char*, const char*, int, int) { return 0; }
    // 2 — TriggerScreenshot(). No-op; the OS handles F12.
    virtual void      TriggerScreenshot()                            {}
    // 3 — HookScreenshots(hooked). Stored only for IsScreenshotsHooked().
    virtual void      HookScreenshots(bool hooked)                   { hooked_.store(hooked); }
    // 4 — SetLocation(handle, location)
    virtual bool      SetLocation(uint32_t, const char*)             { return false; }
    // 5 — TagUser(handle, steamID)
    virtual bool      TagUser(uint32_t, uint64_t)                    { return false; }
    // 6 — TagPublishedFile(handle, publishedFileID)
    virtual bool      TagPublishedFile(uint32_t, uint64_t)           { return false; }
    // 7 — IsScreenshotsHooked()
    virtual bool      IsScreenshotsHooked()                          { return hooked_.load(); }
    // 8 — AddVRScreenshotToLibrary(type, filename, vrFilename) → handle
    virtual uint32_t  AddVRScreenshotToLibrary(int, const char*, const char*) { return 0; }
private:
    std::atomic<bool> hooked_{false};
};

// ---------------------------------------------------------------------------
// ISteamMusic (version "STEAMMUSIC_INTERFACE_VERSION001"). Slot map
// from public/steam/isteammusic.h. We don't currently have an audio
// pipeline; all reads report "no music playing".
class ISteamMusicStub {
public:
    // 0 — BIsEnabled
    virtual bool      BIsEnabled()                                   { return false; }
    // 1 — BIsPlaying
    virtual bool      BIsPlaying()                                   { return false; }
    // 2 — GetPlaybackStatus → AudioPlayback_Status (int): 0=Undefined
    virtual int       GetPlaybackStatus()                            { return 0; }
    // 3 — Play() / 4 — Pause() / 5 — PlayPrevious() / 6 — PlayNext()
    virtual void      Play()                                         {}
    virtual void      Pause()                                        {}
    virtual void      PlayPrevious()                                 {}
    virtual void      PlayNext()                                     {}
    // 7 — SetVolume(volume) / 8 — GetVolume()
    virtual void      SetVolume(float)                               {}
    virtual float     GetVolume()                                    { return 0.0f; }
};

// ---------------------------------------------------------------------------
// ISteamAppList (version "STEAMAPPLIST_INTERFACE_VERSION001"). Mostly
// surfaced to Steam-internal tooling; modern games rarely probe it,
// but Source-engine launchers do. Slot map from public/steam/
// isteamapplist.h. Reads serve the user's owned set + per-app
// metadata that we already have in pushed state.
class ISteamAppListStub {
public:
    // 0 — GetNumInstalledApps. Reuses pushed.installed_apps.
    virtual uint32_t  GetNumInstalledApps() {
        std::lock_guard<std::mutex> lk(state_mutex());
        return static_cast<uint32_t>(pushed().installed_apps.size());
    }
    // 1 — GetInstalledApps(*pvecAppID, cMax). Copies up to cMax appids
    //   from pushed.installed_apps; returns the number written.
    virtual uint32_t  GetInstalledApps(uint32_t* pvecAppID, uint32_t cMax) {
        std::lock_guard<std::mutex> lk(state_mutex());
        const auto& set = pushed().installed_apps;
        uint32_t total = static_cast<uint32_t>(set.size());
        uint32_t copy  = std::min<uint32_t>(total, cMax);
        if (pvecAppID && copy > 0) {
            uint32_t i = 0;
            for (uint32_t id : set) {
                if (i >= copy) break;
                pvecAppID[i++] = id;
            }
        }
        return copy;
    }
    // 2 — GetAppName(appId, *pName, cMaxName)
    //   Returns the byte count copied (per the SDK contract — same
    //   shape as GetLaunchCommandLine slot 26). Reads from
    //   pushed.app_names; empty buffer + 0 when the appId isn't
    //   in the cache. Names come from the Room steam_app table
    //   (SteamService.onCreate seed) and the wn-session library
    //   snapshot.
    virtual int       GetAppName(uint32_t appId, char* pName, int cMaxName) {
        if (!pName || cMaxName <= 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_names.find(appId);
        if (it == pushed().app_names.end()) { pName[0] = '\0'; return 0; }
        const std::string& n = it->second;
        int copy = std::min<int>(static_cast<int>(n.size()), cMaxName - 1);
        if (copy > 0) std::memcpy(pName, n.data(), copy);
        pName[copy] = '\0';
        return copy;
    }
    // 3 — GetAppInstallDir(appId, pDir, cMaxDir) — same as ISteamApps slot 18.
    virtual int       GetAppInstallDir(uint32_t appId, char* pDir, int cMaxDir) {
        if (!pDir || cMaxDir <= 0) return 0;
        std::lock_guard<std::mutex> lk(state_mutex());
        auto it = pushed().app_install_dirs.find(appId);
        if (it == pushed().app_install_dirs.end()) { pDir[0] = '\0'; return 0; }
        const std::string& d = it->second;
        int copy = std::min<int>(static_cast<int>(d.size()), cMaxDir - 1);
        if (copy > 0) std::memcpy(pDir, d.data(), copy);
        pDir[copy] = '\0';
        return copy;
    }
    // 4 — GetAppBuildId(appId). Not in pushed state yet; return 0.
    virtual int       GetAppBuildId(uint32_t)                        { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamVideo (version "STEAMVIDEO_INTERFACE_V007"). Slot map from
// public/steam/isteamvideo.h. Used by games with embedded Steam-
// Broadcasting / Steam.tv integration. All slots return safe "no
// video / no broadcast" defaults.
class ISteamVideoStub {
public:
    // 0 — GetVideoURL(unVideoAppID). Async — returns SteamAPICall_t.
    //   We don't issue the request, so callers wait for a result that
    //   never lands. Returning 0 = "invalid call" matches the SDK
    //   sentinel for failed dispatch.
    virtual uint64_t  GetVideoURL_DEPRECATED(uint32_t)               { return 0; }
    // 1 — IsBroadcasting(*pnNumViewers)
    virtual bool      IsBroadcasting(int* pnNumViewers) {
        if (pnNumViewers) *pnNumViewers = 0;
        return false;
    }
    // 2 — GetOPFSettings(unVideoAppID). Async.
    virtual uint64_t  GetOPFSettings(uint32_t)                       { return 0; }
    // 3 — GetOPFStringForApp(unVideoAppID, *pchBuf, *pnBufSize)
    virtual bool      GetOPFStringForApp(uint32_t, char* buf, int32_t* pnBufSize) {
        if (buf && pnBufSize && *pnBufSize > 0) buf[0] = '\0';
        if (pnBufSize) *pnBufSize = 0;
        return false;
    }
};

// ---------------------------------------------------------------------------
// ISteamParentalSettings (version "STEAMPARENTALSETTINGS_INTERFACE_VERSION001").
// Slot map from public/steam/isteamparentalsettings.h. All slots
// return "no parental lock active" — matches the default Steam
// Family-View-disabled posture. Games that gate features on family-
// view restrictions (mature-content unlocks, in-game spending caps)
// proceed without restriction.
class ISteamParentalSettingsStub {
public:
    // 0 — BIsParentalLockEnabled
    virtual bool      BIsParentalLockEnabled()                       { return false; }
    // 1 — BIsParentalLockLocked
    virtual bool      BIsParentalLockLocked()                        { return false; }
    // 2 — BIsAppBlocked(appId)
    virtual bool      BIsAppBlocked(uint32_t)                        { return false; }
    // 3 — BIsAppInBlockList(appId)
    virtual bool      BIsAppInBlockList(uint32_t)                    { return false; }
    // 4 — BIsFeatureBlocked(EParentalFeature)
    virtual bool      BIsFeatureBlocked(int)                         { return false; }
    // 5 — BIsFeatureInBlockList(EParentalFeature)
    virtual bool      BIsFeatureInBlockList(int)                     { return false; }
};

// ---------------------------------------------------------------------------
// ISteamMatchmakingServers (version "SteamMatchMakingServers002").
// Server-browser surface — games like Source-engine titles, ARMA,
// Insurgency, etc. probe this to render their in-game server list.
// Slot map from public/steam/isteammatchmakingservers.h.
//
// All slots return safe "no request in flight / no servers" defaults.
// Real implementation would back onto Valve's master-server protocol
// (UDP A2S queries against the SteamMasterServerUpdater output), but
// no game on this platform has been observed needing it yet.
class ISteamMatchmakingServersStub {
public:
    // 0..5 Request*ServerList — return a non-null sentinel handle (1)
    //   rather than null. Real Steam returns HServerListRequest that
    //   the caller passes back through GetServerCount / RefreshQuery /
    //   ReleaseRequest. Returning null could make games crash on the
    //   "auto-deref" pattern foo->vtable. With a sentinel, downstream
    //   slots (GetServerCount=0, IsRefreshing=false) report an empty
    //   completed list which most game UIs render as "0 servers" —
    //   cleaner than a crash or "Failed to fetch" dialog. Logs each
    //   probe so we know what kind of server browsing the game tried.
    // reinterpret_cast<void*> isn't constexpr-evaluable pre-C++20, so
    // hold the sentinel as a static const POD pointer instead.
    static void* fake_handle() {
        return reinterpret_cast<void*>(uintptr_t{1});
    }
    // 0 — RequestInternetServerList (filters → response IF)  → HServerListRequest
    virtual void*     RequestInternetServerList(uint32_t app, void**, uint32_t n, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestInternetServerList app=%u nFilters=%u",
            app, n);
        return fake_handle();
    }
    // 1 — RequestLANServerList(unAppID, *response) → HServerListRequest
    virtual void*     RequestLANServerList(uint32_t app, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestLANServerList app=%u", app);
        return fake_handle();
    }
    // 2 — RequestFriendsServerList
    virtual void*     RequestFriendsServerList(uint32_t app, void**, uint32_t, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestFriendsServerList app=%u", app);
        return fake_handle();
    }
    // 3 — RequestFavoritesServerList
    virtual void*     RequestFavoritesServerList(uint32_t app, void**, uint32_t, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestFavoritesServerList app=%u", app);
        return fake_handle();
    }
    // 4 — RequestHistoryServerList
    virtual void*     RequestHistoryServerList(uint32_t app, void**, uint32_t, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestHistoryServerList app=%u", app);
        return fake_handle();
    }
    // 5 — RequestSpectatorServerList
    virtual void*     RequestSpectatorServerList(uint32_t app, void**, uint32_t, void*) {
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmakingServers.RequestSpectatorServerList app=%u", app);
        return fake_handle();
    }
    // 6 — ReleaseRequest
    virtual void      ReleaseRequest(void*)                          {}
    // 7 — GetServerDetails(hRequest, iServer) → gameserveritem_t*
    virtual void*     GetServerDetails(void*, int)                   { return nullptr; }
    // 8 — CancelQuery
    virtual void      CancelQuery(void*)                             {}
    // 9 — RefreshQuery
    virtual void      RefreshQuery(void*)                            {}
    // 10 — IsRefreshing
    virtual bool      IsRefreshing(void*)                            { return false; }
    // 11 — GetServerCount
    virtual int       GetServerCount(void*)                          { return 0; }
    // 12 — RefreshServer(hRequest, iServer)
    virtual void      RefreshServer(void*, int)                      {}
    // 13 — PingServer(unIP, usPort, response*) → HServerQuery
    virtual int       PingServer(uint32_t, uint16_t, void*)          { return -1; /*HSERVERQUERY_INVALID*/ }
    // 14 — PlayerDetails(unIP, usPort, response*) → HServerQuery
    virtual int       PlayerDetails(uint32_t, uint16_t, void*)       { return -1; }
    // 15 — ServerRules(unIP, usPort, response*) → HServerQuery
    virtual int       ServerRules(uint32_t, uint16_t, void*)         { return -1; }
    // 16 — CancelServerQuery
    virtual void      CancelServerQuery(int)                         {}
};

// ---------------------------------------------------------------------------
// ISteamMatchmaking (version "SteamMatchMaking009"). Lobby creation/
// join surface used by P2P-multiplayer games. Slot map from
// public/steam/isteammatchmaking.h.
//
// Stubs report "no lobbies / no members" — async slots return 0 hCall
// (no callback will land), sync slots return CSteamID 0 / empty data.
// Real implementation would back onto Steam's Matchmaking service
// methods + CMsgClientLobbyOps; deferred until needed by a tested
// game.
// Per-thread filter accumulator. The SDK lets games call
// AddRequestLobbyList*Filter (slots 5-11) zero-or-more times before
// finally calling RequestLobbyList (slot 4). Each filter setter must
// remember the value for the next RequestLobbyList call. Thread-local
// keeps games that drive matchmaking from multiple threads independent;
// matches what gbe_fork's local impl does internally.
struct PendingLobbyFilters {
    struct Entry {
        std::string key;
        std::string value;
        int32_t     comparison  = 0;
        int32_t     filter_type = 0;
    };
    std::vector<Entry> entries;
    int32_t            num_results = 50;
    int32_t            distance    = 1;   // k_ELobbyDistanceFilterDefault
};

static thread_local PendingLobbyFilters tls_lobby_filters;

class ISteamMatchmakingStub {
public:
    // 0 — GetFavoriteGameCount
    virtual int       GetFavoriteGameCount()                         { return 0; }
    // 1 — GetFavoriteGame(idx, *appID, *ip, *port, *queryPort, *flags, *lastPlayed)
    virtual bool      GetFavoriteGame(int, uint32_t*, uint32_t*, uint16_t*,
                                       uint16_t*, uint32_t*, uint32_t*) { return false; }
    // 2 — AddFavoriteGame(appID, ip, port, queryPort, flags, lastPlayed) → index
    virtual int       AddFavoriteGame(uint32_t, uint32_t, uint16_t,
                                       uint16_t, uint32_t, uint32_t)   { return -1; }
    // 3 — RemoveFavoriteGame(appID, ip, port, queryPort, flags)
    virtual bool      RemoveFavoriteGame(uint32_t, uint32_t, uint16_t,
                                          uint16_t, uint32_t)          { return false; }
    // 4 — RequestLobbyList() → SteamAPICall_t (LobbyMatchList_t).
    //   Real CM round-trip: flushes the per-thread filter accumulator
    //   into wn_cm_lobby_get_list, awaits ClientMMSGetLobbyListResponse
    //   on the CM transport thread, and emits LobbyMatchList_t + seeds
    //   pushed().lobby_match_list. Synthetic failure (not logged on /
    //   transport error) falls back to an empty list so the game's
    //   "Searching for games..." UI unblocks rather than hangs.
    virtual uint64_t RequestLobbyList() {
        uint64_t h = alloc_api_call_handle();
        // Snapshot + clear the per-thread filter accumulator so the
        // next batch of AddRequestLobbyList*Filter calls (which the
        // game may make before the response lands) accumulate fresh.
        PendingLobbyFilters f = std::move(tls_lobby_filters);
        tls_lobby_filters = PendingLobbyFilters{};
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmaking.RequestLobbyList hCall=0x%llx app=%u filters=%zu",
            (unsigned long long)h, pushed().app_id.load(),
            f.entries.size());

        std::vector<std::string> keys_storage, values_storage;
        std::vector<const char*> keys, values;
        std::vector<int32_t>     comparisons, types;
        keys_storage.reserve(f.entries.size());
        values_storage.reserve(f.entries.size());
        keys.reserve(f.entries.size());
        values.reserve(f.entries.size());
        comparisons.reserve(f.entries.size());
        types.reserve(f.entries.size());
        for (auto& e : f.entries) {
            keys_storage.push_back(std::move(e.key));
            values_storage.push_back(std::move(e.value));
            keys.push_back(keys_storage.back().c_str());
            values.push_back(values_storage.back().c_str());
            comparisons.push_back(e.comparison);
            types.push_back(e.filter_type);
        }

        const uint32_t app = pushed().app_id.load();
        bool dispatched = wn_cm_lobby_get_list(
            h,
            app,
            f.num_results,
            keys.empty() ? nullptr : keys.data(),
            values.empty() ? nullptr : values.data(),
            comparisons.empty() ? nullptr : comparisons.data(),
            types.empty() ? nullptr : types.data(),
            keys.size(),
            [](uint64_t hCall, int32_t eresult,
               const WnCmLobbyEntry* lobbies, size_t count) {
                std::vector<uint64_t> sids;
                sids.reserve(count);
                if (lobbies && eresult >= 0) {
                    auto guard = std::lock_guard{state_mutex()};
                    for (size_t i = 0; i < count; ++i) {
                        sids.push_back(lobbies[i].steam_id);
                        // Seed a minimal LobbyState entry so a follow-up
                        // GetLobbyByIndex → GetNumLobbyMembers / GetLobby
                        // MemberLimit / GetLobbyOwner read returns the
                        // counts the list response carried, without
                        // needing a separate RequestLobbyData round-trip
                        // first.
                        auto& L = pushed().active_lobbies[lobbies[i].steam_id];
                        L.max_members = lobbies[i].max_members;
                        // num_members carried in pushed.LobbyState .members
                        // would be misleading (we don't have the SIDs yet),
                        // so we keep .members empty until ClientMMSLobbyData
                        // arrives. callers that ask GetNumLobbyMembers
                        // before that get 0 — matches the SDK contract
                        // ("you must RequestLobbyData first").
                    }
                    pushed().lobby_match_list = sids;
                }
                lsc_cb::LobbyMatchList cb{};
                cb.m_nLobbiesMatching = static_cast<uint32_t>(sids.size());
                push_call_result(hCall, lsc_cb::kLobbyMatchList,
                                  &cb, sizeof(cb),
                                  /*io_failure=*/(eresult < 0));
            });
        if (!dispatched) {
            // No active CMClient / not logged on. Fire an empty
            // LobbyMatchList_t synchronously so games unblock.
            lsc_cb::LobbyMatchList cb{};
            cb.m_nLobbiesMatching = 0;
            push_call_result(h, lsc_cb::kLobbyMatchList,
                             &cb, sizeof(cb), /*io_failure=*/true);
        }
        return h;
    }
    // 5-11 — Filter setters accumulate into the per-thread state.
    virtual void AddRequestLobbyListStringFilter(const char* k, const char* v, int cmp) {
        if (!k || !v) return;
        tls_lobby_filters.entries.push_back({k, v, cmp, /*String*/ 0});
    }
    virtual void AddRequestLobbyListNumericalFilter(const char* k, int v, int cmp) {
        if (!k) return;
        tls_lobby_filters.entries.push_back({k, std::to_string(v), cmp, /*Numerical*/ 1});
    }
    virtual void AddRequestLobbyListNearValueFilter(const char* k, int v) {
        if (!k) return;
        tls_lobby_filters.entries.push_back({k, std::to_string(v), /*cmp*/ 0, /*NearValue*/ 3});
    }
    virtual void AddRequestLobbyListFilterSlotsAvailable(int slots) {
        tls_lobby_filters.entries.push_back({"", std::to_string(slots), 0, /*SlotsAvail*/ 2});
    }
    virtual void AddRequestLobbyListDistanceFilter(int eDist) {
        tls_lobby_filters.distance = eDist;
        tls_lobby_filters.entries.push_back({"", std::to_string(eDist), 0, /*Distance*/ 4});
    }
    virtual void AddRequestLobbyListResultCountFilter(int n) {
        if (n > 0) tls_lobby_filters.num_results = n;
    }
    virtual void AddRequestLobbyListCompatibleMembersFilter(uint64_t /*sid*/) {}
    // 12 — GetLobbyByIndex(idx) → CSteamID. Read from
    //   pushed().lobby_match_list (seeded by RequestLobbyList's
    //   callback). Out-of-range → 0.
    virtual uint64_t GetLobbyByIndex(int idx) {
        if (idx < 0) return 0;
        auto guard = std::lock_guard{state_mutex()};
        const auto& v = pushed().lobby_match_list;
        if (static_cast<size_t>(idx) >= v.size()) return 0;
        return v[idx];
    }
    // 13 — CreateLobby(eLobbyType, maxMembers) → SteamAPICall_t.
    //   Real CM round-trip via wn_cm_lobby_create. On success the
    //   bridge has already seeded pushed().active_lobbies[sid] with the
    //   host as the sole member; we emit BOTH LobbyCreated_t (the
    //   CCallResult the game waited on) AND LobbyEnter_t (the regular
    //   callback that fires when the host enters its own lobby, per
    //   SDK contract). On failure we fire LobbyCreated_t with
    //   eResult=Fail and skip LobbyEnter_t.
    virtual uint64_t CreateLobby(int eLobbyType, int maxMembers) {
        const uint64_t h = alloc_api_call_handle();
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmaking.CreateLobby hCall=0x%llx type=%d maxMembers=%d",
            (unsigned long long)h, eLobbyType, maxMembers);
        bool dispatched = wn_cm_lobby_create(
            h, pushed().app_id.load(),
            static_cast<int32_t>(eLobbyType),
            static_cast<int32_t>(maxMembers > 0 ? maxMembers : 4),
            [](uint64_t hCall, int32_t eresult, uint64_t lobby_sid) {
                lsc_cb::LobbyCreated cb{};
                cb.m_eResult        = (eresult > 0) ? eresult : 2; // synthetic fail → Fail
                cb.m_ulSteamIDLobby = lobby_sid;
                push_call_result(hCall, lsc_cb::kLobbyCreated,
                                 &cb, sizeof(cb),
                                 /*io_failure=*/(eresult < 0));
                if (cb.m_eResult == 1 && lobby_sid != 0) {
                    // Host auto-enters the new lobby. Mirror SDK
                    // behavior: fire LobbyEnter_t as a regular cb,
                    // not a CCallResult.
                    lsc_cb::LobbyEnter le{};
                    le.m_ulSteamIDLobby         = lobby_sid;
                    le.m_rgfChatPermissions     = 0;
                    le.m_bLocked                = 0;
                    le.m_EChatRoomEnterResponse = 1; // Success
                    push_callback(state().user.load(),
                                  lsc_cb::kLobbyEnter, &le, sizeof(le));
                }
            });
        if (!dispatched) {
            lsc_cb::LobbyCreated cb{};
            cb.m_eResult        = 2; // k_EResultFail
            cb.m_ulSteamIDLobby = 0;
            push_call_result(h, lsc_cb::kLobbyCreated,
                             &cb, sizeof(cb), /*io_failure=*/true);
        }
        return h;
    }
    // 14 — JoinLobby(lobbySid) → SteamAPICall_t (LobbyEnter_t).
    //   Real CM round-trip via wn_cm_lobby_join. Bridge has primed
    //   the lobby cache before this callback fires.
    virtual uint64_t JoinLobby(uint64_t lobbySid) {
        const uint64_t h = alloc_api_call_handle();
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmaking.JoinLobby hCall=0x%llx lobby=0x%llx",
            (unsigned long long)h, (unsigned long long)lobbySid);
        bool dispatched = wn_cm_lobby_join(
            h, pushed().app_id.load(), lobbySid,
            [](uint64_t hCall, int32_t chat_resp, uint64_t lobby_sid) {
                lsc_cb::LobbyEnter cb{};
                cb.m_ulSteamIDLobby         = lobby_sid;
                cb.m_rgfChatPermissions     = 0;
                cb.m_bLocked                = 0;
                // Map synthetic failure (-1) to EChatRoomEnterResponseError(2);
                // otherwise pass through Steam's response code.
                cb.m_EChatRoomEnterResponse = (chat_resp > 0) ? chat_resp : 2;
                push_call_result(hCall, lsc_cb::kLobbyEnter,
                                 &cb, sizeof(cb),
                                 /*io_failure=*/(chat_resp < 0));
            });
        if (!dispatched) {
            lsc_cb::LobbyEnter cb{};
            cb.m_ulSteamIDLobby         = lobbySid;
            cb.m_EChatRoomEnterResponse = 2; // Error
            push_call_result(h, lsc_cb::kLobbyEnter,
                             &cb, sizeof(cb), /*io_failure=*/true);
        }
        return h;
    }
    // 15 — LeaveLobby(lobbySid). Fire-and-forget CM message + drop the
    //   local cache entry so subsequent GetLobbyData returns "" rather
    //   than stale state. wn_cm_lobby_leave is no-op when CMClient is
    //   inactive (offline games still get the local cache drop).
    virtual void LeaveLobby(uint64_t sid) {
        if (sid == 0) return;
        __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
            "ISteamMatchmaking.LeaveLobby lobby=0x%llx",
            (unsigned long long)sid);
        wn_cm_lobby_leave(pushed().app_id.load(), sid);
        auto guard = std::lock_guard{state_mutex()};
        pushed().active_lobbies.erase(sid);
    }
    // 16 — InviteUserToLobby(lobbySid, inviteeSid). Asks Steam to
    //   surface a lobby-invite notification on the invitee's Steam
    //   client (overlay popup if they're in-game, Friends-list popup
    //   otherwise, Friends chat fallback if offline). Fire-and-forget;
    //   the SDK contract is that the return reports "did we
    //   successfully ASK Steam to deliver", not whether the invitee
    //   accepts.
    virtual bool      InviteUserToLobby(uint64_t sid, uint64_t invitee) {
        if (sid == 0 || invitee == 0) return false;
        // SDK contract requires the caller to be IN the lobby. Reject
        // dangling invites against lobbies we don't have cached so we
        // don't ship a doomed CM round-trip.
        {
            auto guard = std::lock_guard{state_mutex()};
            if (pushed().active_lobbies.find(sid)
                    == pushed().active_lobbies.end()) {
                return false;
            }
        }
        return wn_cm_lobby_invite_user(pushed().app_id.load(), sid, invitee);
    }
    // 17 — GetNumLobbyMembers(lobbySid) — read from pushed cache.
    virtual int GetNumLobbyMembers(uint64_t sid) {
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return 0;
        return static_cast<int>(it->second.members.size());
    }
    // 18 — GetLobbyMemberByIndex(lobbySid, idx) → CSteamID
    virtual uint64_t GetLobbyMemberByIndex(uint64_t sid, int idx) {
        if (idx < 0) return 0;
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return 0;
        int n = 0;
        for (const auto& kv : it->second.members) {
            if (n++ == idx) return kv.first;
        }
        return 0;
    }
    // 19 — GetLobbyData(lobbySid, key) → const char* (TLS-stable).
    virtual const char* GetLobbyData(uint64_t sid, const char* key) {
        static thread_local std::string tls;
        if (!key) { tls.clear(); return tls.c_str(); }
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) { tls.clear(); return tls.c_str(); }
        auto kt = it->second.data.find(key);
        tls = (kt == it->second.data.end()) ? std::string{} : kt->second;
        return tls.c_str();
    }
    // 20 — SetLobbyData. Two-step: (1) local cache write for immediate
    //   read-back consistency, (2) CMsgClientMMSSetLobbyData round-trip
    //   so all other lobby members see the new value via Steam's 6612
    //   LobbyData push. Steam echoes the whole metadata KV blob, so we
    //   serialize the FULL current lobby data map (including the new
    //   key) — not just the diff — matching the SDK's atomic-replace
    //   contract.
    //
    //   Metadata wire format: SteamKit/Steamworks pack KVs as repeated
    //   c-string pairs (key\0 value\0 ... ) terminated by an extra \0.
    //   This matches Valve's CMsgKeyValuePair pack and the
    //   ISteamMatchmaking sample code.
    virtual bool SetLobbyData(uint64_t sid, const char* key, const char* val) {
        if (sid == 0 || !key) return false;
        std::string blob;
        int32_t max_members = 0;
        int32_t lobby_type  = 0;
        int32_t lobby_flags = 0;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto& L = pushed().active_lobbies[sid];
            L.data[key] = val ? val : "";
            max_members = L.max_members;
            lobby_type  = L.lobby_type;
            lobby_flags = L.lobby_flags;
            // Serialize the full data dict as cstr-pairs.
            for (const auto& kv : L.data) {
                blob.append(kv.first);
                blob.push_back('\0');
                blob.append(kv.second);
                blob.push_back('\0');
            }
            blob.push_back('\0');  // double-null terminator
        }
        // Dispatch to CM, fire-and-forget on the callback side — game
        // doesn't need a CCallResult for SetLobbyData (SDK contract).
        // We allocate a throwaway hCall for the bridge.
        const uint64_t h = alloc_api_call_handle();
        wn_cm_lobby_set_data(h, pushed().app_id.load(), sid,
                             /*steam_id_member=*/0,
                             reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size(),
                             max_members, lobby_type, lobby_flags,
                             [](uint64_t /*hCall*/, int32_t /*eresult*/) {
                                 // No-op — game polls via LobbyDataUpdate_t
                                 // which the 6612 push handler fires.
                             });
        return true;
    }
    // 21 — GetLobbyDataCount
    virtual int GetLobbyDataCount(uint64_t sid) {
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return 0;
        return static_cast<int>(it->second.data.size());
    }
    // 22 — GetLobbyDataByIndex(lobbySid, idx, *key, kn, *val, vn)
    virtual bool GetLobbyDataByIndex(uint64_t sid, int idx, char* key, int kn,
                                      char* val, int vn) {
        if (key && kn > 0) key[0] = '\0';
        if (val && vn > 0) val[0] = '\0';
        if (idx < 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return false;
        int n = 0;
        for (const auto& kv : it->second.data) {
            if (n++ != idx) continue;
            if (key && kn > 0) {
                const auto cc = (kv.first.size() < static_cast<size_t>(kn - 1)
                                  ? kv.first.size() : static_cast<size_t>(kn - 1));
                std::memcpy(key, kv.first.data(), cc);
                key[cc] = '\0';
            }
            if (val && vn > 0) {
                const auto cc = (kv.second.size() < static_cast<size_t>(vn - 1)
                                  ? kv.second.size() : static_cast<size_t>(vn - 1));
                std::memcpy(val, kv.second.data(), cc);
                val[cc] = '\0';
            }
            return true;
        }
        return false;
    }
    // 23 — DeleteLobbyData
    virtual bool DeleteLobbyData(uint64_t sid, const char* key) {
        if (sid == 0 || !key) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return false;
        return it->second.data.erase(key) > 0;
    }
    // 24 — GetLobbyMemberData(lobbySid, memberSid, key) — TLS empty
    virtual const char* GetLobbyMemberData(uint64_t sid, uint64_t member, const char* key) {
        static thread_local std::string tls;
        tls.clear();
        if (!key) return tls.c_str();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return tls.c_str();
        auto mt = it->second.members.find(member);
        if (mt == it->second.members.end()) return tls.c_str();
        auto kt = mt->second.data.find(key);
        if (kt == mt->second.data.end()) return tls.c_str();
        tls = kt->second;
        return tls.c_str();
    }
    // 25 — SetLobbyMemberData(lobbySid, key, val). Sets the caller's own
    //   member-data slot inside the lobby (SDK contract: you can only set
    //   your own). Mirror of SetLobbyData's two-step model but scoped to
    //   pushed().steam_id within active_lobbies[sid].members:
    //     (1) write local cache so a follow-up GetLobbyMemberData
    //         self-read returns the new value without waiting for the
    //         6612 LobbyData push round-trip.
    //     (2) wn_cm_lobby_set_data with steam_id_member = self so Steam
    //         broadcasts a LobbyData push to every member; the observer
    //         then mirrors the new map into active_lobbies and other
    //         members see it via GetLobbyMemberData. Empty/null val
    //         REMOVES the key — matches the SDK's "set to empty to
    //         clear" semantic that Forest relies on for "ready" toggles.
    virtual void      SetLobbyMemberData(uint64_t sid, const char* key,
                                         const char* val) {
        if (sid == 0 || !key) return;
        const uint64_t self = pushed().steam_id.load();
        if (self == 0) return;
        std::string blob;
        int32_t max_members = 0;
        int32_t lobby_type  = 0;
        int32_t lobby_flags = 0;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto& L = pushed().active_lobbies[sid];
            auto& M = L.members[self];
            if (val && *val) M.data[key] = val;
            else             M.data.erase(key);
            max_members = L.max_members;
            lobby_type  = L.lobby_type;
            lobby_flags = L.lobby_flags;
            for (const auto& kv : M.data) {
                blob.append(kv.first);
                blob.push_back('\0');
                blob.append(kv.second);
                blob.push_back('\0');
            }
            blob.push_back('\0');
        }
        const uint64_t h = alloc_api_call_handle();
        wn_cm_lobby_set_data(h, pushed().app_id.load(), sid,
                             /*steam_id_member=*/self,
                             reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size(),
                             max_members, lobby_type, lobby_flags,
                             [](uint64_t /*hCall*/, int32_t /*eresult*/) {
                                 // No-op — game polls via LobbyDataUpdate_t
                                 // which the 6612 push handler fires for
                                 // both lobby-level and member-level
                                 // metadata changes.
                             });
    }
    // 26 — SendLobbyChatMsg(lobbySid, *body, bytes). Fire-and-forget;
    //   Steam relays the message back as a 6614 ClientMMSLobbyChatMsg
    //   push (including the sender's own copy) which the on_lobby_chat
    //   _msg_event observer rings into pushed().lobby_chat_buffer +
    //   emits LobbyChatMsg_t (507).
    virtual bool      SendLobbyChatMsg(uint64_t sid, const void* body, int n) {
        if (sid == 0 || !body || n <= 0) return false;
        return wn_cm_lobby_send_chat(pushed().app_id.load(), sid,
                                     static_cast<const uint8_t*>(body),
                                     static_cast<size_t>(n));
    }
    // 27 — GetLobbyChatEntry(lobbySid, idx, *speakerSid, *body, bn,
    //   *chatType) → bytes. idx is the m_iChatID handed out by the
    //   LobbyChatMsg_t callback (= ring position). Returns the length
    //   of the body actually copied; 0 if no such entry or buffer
    //   too small.
    virtual int       GetLobbyChatEntry(uint64_t sid, int idx,
                                         uint64_t* speaker_out,
                                         void* body_out, int body_cap,
                                         int* chat_type_out) {
        if (speaker_out)   *speaker_out   = 0;
        if (chat_type_out) *chat_type_out = 0;
        if (sid == 0 || idx < 0 || !body_out || body_cap <= 0) return 0;
        auto guard = std::lock_guard{state_mutex()};
        auto bt = pushed().lobby_chat_buffer.find(sid);
        if (bt == pushed().lobby_chat_buffer.end()) return 0;
        const auto& ring = bt->second;
        if (static_cast<size_t>(idx) >= ring.size()) return 0;
        const auto& e = ring[static_cast<size_t>(idx)];
        if (speaker_out)   *speaker_out   = e.sender_sid;
        if (chat_type_out) *chat_type_out = e.chat_type;
        const int n = static_cast<int>(
            std::min<size_t>(e.body.size(),
                              static_cast<size_t>(body_cap)));
        if (n > 0) std::memcpy(body_out, e.body.data(),
                               static_cast<size_t>(n));
        return n;
    }
    // 28 — RequestLobbyData(lobbySid). SDK contract: a LobbyDataUpdate_t
    //   (callback 505) MUST fire after this returns. Invite-link join
    //   flows (steam://joinlobby/<app>/<sid>/<host>) call this from
    //   the Forest's lobby-browser UI to refresh a lobby's metadata
    //   without re-listing.
    //
    //   We synthesize the callback immediately from pushed().active_
    //   lobbies[sid] — m_bSuccess=true if we know the lobby (it came
    //   back in a recent GetLobbyList or we already joined), false
    //   otherwise. A fresh CM round-trip for one-shot metadata isn't
    //   exposed by the public mms proto — GetLobbyList with a single-
    //   SID filter would refresh it, but games hit RequestLobbyData
    //   most often when they're already in the lobby and pushes are
    //   flowing, so the cached read is what they expect.
    virtual bool      RequestLobbyData(uint64_t sid) {
        if (sid == 0) return false;
        bool have = false;
        {
            auto guard = std::lock_guard{state_mutex()};
            have = pushed().active_lobbies.find(sid) !=
                   pushed().active_lobbies.end();
        }
        // LobbyDataUpdate_t (kLobbyDataUpdate = 505). Layout (24B):
        //   uint64 m_ulSteamIDLobby
        //   uint64 m_ulSteamIDMember  (== lobby for lobby-level update)
        //   uint8  m_bSuccess
        //   pad x7
        struct LobbyDataUpdate {
            uint64_t lobby;
            uint64_t member;
            uint8_t  success;
            uint8_t  _pad[7];
        };
        LobbyDataUpdate cb{};
        cb.lobby   = sid;
        cb.member  = sid;
        cb.success = have ? 1 : 0;
        push_callback(state().user.load(), /*kLobbyDataUpdate*/ 505,
                      &cb, sizeof(cb));
        return true;
    }
    // 29 — SetLobbyGameServer(lobbySid, ip, port, gameServerSid)
    virtual void SetLobbyGameServer(uint64_t sid, uint32_t ip, uint16_t port, uint64_t gs) {
        if (sid == 0) return;
        auto guard = std::lock_guard{state_mutex()};
        auto& L = pushed().active_lobbies[sid];
        L.game_server_ip   = ip;
        L.game_server_port = port;
        L.game_server_sid  = gs;
    }
    // 30 — GetLobbyGameServer(lobbySid, *ip, *port, *gameServerSid)
    virtual bool GetLobbyGameServer(uint64_t sid, uint32_t* ip,
                                     uint16_t* port, uint64_t* sid_out) {
        if (ip) *ip = 0; if (port) *port = 0; if (sid_out) *sid_out = 0;
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return false;
        if (it->second.game_server_sid == 0 && it->second.game_server_ip == 0) return false;
        if (ip)      *ip      = it->second.game_server_ip;
        if (port)    *port    = it->second.game_server_port;
        if (sid_out) *sid_out = it->second.game_server_sid;
        return true;
    }
    // 31 — SetLobbyMemberLimit(lobbySid, max)
    virtual bool SetLobbyMemberLimit(uint64_t sid, int max_members) {
        if (sid == 0 || max_members <= 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto& L = pushed().active_lobbies[sid];
        L.max_members = max_members;
        return true;
    }
    // 32 — GetLobbyMemberLimit(lobbySid)
    virtual int GetLobbyMemberLimit(uint64_t sid) {
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return 0;
        return it->second.max_members;
    }
    // 33 — SetLobbyType(lobbySid, eLobbyType). Host-only. Reuses the
    //   CMsgClientMMSSetLobbyData (6609) channel — the lobby_type field
    //   in that message is the authoritative way Steam updates a
    //   lobby's visibility class (Private/FriendsOnly/Public/Invisible).
    //   Send the existing metadata blob unchanged + the new type; the
    //   server broadcasts a 6612 LobbyData push with the updated value
    //   that the existing observer mirrors back into active_lobbies.
    virtual bool      SetLobbyType(uint64_t sid, int eLobbyType) {
        if (sid == 0) return false;
        std::string blob;
        int32_t max_members = 0;
        int32_t lobby_flags = 0;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto it = pushed().active_lobbies.find(sid);
            if (it == pushed().active_lobbies.end()) return false;
            auto& L = it->second;
            // Only owner can set type; reject otherwise so we don't
            // ship a doomed CM round-trip.
            if (L.owner_sid != pushed().steam_id.load()) return false;
            L.lobby_type = eLobbyType;
            max_members  = L.max_members;
            lobby_flags  = L.lobby_flags;
            for (const auto& kv : L.data) {
                blob.append(kv.first); blob.push_back('\0');
                blob.append(kv.second); blob.push_back('\0');
            }
            blob.push_back('\0');
        }
        const uint64_t h = alloc_api_call_handle();
        wn_cm_lobby_set_data(h, pushed().app_id.load(), sid, /*member=*/0,
                             reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size(),
                             max_members, eLobbyType, lobby_flags,
                             [](uint64_t, int32_t) {});
        return true;
    }
    // 34 — SetLobbyJoinable(lobbySid, joinable). Host-only. Forest
    //   calls SetLobbyJoinable(false) after the game starts to stop
    //   new joiners; without this the lobby stays open and late
    //   joiners spawn into running matches. Steam tracks joinability
    //   in lobby_flags (bit 0). Reuse the same SetLobbyData channel.
    virtual bool      SetLobbyJoinable(uint64_t sid, bool joinable) {
        if (sid == 0) return false;
        std::string blob;
        int32_t max_members = 0;
        int32_t lobby_type  = 0;
        int32_t new_flags   = 0;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto it = pushed().active_lobbies.find(sid);
            if (it == pushed().active_lobbies.end()) return false;
            auto& L = it->second;
            if (L.owner_sid != pushed().steam_id.load()) return false;
            L.joinable = joinable;
            new_flags = joinable
                        ? (L.lobby_flags & ~0x1)   // clear "non-joinable" bit
                        : (L.lobby_flags |  0x1);  // set it
            L.lobby_flags = new_flags;
            max_members   = L.max_members;
            lobby_type    = L.lobby_type;
            for (const auto& kv : L.data) {
                blob.append(kv.first); blob.push_back('\0');
                blob.append(kv.second); blob.push_back('\0');
            }
            blob.push_back('\0');
        }
        const uint64_t h = alloc_api_call_handle();
        wn_cm_lobby_set_data(h, pushed().app_id.load(), sid, /*member=*/0,
                             reinterpret_cast<const uint8_t*>(blob.data()),
                             blob.size(),
                             max_members, lobby_type, new_flags,
                             [](uint64_t, int32_t) {});
        return true;
    }
    // 35 — GetLobbyOwner(lobbySid) → CSteamID. Read from pushed cache;
    //   0 = unknown (caller can RequestLobbyData first).
    virtual uint64_t GetLobbyOwner(uint64_t sid) {
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_lobbies.find(sid);
        if (it == pushed().active_lobbies.end()) return 0;
        return it->second.owner_sid;
    }
    // 36 — SetLobbyOwner(lobbySid, newOwnerSid). Transfer ownership of
    //   a lobby to another member. Host-only per SDK contract. Goes out
    //   as CMsgClientMMSSetLobbyOwner (6615); Steam pushes a 6612
    //   LobbyData with the updated owner_sid that the existing observer
    //   mirrors back into active_lobbies. Useful for host-migration on
    //   leave + delegated-control flows.
    virtual bool      SetLobbyOwner(uint64_t sid, uint64_t new_owner) {
        if (sid == 0 || new_owner == 0) return false;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto it = pushed().active_lobbies.find(sid);
            if (it == pushed().active_lobbies.end()) return false;
            if (it->second.owner_sid != pushed().steam_id.load()) return false;
            // Local prediction — Steam's LobbyData push will overwrite
            // with authoritative value, but predicting here lets the
            // caller's GetLobbyOwner read the new value immediately.
            it->second.owner_sid = new_owner;
        }
        const uint64_t h = alloc_api_call_handle();
        return wn_cm_lobby_set_owner(h, pushed().app_id.load(), sid,
                                      new_owner,
                                      [](uint64_t, int32_t) {});
    }
    // 37 — SetLinkedLobby(lobbySid, lobbyDest)
    virtual bool      SetLinkedLobby(uint64_t, uint64_t)              { return false; }
};

// ---------------------------------------------------------------------------
// ISteamNetworking (version "SteamNetworking005"). Legacy P2P API
// superseded by ISteamNetworkingSockets, but older games (and some
// new ones — Garry's Mod, anything from the Source-engine lineage)
// still probe it on init. Slot map from public/steam/isteamnetworking.h.
//
// All slots return "no packet / no socket / no peer" defaults. Real
// implementation would tunnel over Steam's relay network; out of scope
// today.
class ISteamNetworkingStub {
public:
    // 0 — SendP2PPacket(sid, data, n, eP2PSendType, nChannel) → bool
    //   Session-state tracking + outbound queue. The wire transport
    //   is not yet attached (real Steam P2P uses Steam Datagram Relay
    //   or direct UDP — both are larger workstreams). For now we
    //   record the session as active so callbacks fire correctly and
    //   GetP2PSessionState reports sensible values; the bytes get
    //   counted into bytes_queued_for_send so any game-side congestion
    //   probe sees nonzero pending traffic rather than always-clear.
    virtual bool SendP2PPacket(uint64_t sid, const void* /*data*/,
                                uint32_t n, int /*eP2PSendType*/,
                                int /*nChannel*/) {
        if (sid == 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto& s = pushed().active_p2p_sessions[sid];
        if (!s.connection_active && !s.connecting) {
            s.connecting           = true;
            s.using_relay          = pushed().p2p_relay_allowed.load();
        }
        s.bytes_queued_for_send += n;
        return true;
    }
    // 1 — IsP2PPacketAvailable(*pcubMsgSize, nChannel). Peeks the
    //   per-channel inbound queue and reports the front packet's size.
    virtual bool IsP2PPacketAvailable(uint32_t* pcub, int nChannel) {
        auto guard = std::lock_guard{state_mutex()};
        auto& q = pushed().p2p_inbound_queue[nChannel];
        if (q.empty()) { if (pcub) *pcub = 0; return false; }
        if (pcub) *pcub = static_cast<uint32_t>(q.front().body.size());
        return true;
    }
    // 2 — ReadP2PPacket(dest, cubDest, *pcubMsgSize, *pSteamIDRemote, nChannel)
    //   Pop one packet from the per-channel queue, copy bytes into
    //   caller's dest buffer, fill remote-sid + actual byte count.
    virtual bool ReadP2PPacket(void* dest, uint32_t cubDest,
                                uint32_t* pcub, uint64_t* sidOut, int nChannel) {
        if (pcub) *pcub = 0;
        if (sidOut) *sidOut = 0;
        auto guard = std::lock_guard{state_mutex()};
        auto& q = pushed().p2p_inbound_queue[nChannel];
        if (q.empty()) return false;
        auto& pkt = q.front();
        if (sidOut) *sidOut = pkt.sender_sid;
        const auto copy = static_cast<uint32_t>(
            pkt.body.size() < cubDest ? pkt.body.size() : cubDest);
        if (dest && copy > 0) std::memcpy(dest, pkt.body.data(), copy);
        if (pcub) *pcub = copy;
        q.pop_front();
        return true;
    }
    // 3 — AcceptP2PSessionWithUser(sid). Mark the session as active —
    //   the game accepts incoming traffic from this peer (and the
    //   SDK contract is that subsequent ReadP2PPacket from this sid
    //   succeeds).
    virtual bool AcceptP2PSessionWithUser(uint64_t sid) {
        if (sid == 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto& s = pushed().active_p2p_sessions[sid];
        s.connection_active = true;
        s.connecting        = false;
        s.last_session_error = 0;
        return true;
    }
    // 4 — CloseP2PSessionWithUser(sid). Drop session + any queued
    //   incoming packets from this peer across all channels.
    virtual bool CloseP2PSessionWithUser(uint64_t sid) {
        if (sid == 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        if (!pushed().active_p2p_sessions.erase(sid)) return false;
        // Strip any queued packets from this peer (loop over channels).
        for (auto& kv : pushed().p2p_inbound_queue) {
            auto& q = kv.second;
            q.erase(std::remove_if(q.begin(), q.end(),
                [sid](const PushedState::P2PInboundPacket& p) {
                    return p.sender_sid == sid;
                }),
                q.end());
        }
        return true;
    }
    // 5 — CloseP2PChannelWithUser(sid, nChannel). Drop only the
    //   per-channel queue entries from this peer; session stays open.
    virtual bool CloseP2PChannelWithUser(uint64_t sid, int nChannel) {
        if (sid == 0) return false;
        auto guard = std::lock_guard{state_mutex()};
        auto& q = pushed().p2p_inbound_queue[nChannel];
        const auto before = q.size();
        q.erase(std::remove_if(q.begin(), q.end(),
            [sid](const PushedState::P2PInboundPacket& p) {
                return p.sender_sid == sid;
            }),
            q.end());
        return q.size() != before;
    }
    // 6 — GetP2PSessionState(sid, *pConnectionState). Fill the SDK's
    //   P2PSessionState_t (16B): m_bConnectionActive, m_bConnecting,
    //   m_eP2PSessionError, m_bUsingRelay, m_nBytesQueuedForSend,
    //   m_nPacketsQueuedForSend, m_nRemoteIP, m_nRemotePort.
    virtual bool GetP2PSessionState(uint64_t sid, void* pState) {
        if (!pState) return false;
        struct P2PSessionStateWire {
            uint8_t  m_bConnectionActive;
            uint8_t  m_bConnecting;
            uint8_t  m_eP2PSessionError;
            uint8_t  m_bUsingRelay;
            int32_t  m_nBytesQueuedForSend;
            int32_t  m_nPacketsQueuedForSend;
            uint32_t m_nRemoteIP;
            uint16_t m_nRemotePort;
            uint16_t _pad;
        };
        auto* out = reinterpret_cast<P2PSessionStateWire*>(pState);
        std::memset(out, 0, sizeof(P2PSessionStateWire));
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().active_p2p_sessions.find(sid);
        if (it == pushed().active_p2p_sessions.end()) return false;
        const auto& s = it->second;
        out->m_bConnectionActive     = s.connection_active ? 1 : 0;
        out->m_bConnecting           = s.connecting ? 1 : 0;
        out->m_eP2PSessionError      = static_cast<uint8_t>(s.last_session_error);
        out->m_bUsingRelay           = s.using_relay ? 1 : 0;
        out->m_nBytesQueuedForSend   = static_cast<int32_t>(s.bytes_queued_for_send);
        // Packet count is approximated as 1 per send call (we don't
        // track per-packet boundaries in the outbound queue yet).
        out->m_nPacketsQueuedForSend = s.bytes_queued_for_send > 0 ? 1 : 0;
        out->m_nRemoteIP             = s.remote_ip;
        out->m_nRemotePort           = s.remote_port;
        return true;
    }
    // 7 — AllowP2PPacketRelay(bAllow). Persist the flag — Steam's SDK
    //   contract treats this as advisory ("prefer direct, fall back to
    //   relay"). Until our transport supports direct, relay-on is the
    //   only path so this is currently informational only.
    virtual bool AllowP2PPacketRelay(bool bAllow) {
        pushed().p2p_relay_allowed.store(bAllow);
        return true;
    }

    // 8-21 — old listen/connect socket API (deprecated long before
    // ISteamNetworkingSockets shipped). Game-side code that probes
    // these slots either: (a) handles failure gracefully, or (b) was
    // written against pre-2016 Steam and is unlikely to run today.
    // -1 is the documented "invalid handle" sentinel.
    // 8 — CreateListenSocket
    virtual int       CreateListenSocket(int, uint32_t, uint16_t, bool) { return -1; }
    // 9 — CreateP2PConnectionSocket
    virtual int       CreateP2PConnectionSocket(uint64_t, int, int, bool) { return -1; }
    // 10 — CreateConnectionSocket
    virtual int       CreateConnectionSocket(uint32_t, uint16_t, int)  { return -1; }
    // 11 — DestroySocket
    virtual bool      DestroySocket(int, bool)                       { return false; }
    // 12 — DestroyListenSocket
    virtual bool      DestroyListenSocket(int, bool)                 { return false; }
    // 13 — SendDataOnSocket
    virtual bool      SendDataOnSocket(int, void*, uint32_t, bool)   { return false; }
    // 14 — IsDataAvailableOnSocket
    virtual bool      IsDataAvailableOnSocket(int, uint32_t* pcb) {
        if (pcb) *pcb = 0;
        return false;
    }
    // 15 — RetrieveDataFromSocket
    virtual bool      RetrieveDataFromSocket(int, void*, uint32_t, uint32_t* pcb) {
        if (pcb) *pcb = 0;
        return false;
    }
    // 16 — IsDataAvailable
    virtual bool      IsDataAvailable(int, uint32_t* pcb, int*) {
        if (pcb) *pcb = 0;
        return false;
    }
    // 17 — RetrieveData
    virtual bool      RetrieveData(int, void*, uint32_t, uint32_t* pcb, int*) {
        if (pcb) *pcb = 0;
        return false;
    }
    // 18 — GetSocketInfo(socket, *pSteamIDRemote, *peSocketStatus,
    //                    *punIPRemote, *punPortRemote, *phListenSocket)
    virtual bool      GetSocketInfo(int, uint64_t* sid, int* status,
                                     uint32_t* ip, uint16_t* port, int* lsock) {
        if (sid)    *sid    = 0;
        if (status) *status = 0;
        if (ip)     *ip     = 0;
        if (port)   *port   = 0;
        if (lsock)  *lsock  = -1;
        return false;
    }
    // 19 — GetListenSocketInfo(socket, *punIP, *punPort)
    virtual bool      GetListenSocketInfo(int, uint32_t* ip, uint16_t* port) {
        if (ip)   *ip   = 0;
        if (port) *port = 0;
        return false;
    }
    // 20 — GetSocketConnectionType(socket) → ESNetSocketConnectionType (0=NotConnected)
    virtual int       GetSocketConnectionType(int)                   { return 0; }
    // 21 — GetMaxPacketSize(socket)
    virtual int       GetMaxPacketSize(int)                          { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamUGC (version "STEAMUGC_INTERFACE_VERSION018"). Workshop surface.
// Slot map from public/steam/isteamugc.h. Mod-supporting games
// (Garry's Mod, TF2, CS:GO/CS2, every Source-engine title, Skyrim,
// L4D2, all Workshop-enabled games) probe this on init. Previously
// nullptr → SIGSEGV.
//
// All slots return safe "no workshop content" defaults so the game's
// "list my subscribed items" + "did mod X download" probes resolve
// to "nothing here yet" rather than crashing. Real implementation
// would back onto wn-steam-client's CPublishedFile service-method
// path; deferred.
class ISteamUGCStub {
public:
    // Query construction (0-3) — all return invalid handle sentinel.
    virtual uint64_t  CreateQueryUserUGCRequest(uint32_t, int, int, int, uint32_t, uint32_t, uint32_t) { return 0; }
    virtual uint64_t  CreateQueryAllUGCRequest_Page(int, int, uint32_t, uint32_t, uint32_t) { return 0; }
    virtual uint64_t  CreateQueryAllUGCRequest_Cursor(int, int, uint32_t, uint32_t, const char*) { return 0; }
    virtual uint64_t  CreateQueryUGCDetailsRequest(const uint64_t*, uint32_t) { return 0; }
    // 4 — SendQueryUGCRequest(handle). Async — fires
    //   SteamUGCQueryCompleted_t (k_iCallback=3401). Without this,
    //   Workshop-using games (Cities Skylines, RimWorld, Skyrim, many
    //   indies) call it on boot to enumerate subscribed mods and HANG
    //   forever waiting for the callback. Post an "empty result" so
    //   the game proceeds — no items returned matches the truth of
    //   "no Workshop content fetched yet on this device".
    virtual uint64_t SendQueryUGCRequest(uint64_t handle) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::SteamUGCQueryCompleted cb{};
        cb.m_handle                 = handle;
        cb.m_eResult                = 1; // k_EResultOK — empty success
        cb.m_unNumResultsReturned   = 0;
        cb.m_unTotalMatchingResults = 0;
        cb.m_bCachedData            = 0;
        cb.m_rgchNextCursor[0]      = '\0';
        push_call_result(h, lsc_cb::kSteamUGCQueryCompleted,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // Query result accessors (5-18)
    virtual bool      GetQueryUGCResult(uint64_t, uint32_t, void*)   { return false; }
    virtual uint32_t  GetQueryUGCNumTags(uint64_t, uint32_t)         { return 0; }
    virtual bool      GetQueryUGCTag(uint64_t, uint32_t, uint32_t, char* v, uint32_t vn) { if (v && vn) v[0] = '\0'; return false; }
    virtual bool      GetQueryUGCTagDisplayName(uint64_t, uint32_t, uint32_t, char* v, uint32_t vn) { if (v && vn) v[0] = '\0'; return false; }
    virtual bool      GetQueryUGCPreviewURL(uint64_t, uint32_t, char* v, uint32_t vn) { if (v && vn) v[0] = '\0'; return false; }
    virtual bool      GetQueryUGCMetadata(uint64_t, uint32_t, char* v, uint32_t vn) { if (v && vn) v[0] = '\0'; return false; }
    virtual bool      GetQueryUGCChildren(uint64_t, uint32_t, uint64_t*, uint32_t) { return false; }
    virtual bool      GetQueryUGCStatistic(uint64_t, uint32_t, int, uint64_t* out) { if (out) *out = 0; return false; }
    virtual uint32_t  GetQueryUGCNumAdditionalPreviews(uint64_t, uint32_t) { return 0; }
    virtual bool      GetQueryUGCAdditionalPreview(uint64_t, uint32_t, uint32_t, char* url, uint32_t uns, char* orig, uint32_t os, int*) {
        if (url && uns) url[0] = '\0';
        if (orig && os) orig[0] = '\0';
        return false;
    }
    virtual uint32_t  GetQueryUGCNumKeyValueTags(uint64_t, uint32_t) { return 0; }
    virtual bool      GetQueryUGCKeyValueTagByIndex(uint64_t, uint32_t, uint32_t, char* k, uint32_t kn, char* v, uint32_t vn) {
        if (k && kn) k[0] = '\0';
        if (v && vn) v[0] = '\0';
        return false;
    }
    virtual bool      GetQueryUGCKeyValueTagByName(uint64_t, uint32_t, const char*, char* v, uint32_t vn) {
        if (v && vn) v[0] = '\0';
        return false;
    }
    virtual uint32_t  GetQueryUGCContentDescriptors(uint64_t, uint32_t, int*, uint32_t) { return 0; }
    // 19 — ReleaseQueryUGCRequest
    virtual bool      ReleaseQueryUGCRequest(uint64_t)               { return false; }
    // Query filter setters (20-39) — all no-op false (caller will see
    // their later SendQueryUGCRequest return 0 anyway).
    virtual bool      AddRequiredTag(uint64_t, const char*)          { return false; }
    virtual bool      AddRequiredTagGroup(uint64_t, const void*)     { return false; }
    virtual bool      AddExcludedTag(uint64_t, const char*)          { return false; }
    virtual bool      SetReturnOnlyIDs(uint64_t, bool)               { return false; }
    virtual bool      SetReturnKeyValueTags(uint64_t, bool)          { return false; }
    virtual bool      SetReturnLongDescription(uint64_t, bool)       { return false; }
    virtual bool      SetReturnMetadata(uint64_t, bool)              { return false; }
    virtual bool      SetReturnChildren(uint64_t, bool)              { return false; }
    virtual bool      SetReturnAdditionalPreviews(uint64_t, bool)    { return false; }
    virtual bool      SetReturnTotalOnly(uint64_t, bool)             { return false; }
    virtual bool      SetReturnPlaytimeStats(uint64_t, uint32_t)     { return false; }
    virtual bool      SetLanguage(uint64_t, const char*)             { return false; }
    virtual bool      SetAllowCachedResponse(uint64_t, uint32_t)     { return false; }
    virtual bool      SetCloudFileNameFilter(uint64_t, const char*)  { return false; }
    virtual bool      SetMatchAnyTag(uint64_t, bool)                 { return false; }
    virtual bool      SetSearchText(uint64_t, const char*)           { return false; }
    virtual bool      SetRankedByTrendDays(uint64_t, uint32_t)       { return false; }
    virtual bool      SetTimeCreatedDateRange(uint64_t, uint32_t, uint32_t) { return false; }
    virtual bool      SetTimeUpdatedDateRange(uint64_t, uint32_t, uint32_t) { return false; }
    virtual bool      AddRequiredKeyValueTag(uint64_t, const char*, const char*) { return false; }
    // 40 — RequestUGCDetails (deprecated) → SteamAPICall_t. Some
    //   legacy games still call this. Post a minimal EResult=Fail so
    //   the CCallResult dispatch unblocks.
    virtual uint64_t RequestUGCDetails(uint64_t /*publishedFileId*/, uint32_t /*maxAgeSeconds*/) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::SteamUGCRequestUGCDetailsResultMinimal cb{};
        cb.m_eResult = 2; // k_EResultFail
        push_call_result(h, lsc_cb::kSteamUGCRequestUGCDetails,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // Item creation (41) → SteamAPICall_t
    virtual uint64_t  CreateItem(uint32_t, int)                      { return 0; }
    // Item update workflow (42-62)
    virtual uint64_t  StartItemUpdate(uint32_t, uint64_t)            { return 0; }
    virtual bool      SetItemTitle(uint64_t, const char*)            { return false; }
    virtual bool      SetItemDescription(uint64_t, const char*)      { return false; }
    virtual bool      SetItemUpdateLanguage(uint64_t, const char*)   { return false; }
    virtual bool      SetItemMetadata(uint64_t, const char*)         { return false; }
    virtual bool      SetItemVisibility(uint64_t, int)               { return false; }
    virtual bool      SetItemTags(uint64_t, const void*, bool)       { return false; }
    virtual bool      SetItemContent(uint64_t, const char*)          { return false; }
    virtual bool      SetItemPreview(uint64_t, const char*)          { return false; }
    virtual bool      SetAllowLegacyUpload(uint64_t, bool)           { return false; }
    virtual bool      RemoveAllItemKeyValueTags(uint64_t)            { return false; }
    virtual bool      RemoveItemKeyValueTags(uint64_t, const char*)  { return false; }
    virtual bool      AddItemKeyValueTag(uint64_t, const char*, const char*) { return false; }
    virtual bool      AddItemPreviewFile(uint64_t, const char*, int) { return false; }
    virtual bool      AddItemPreviewVideo(uint64_t, const char*)     { return false; }
    virtual bool      UpdateItemPreviewFile(uint64_t, uint32_t, const char*) { return false; }
    virtual bool      UpdateItemPreviewVideo(uint64_t, uint32_t, const char*) { return false; }
    virtual bool      RemoveItemPreview(uint64_t, uint32_t)          { return false; }
    virtual bool      AddContentDescriptor(uint64_t, int)            { return false; }
    virtual bool      RemoveContentDescriptor(uint64_t, int)         { return false; }
    virtual uint64_t  SubmitItemUpdate(uint64_t, const char*)        { return 0; }
    // 63 — GetItemUpdateProgress(handle, *bp, *bt) → EItemUpdateStatus (0=Invalid)
    virtual int       GetItemUpdateProgress(uint64_t, uint64_t* bp, uint64_t* bt) {
        if (bp) *bp = 0;
        if (bt) *bt = 0;
        return 0;
    }
    // Voting / favorites (64-67) → SteamAPICall_t
    virtual uint64_t  SetUserItemVote(uint64_t, bool)                { return 0; }
    virtual uint64_t  GetUserItemVote(uint64_t)                      { return 0; }
    virtual uint64_t  AddItemToFavorites(uint32_t, uint64_t)         { return 0; }
    virtual uint64_t  RemoveItemFromFavorites(uint32_t, uint64_t)    { return 0; }
    // Subscription (68-71). Post the standard RemoteStorageSubscribe
    //   PublishedFileResult_t / Unsubscribe variant so callbacks fire.
    //   We don't actually fetch / unfetch — EResult=Fail signals the
    //   game that the operation didn't complete server-side, but the
    //   callback IS delivered so the game's CCallResult unblocks.
    virtual uint64_t SubscribeItem(uint64_t publishedFileId) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::RemoteStorageSubscribePublishedFileResult cb{};
        cb.m_eResult          = 2; // k_EResultFail (no UGC backend)
        cb.m_nPublishedFileId = publishedFileId;
        push_call_result(h, lsc_cb::kRemoteStorageSubscribePublishedFile,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    virtual uint64_t UnsubscribeItem(uint64_t publishedFileId) {
        uint64_t h = alloc_api_call_handle();
        lsc_cb::RemoteStorageUnsubscribePublishedFileResult cb{};
        cb.m_eResult          = 2; // k_EResultFail
        cb.m_nPublishedFileId = publishedFileId;
        push_call_result(h, lsc_cb::kRemoteStorageUnsubscribePublishedFile,
                         &cb, sizeof(cb), /*io_failure=*/false);
        return h;
    }
    // 70 — GetNumSubscribedItems → count of subscribed Workshop items
    //   for the bound app. Backed by pushed().subscribed_workshop_items
    //   which SteamService populates at game launch from the existing
    //   WorkshopModsGenerator staging dir (anything fully downloaded
    //   counts as subscribed-and-installed).
    virtual uint32_t  GetNumSubscribedItems() {
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().subscribed_workshop_items.find(app);
        if (it == pushed().subscribed_workshop_items.end()) return 0;
        return static_cast<uint32_t>(it->second.size());
    }
    // 71 — GetSubscribedItems(*pIds, cMaxItems) → fill pIds with the
    //   first cMaxItems PublishedFileId_t entries. Order is map
    //   iteration order (unordered_map); games that need stable order
    //   sort on their side.
    virtual uint32_t  GetSubscribedItems(uint64_t* pIds, uint32_t cMax) {
        if (!pIds || cMax == 0) return 0;
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().subscribed_workshop_items.find(app);
        if (it == pushed().subscribed_workshop_items.end()) return 0;
        uint32_t n = 0;
        for (const auto& kv : it->second) {
            if (n >= cMax) break;
            pIds[n++] = kv.first;
        }
        return n;
    }
    // 72 — GetItemState(workshop_id) → bitmask of k_EItemState*.
    //   Installed-and-subscribed = Installed (0x4) | Subscribed (0x1).
    //   Unknown id → 0 (k_EItemStateNone).
    virtual uint32_t  GetItemState(uint64_t publishedFileId) {
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().subscribed_workshop_items.find(app);
        if (it == pushed().subscribed_workshop_items.end()) return 0;
        auto jt = it->second.find(publishedFileId);
        if (jt == it->second.end() || !jt->second.installed) return 0;
        return /*k_EItemStateSubscribed*/ 1u | /*k_EItemStateInstalled*/ 4u;
    }
    // 73 — GetItemInstallInfo(workshop_id, *bytes, *folder, fn, *timestamp)
    //   Returns true and fills out params for an installed item;
    //   false when the id is unknown / not installed.
    virtual bool      GetItemInstallInfo(uint64_t publishedFileId,
                                          uint64_t* bytes,
                                          char* folder, uint32_t fn,
                                          uint32_t* timestamp) {
        if (bytes) *bytes = 0;
        if (folder && fn) folder[0] = '\0';
        if (timestamp) *timestamp = 0;
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().subscribed_workshop_items.find(app);
        if (it == pushed().subscribed_workshop_items.end()) return false;
        auto jt = it->second.find(publishedFileId);
        if (jt == it->second.end() || !jt->second.installed) return false;
        if (bytes)     *bytes     = jt->second.size_bytes;
        if (timestamp) *timestamp = jt->second.timestamp;
        if (folder && fn) {
            const auto& src = jt->second.install_dir;
            const auto copy = (src.size() < fn ? src.size() : fn - 1);
            std::memcpy(folder, src.data(), copy);
            folder[copy] = '\0';
        }
        return true;
    }
    // 74 — GetItemDownloadInfo(workshop_id, *bd, *bt). For installed
    //   items we report bd=bt=total so progress bars resolve to 100%.
    //   Pending / not-installed → false.
    virtual bool      GetItemDownloadInfo(uint64_t publishedFileId, uint64_t* bd, uint64_t* bt) {
        if (bd) *bd = 0;
        if (bt) *bt = 0;
        const auto app = pushed().app_id.load();
        auto guard = std::lock_guard{state_mutex()};
        auto it = pushed().subscribed_workshop_items.find(app);
        if (it == pushed().subscribed_workshop_items.end()) return false;
        auto jt = it->second.find(publishedFileId);
        if (jt == it->second.end() || !jt->second.installed) return false;
        if (bd) *bd = jt->second.size_bytes;
        if (bt) *bt = jt->second.size_bytes;
        return true;
    }
    // 75 — DownloadItem(workshop_id, bHighPriority). For already-
    //   installed items we synthesize an immediate ItemInstalled_t /
    //   DownloadItemResult_t pair so games waiting on a callback
    //   unblock. For unknown ids we return false — game then knows it
    //   needs to subscribe first.
    virtual bool      DownloadItem(uint64_t publishedFileId, bool /*bHighPriority*/) {
        const auto app = pushed().app_id.load();
        bool installed = false;
        {
            auto guard = std::lock_guard{state_mutex()};
            auto it = pushed().subscribed_workshop_items.find(app);
            if (it != pushed().subscribed_workshop_items.end()) {
                auto jt = it->second.find(publishedFileId);
                if (jt != it->second.end() && jt->second.installed) installed = true;
            }
        }
        if (!installed) return false;
        // DownloadItemResult_t = k_iSteamUGCCallbacks + 6 = 3406.
        // ItemInstalled_t      = k_iSteamUGCCallbacks + 14 = 3414.
        // Both are tiny POD structs the game polls in its callback
        // loop. We emit synthetic success so the game's own download-
        // queue thinks the item just landed and runs its install hook.
        struct DownloadItemResult { uint32_t app_id; uint64_t pfid; int32_t eResult; };
        struct ItemInstalled      { uint32_t app_id; uint64_t pfid; };
        DownloadItemResult dr{ app, publishedFileId, /*k_EResultOK*/ 1 };
        ItemInstalled      ii{ app, publishedFileId };
        push_callback(state().user.load(), 3406, &dr, sizeof(dr));
        push_callback(state().user.load(), 3414, &ii, sizeof(ii));
        return true;
    }
    // Workshop game-server hook (76)
    virtual bool      BInitWorkshopForGameServer(uint32_t, const char*) { return false; }
    // 77 — SuspendDownloads
    virtual void      SuspendDownloads(bool)                         {}
    // Playtime tracking (78-80) → SteamAPICall_t
    virtual uint64_t  StartPlaytimeTracking(uint64_t*, uint32_t)     { return 0; }
    virtual uint64_t  StopPlaytimeTracking(uint64_t*, uint32_t)      { return 0; }
    virtual uint64_t  StopPlaytimeTrackingForAllItems()              { return 0; }
    // Dependencies (81-85) → SteamAPICall_t
    virtual uint64_t  AddDependency(uint64_t, uint64_t)              { return 0; }
    virtual uint64_t  RemoveDependency(uint64_t, uint64_t)           { return 0; }
    virtual uint64_t  AddAppDependency(uint64_t, uint32_t)           { return 0; }
    virtual uint64_t  RemoveAppDependency(uint64_t, uint32_t)        { return 0; }
    virtual uint64_t  GetAppDependencies(uint64_t)                   { return 0; }
    // 86 — DeleteItem
    virtual uint64_t  DeleteItem(uint64_t)                           { return 0; }
    // EULA (87-88)
    virtual bool      ShowWorkshopEULA()                             { return false; }
    virtual uint64_t  GetWorkshopEULAStatus()                        { return 0; }
    // 89 — GetUserContentDescriptorPreferences
    virtual uint32_t  GetUserContentDescriptorPreferences(int*, uint32_t) { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamGameServer (version "SteamGameServer015"). Dedicated-server
// surface — used by games that host their own servers (Source-engine
// dedicated, ARMA, Team Fortress 2 srcds, anything launched with
// SteamGameServer_Init). Slot map from public/steam/isteamgameserver.h.
//
// Stubs report "not logged on / no server". Real server hosting on
// this platform is out of scope (we're a client runtime); games that
// probe ISteamGameServer get safe defaults and gracefully degrade.
class ISteamGameServerStub {
public:
    // 0-3 — config setters; all no-op
    virtual void      SetProduct(const char*)                        {}
    virtual void      SetGameDescription(const char*)                {}
    virtual void      SetModDir(const char*)                         {}
    virtual void      SetDedicatedServer(bool)                       {}
    // 4-6 — auth
    virtual void      LogOn(const char*)                             {}
    virtual void      LogOnAnonymous()                               {}
    virtual void      LogOff()                                       {}
    // 7-10 — state
    virtual bool      BLoggedOn()                                    { return false; }
    virtual bool      BSecure()                                      { return false; }
    virtual uint64_t  GetSteamID()                                   { return 0; }
    virtual bool      WasRestartRequested()                          { return false; }
    // 11-22 — config setters
    virtual void      SetMaxPlayerCount(int)                         {}
    virtual void      SetBotPlayerCount(int)                         {}
    virtual void      SetServerName(const char*)                     {}
    virtual void      SetMapName(const char*)                        {}
    virtual void      SetPasswordProtected(bool)                     {}
    virtual void      SetSpectatorPort(uint16_t)                     {}
    virtual void      SetSpectatorServerName(const char*)            {}
    virtual void      ClearAllKeyValues()                            {}
    virtual void      SetKeyValue(const char*, const char*)          {}
    virtual void      SetGameTags(const char*)                       {}
    virtual void      SetGameData(const char*)                       {}
    virtual void      SetRegion(const char*)                         {}
    // 23 — SetAdvertiseServerActive (deprecated, ignored)
    virtual void      SetAdvertiseServerActive(bool)                 {}
    // 24-27 — auth-session surface (mirrors ISteamUser slots 13/15/16/17)
    virtual uint64_t  GetAuthSessionTicket(void*, int, uint32_t* pcb, const void*) {
        if (pcb) *pcb = 0;
        return 0;  // k_HAuthTicketInvalid
    }
    virtual int       BeginAuthSession(const void*, int, uint64_t)   { return 5; /*ServerNotConnectedToSteam*/ }
    virtual void      EndAuthSession(uint64_t)                       {}
    virtual void      CancelAuthTicket(uint64_t)                     {}
    // 28 — UserHasLicenseForApp
    virtual int       UserHasLicenseForApp(uint64_t, uint32_t)       { return 2; /*NoAuth*/ }
    // 29 — RequestUserGroupStatus
    virtual bool      RequestUserGroupStatus(uint64_t, uint64_t)     { return false; }
    // 30 — GetGameplayStats (deprecated)
    virtual void      GetGameplayStats()                             {}
    // 31 — GetServerReputation
    virtual uint64_t  GetServerReputation()                          { return 0; }
    // 32 — GetPublicIP → SteamIPAddress_t (16B)
    virtual void      GetPublicIP(void* out) {
        if (out) std::memset(out, 0, 16);
    }
    // 33 — HandleIncomingPacket
    virtual bool      HandleIncomingPacket(const void*, int, uint32_t, uint16_t) { return false; }
    // 34 — GetNextOutgoingPacket
    virtual int       GetNextOutgoingPacket(void*, int, uint32_t*, uint16_t*) { return 0; }
    // 35 — AssociateWithClan
    virtual uint64_t  AssociateWithClan(uint64_t)                    { return 0; }
    // 36 — ComputeNewPlayerCompatibility
    virtual uint64_t  ComputeNewPlayerCompatibility(uint64_t)        { return 0; }
    // 37-39 — deprecated user-connect (legacy)
    virtual bool      SendUserConnectAndAuthenticate_DEPRECATED(uint32_t, const void*, uint32_t, uint64_t*) { return false; }
    virtual uint64_t  CreateUnauthenticatedUserConnection()          { return 0; }
    virtual void      SendUserDisconnect_DEPRECATED(uint64_t)        {}
    // 40 — BUpdateUserData
    virtual bool      BUpdateUserData(uint64_t, const char*, uint32_t) { return false; }
    // 41 — GetAuthTicketForWebApi
    virtual uint64_t  GetAuthTicketForWebApi(const char*)            { return 0; }
};

// ---------------------------------------------------------------------------
// ISteamMusicRemote (version "STEAMMUSICREMOTE_INTERFACE_VERSION001").
// Third-party music-controller surface — apps register themselves as
// the current music source and Steam's overlay forwards play/pause/
// volume controls. Slot map from public/steam/isteammusicremote.h.
//
// Games don't probe this; the prebuilt Steam Client does. Stubbing
// for ABI completeness so any consumer that dlopens our .so and
// dlsyms SteamClient020 + slot 21 doesn't trip on null.
class ISteamMusicRemoteStub {
public:
    // 0-1 — registration
    virtual bool      RegisterSteamMusicRemote(const char*)          { return false; }
    virtual bool      DeregisterSteamMusicRemote()                   { return false; }
    // 2-3 — state checks
    virtual bool      BIsCurrentMusicRemote()                        { return false; }
    virtual bool      BActivationSuccess(bool)                       { return false; }
    // 4-5 — display
    virtual bool      SetDisplayName(const char*)                    { return false; }
    virtual bool      SetPNGIcon_64x64(void*, uint32_t)              { return false; }
    // 6-11 — feature enables
    virtual bool      EnablePlayPrevious(bool)                       { return false; }
    virtual bool      EnablePlayNext(bool)                           { return false; }
    virtual bool      EnableShuffled(bool)                           { return false; }
    virtual bool      EnableLooped(bool)                             { return false; }
    virtual bool      EnableQueue(bool)                              { return false; }
    virtual bool      EnablePlaylists(bool)                          { return false; }
    // 12-15 — playback state updates
    virtual bool      UpdatePlaybackStatus(int)                      { return false; }
    virtual bool      UpdateShuffled(bool)                           { return false; }
    virtual bool      UpdateLooped(bool)                             { return false; }
    virtual bool      UpdateVolume(float)                            { return false; }
    // 16-21 — current entry
    virtual bool      CurrentEntryWillChange()                       { return false; }
    virtual bool      CurrentEntryIsAvailable(bool)                  { return false; }
    virtual bool      UpdateCurrentEntryText(const char*)            { return false; }
    virtual bool      UpdateCurrentEntryElapsedSeconds(int)          { return false; }
    virtual bool      UpdateCurrentEntryCoverArt(void*, uint32_t)    { return false; }
    virtual bool      CurrentEntryDidChange()                        { return false; }
    // 22-26 — queue
    virtual bool      QueueWillChange()                              { return false; }
    virtual bool      ResetQueueEntries()                            { return false; }
    virtual bool      SetQueueEntry(int, int, const char*)           { return false; }
    virtual bool      SetCurrentQueueEntry(int)                      { return false; }
    virtual bool      QueueDidChange()                               { return false; }
    // 27-31 — playlists
    virtual bool      PlaylistWillChange()                           { return false; }
    virtual bool      ResetPlaylistEntries()                         { return false; }
    virtual bool      SetPlaylistEntry(int, int, const char*)        { return false; }
    virtual bool      SetCurrentPlaylistEntry(int)                   { return false; }
    virtual bool      PlaylistDidChange()                            { return false; }
};

// ---------------------------------------------------------------------------
// ISteamHTMLSurface (version "STEAMHTMLSURFACE_INTERFACE_VERSION_005").
// In-game web browser — used by the Steam overlay's community panel
// and games with embedded web content (Civ V's pedia browser, etc.).
// Slot map from public/steam/isteamhtmlsurface.h.
//
// CEF-based browser support is out of scope for the .so; we don't
// have a Chromium/CEF embedding here. Stubs return failure across
// the board. Games that probe HTMLSurface gracefully fall back to
// their built-in in-game UI.
class ISteamHTMLSurfaceStub {
public:
    // 0-1 — lifecycle
    virtual bool      Init()                                         { return false; }
    virtual bool      Shutdown()                                     { return false; }
    // 2 — CreateBrowser → SteamAPICall_t (HTML_BrowserReady_t)
    virtual uint64_t  CreateBrowser(const char*, const char*)        { return 0; }
    // 3 — RemoveBrowser
    virtual void      RemoveBrowser(uint32_t)                        {}
    // 4 — LoadURL
    virtual void      LoadURL(uint32_t, const char*, const char*)    {}
    // 5 — SetSize
    virtual void      SetSize(uint32_t, uint32_t, uint32_t)          {}
    // 6 — StopLoad
    virtual void      StopLoad(uint32_t)                             {}
    // 7 — Reload
    virtual void      Reload(uint32_t)                               {}
    // 8 — GoBack
    virtual void      GoBack(uint32_t)                               {}
    // 9 — GoForward
    virtual void      GoForward(uint32_t)                            {}
    // 10 — AddHeader
    virtual void      AddHeader(uint32_t, const char*, const char*)  {}
    // 11 — ExecuteJavascript
    virtual void      ExecuteJavascript(uint32_t, const char*)       {}
    // 12-16 — mouse input
    virtual void      MouseUp(uint32_t, int)                         {}
    virtual void      MouseDown(uint32_t, int)                       {}
    virtual void      MouseDoubleClick(uint32_t, int)                {}
    virtual void      MouseMove(uint32_t, int, int)                  {}
    virtual void      MouseWheel(uint32_t, int32_t)                  {}
    // 17-19 — keyboard input
    virtual void      KeyDown(uint32_t, uint32_t, int)               {}
    virtual void      KeyUp(uint32_t, uint32_t, int)                 {}
    virtual void      KeyChar(uint32_t, uint32_t, int)               {}
    // 20-22 — scroll + focus
    virtual void      SetHorizontalScroll(uint32_t, uint32_t)        {}
    virtual void      SetVerticalScroll(uint32_t, uint32_t)          {}
    virtual void      SetKeyFocus(uint32_t, bool)                    {}
    // 23-25 — view-source / clipboard
    virtual void      ViewSource(uint32_t)                           {}
    virtual void      CopyToClipboard(uint32_t)                      {}
    virtual void      PasteFromClipboard(uint32_t)                   {}
    // 26-28 — find / link
    virtual void      Find(uint32_t, const char*, bool, bool)        {}
    virtual void      StopFind(uint32_t)                             {}
    virtual void      GetLinkAtPosition(uint32_t, int, int)          {}
    // 29-31 — cookie / scaling / background
    virtual void      SetCookie(const char*, const char*, const char*, const char*, uint32_t, bool, bool) {}
    virtual void      SetPageScaleFactor(uint32_t, float, int, int)  {}
    virtual void      SetBackgroundMode(uint32_t, bool)              {}
    // 32 — SetDPIScalingFactor
    virtual void      SetDPIScalingFactor(uint32_t, float)           {}
    // 33 — OpenDeveloperTools
    virtual void      OpenDeveloperTools(uint32_t)                   {}
    // 34 — AllowStartRequest
    virtual void      AllowStartRequest(uint32_t, bool)              {}
    // 35-36 — dialog responses
    virtual void      JSDialogResponse(uint32_t, bool)               {}
    virtual void      FileLoadDialogResponse(uint32_t, const char**) {}
};

// ---------------------------------------------------------------------------
// ISteamInput (version "SteamInput006"). Modern Steam controller /
// gamepad-binding API — supersedes the older ISteamController. Slot
// map from public/steam/isteaminput.h.
//
// Stubs report "no controllers connected" so games gracefully fall
// back to keyboard / mouse. Real Steam-Controller-style binding
// requires the overlay infrastructure (not present on this platform).
class ISteamInputStub {
public:
    // 0-3 — lifecycle
    virtual bool      Init(bool)                                     { return false; }
    virtual bool      Shutdown()                                     { return false; }
    virtual bool      SetInputActionManifestFilePath(const char*)    { return false; }
    virtual void      RunFrame(bool)                                 {}
    // 4-5 — frame-data polling
    virtual bool      BWaitForData(bool, uint32_t)                   { return false; }
    virtual bool      BNewDataAvailable()                            { return false; }
    // 6 — GetConnectedControllers(*handles) → count
    virtual int       GetConnectedControllers(uint64_t*)             { return 0; }
    // 7-8 — callback enables
    virtual void      EnableDeviceCallbacks()                        {}
    virtual void      EnableActionEventCallbacks(void*)              {}
    // 9-15 — action-set
    virtual uint64_t  GetActionSetHandle(const char*)                { return 0; }
    virtual void      ActivateActionSet(uint64_t, uint64_t)          {}
    virtual uint64_t  GetCurrentActionSet(uint64_t)                  { return 0; }
    virtual void      ActivateActionSetLayer(uint64_t, uint64_t)     {}
    virtual void      DeactivateActionSetLayer(uint64_t, uint64_t)   {}
    virtual void      DeactivateAllActionSetLayers(uint64_t)         {}
    virtual int       GetActiveActionSetLayers(uint64_t, uint64_t*)  { return 0; }
    // 16-19 — digital actions
    virtual uint64_t  GetDigitalActionHandle(const char*)            { return 0; }
    virtual void      GetDigitalActionData(uint64_t, uint64_t, void* outData) {
        // InputDigitalActionData_t = uint8 bState + uint8 bActive — 2B
        if (outData) std::memset(outData, 0, 2);
    }
    virtual int       GetDigitalActionOrigins(uint64_t, uint64_t, uint64_t, int*) { return 0; }
    virtual const char* GetStringForDigitalActionName(uint64_t)      { return ""; }
    // 20-22 — analog actions
    virtual uint64_t  GetAnalogActionHandle(const char*)             { return 0; }
    virtual void      GetAnalogActionData(uint64_t, uint64_t, void* outData) {
        // InputAnalogActionData_t = int eMode + float x + float y + bool bActive — 16B
        if (outData) std::memset(outData, 0, 16);
    }
    virtual int       GetAnalogActionOrigins(uint64_t, uint64_t, uint64_t, int*) { return 0; }
    // 23-27 — glyphs + display strings
    virtual const char* GetGlyphPNGForActionOrigin(int, int, uint32_t) { return ""; }
    virtual const char* GetGlyphSVGForActionOrigin(int, uint32_t)    { return ""; }
    virtual const char* GetGlyphForActionOrigin_Legacy(int)          { return ""; }
    virtual const char* GetStringForActionOrigin(int)                { return ""; }
    virtual const char* GetStringForAnalogActionName(uint64_t)       { return ""; }
    // 28 — StopAnalogActionMomentum
    virtual void      StopAnalogActionMomentum(uint64_t, uint64_t)   {}
    // 29 — GetMotionData → InputMotionData_t (36B)
    virtual void      GetMotionData(uint64_t)                        {}
    // 30-35 — haptics + LED
    virtual void      TriggerVibration(uint64_t, uint16_t, uint16_t) {}
    virtual void      TriggerVibrationExtended(uint64_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
    virtual void      TriggerSimpleHapticEvent(uint64_t, int, uint8_t, char, uint8_t, char) {}
    virtual void      SetLEDColor(uint64_t, uint8_t, uint8_t, uint8_t, uint32_t) {}
    virtual void      Legacy_TriggerHapticPulse(uint64_t, int, uint16_t) {}
    virtual void      Legacy_TriggerRepeatedHapticPulse(uint64_t, int, uint16_t, uint16_t, uint16_t, uint32_t) {}
    // 36 — ShowBindingPanel
    virtual bool      ShowBindingPanel(uint64_t)                     { return false; }
    // 37-43 — input-type queries
    virtual int       GetInputTypeForHandle(uint64_t)                { return 0; /*ESteamInputType_Unknown*/ }
    virtual uint64_t  GetControllerForGamepadIndex(int)              { return 0; }
    virtual int       GetGamepadIndexForController(uint64_t)         { return -1; }
    virtual const char* GetStringForXboxOrigin(int)                  { return ""; }
    virtual const char* GetGlyphForXboxOrigin(int)                   { return ""; }
    virtual int       GetActionOriginFromXboxOrigin(uint64_t, int)   { return 0; }
    virtual int       TranslateActionOrigin(int, int)                { return 0; }
    // 44 — GetDeviceBindingRevision
    virtual bool      GetDeviceBindingRevision(uint64_t, int*, int*) { return false; }
    // 45-46 — Remote-Play awareness
    virtual uint32_t  GetRemotePlaySessionID(uint64_t)               { return 0; }
    virtual uint32_t  GetSessionInputConfigurationSettings()         { return 0; }
    // 47 — SetDualSenseTriggerEffect
    virtual void      SetDualSenseTriggerEffect(uint64_t, const void*) {}
};

// ---------------------------------------------------------------------------
// ISteamParties (version "SteamParties002"). Steam Library "Parties"
// feature — beacon-based party formation. Slot map from
// public/steam/isteamparties.h. Niche; few games probe it.
class ISteamPartiesStub {
public:
    // 0 — GetNumActiveBeacons
    virtual uint32_t  GetNumActiveBeacons()                          { return 0; }
    // 1 — GetBeaconByIndex
    virtual uint64_t  GetBeaconByIndex(uint32_t)                     { return 0; }
    // 2 — GetBeaconDetails(beacon, *steamIDBeaconOwner, *locationOut, *metadataBuf, cubMetadata)
    virtual bool      GetBeaconDetails(uint64_t, uint64_t*, void*, char* meta, int mn) {
        if (meta && mn > 0) meta[0] = '\0';
        return false;
    }
    // 3 — JoinParty → SteamAPICall_t
    virtual uint64_t  JoinParty(uint64_t)                            { return 0; }
    // 4 — GetNumAvailableBeaconLocations(*pNumLocations)
    virtual bool      GetNumAvailableBeaconLocations(uint32_t* pNum) {
        if (pNum) *pNum = 0;
        return false;
    }
    // 5 — GetAvailableBeaconLocations
    virtual bool      GetAvailableBeaconLocations(void*, uint32_t)   { return false; }
    // 6 — CreateBeacon → SteamAPICall_t
    virtual uint64_t  CreateBeacon(uint32_t, void*, int, const char*, const char*) { return 0; }
    // 7 — OnReservationCompleted
    virtual void      OnReservationCompleted(uint64_t, uint64_t)     {}
    // 8 — CancelReservation
    virtual void      CancelReservation(uint64_t, uint64_t)          {}
    // 9 — ChangeNumOpenSlots → SteamAPICall_t
    virtual uint64_t  ChangeNumOpenSlots(uint64_t, uint32_t)         { return 0; }
    // 10 — DestroyBeacon
    virtual bool      DestroyBeacon(uint64_t)                        { return false; }
    // 11 — GetBeaconLocationData(location, eData, *pchDataString, cchDataStringOut)
    virtual bool      GetBeaconLocationData(void*, int, char* str, int sn) {
        if (str && sn > 0) str[0] = '\0';
        return false;
    }
};

// ---------------------------------------------------------------------------
// ISteamRemotePlay (version "STEAMREMOTEPLAY_INTERFACE_VERSION001").
// Steam Link / Remote Play session awareness. Slot map from
// public/steam/isteamremoteplay.h. We're never a Remote Play host
// or client — all sessions report empty.
class ISteamRemotePlayStub {
public:
    // 0 — GetSessionCount
    virtual uint32_t  GetSessionCount()                              { return 0; }
    // 1 — GetSessionID(idx) → RemotePlaySessionID_t (uint32)
    virtual uint32_t  GetSessionID(int)                              { return 0; }
    // 2 — GetSessionSteamID(sessionID) → CSteamID
    virtual uint64_t  GetSessionSteamID(uint32_t)                    { return 0; }
    // 3 — GetSessionClientName(sessionID)
    virtual const char* GetSessionClientName(uint32_t)               { return ""; }
    // 4 — GetSessionClientFormFactor(sessionID) → ESteamDeviceFormFactor (0=Unknown)
    virtual int       GetSessionClientFormFactor(uint32_t)           { return 0; }
    // 5 — BGetSessionClientResolution(sessionID, *width, *height)
    virtual bool      BGetSessionClientResolution(uint32_t, int* w, int* h) {
        if (w) *w = 0;
        if (h) *h = 0;
        return false;
    }
    // 6 — BStartRemotePlayTogether(showOverlay)
    virtual bool      BStartRemotePlayTogether(bool)                 { return false; }
    // 7 — BSendRemotePlayTogetherInvite(friendID)
    virtual bool      BSendRemotePlayTogetherInvite(uint64_t)        { return false; }
};

// ---------------------------------------------------------------------------
// ISteamNetworkingSockets (version "SteamNetworkingSockets012" — the
// modern P2P/Steam-Datagram-Relay API that supersedes ISteamNetworking).
// Slot map from public/steamnetworkingsockets/isteamnetworkingsockets.h
// at SDK v1.57. Modern multiplayer games (anything released since 2019,
// including most CS2/Dota 2/everything that uses Valve's relay network)
// probe this interface via CreateInterface("SteamNetworkingSockets012")
// at startup. Previously NOT-EXPORTED — those probes hit `nullptr`
// returned from CreateInterface and games either fall back gracefully
// or crash on first call.
//
// Every slot returns a failure sentinel: invalid handles
// (k_HSteamNetConnection_Invalid = 0, k_HSteamListenSocket_Invalid = 0,
// k_HSteamNetPollGroup_Invalid = 0), zero results, or
// k_EResultNoConnection (3). Authentication slots return
// k_ESteamNetworkingAvailability_CannotTry (-102). Real implementation
// would tunnel through Valve's relay network; deferred.
class ISteamNetworkingSocketsStub {
public:
    // 0  CreateListenSocketIP
    virtual uint32_t CreateListenSocketIP(const void* /*pSteamNetworkingIPAddr*/,
                                          int, const void*)                          { return 0; }
    // 1  ConnectByIPAddress
    virtual uint32_t ConnectByIPAddress(const void*, int, const void*)               { return 0; }
    // 2  CreateListenSocketP2P
    virtual uint32_t CreateListenSocketP2P(int, int, const void*)                    { return 0; }
    // 3  ConnectP2P
    virtual uint32_t ConnectP2P(const void* /*identityRemote*/, int, int, const void*) { return 0; }
    // 4  AcceptConnection → EResult (3 = NoConnection)
    virtual int      AcceptConnection(uint32_t)                                      { return 3; }
    // 5  CloseConnection(hPeer, nReason, pszDebug, bEnableLinger)
    virtual bool     CloseConnection(uint32_t, int, const char*, bool)               { return false; }
    // 6  CloseListenSocket
    virtual bool     CloseListenSocket(uint32_t)                                     { return false; }
    // 7  SetConnectionUserData(hPeer, nUserData)
    virtual bool     SetConnectionUserData(uint32_t, int64_t)                        { return false; }
    // 8  GetConnectionUserData → int64
    virtual int64_t  GetConnectionUserData(uint32_t)                                 { return -1; }
    // 9  SetConnectionName
    virtual void     SetConnectionName(uint32_t, const char*)                        {}
    // 10 GetConnectionName(hPeer, *pszName, nMaxLen)
    virtual bool     GetConnectionName(uint32_t, char* buf, int cap) {
        if (buf && cap > 0) buf[0] = '\0';
        return false;
    }
    // 11 SendMessageToConnection(hConn, pData, cbData, nSendFlags, *pOutMessageNumber)
    virtual int      SendMessageToConnection(uint32_t, const void*, uint32_t, int, int64_t*) { return 3; }
    // 12 SendMessages(nMessages, *pMessages, *pOutMessageNumberOrResult)
    virtual void     SendMessages(int, const void* const*, int64_t*)                 {}
    // 13 FlushMessagesOnConnection → EResult
    virtual int      FlushMessagesOnConnection(uint32_t)                             { return 3; }
    // 14 ReceiveMessagesOnConnection → int (count)
    virtual int      ReceiveMessagesOnConnection(uint32_t, void** /*ppOutMessages*/, int) { return 0; }
    // 15 CreatePollGroup → handle
    virtual uint32_t CreatePollGroup()                                               { return 0; }
    // 16 DestroyPollGroup
    virtual bool     DestroyPollGroup(uint32_t)                                      { return false; }
    // 17 SetConnectionPollGroup
    virtual bool     SetConnectionPollGroup(uint32_t, uint32_t)                      { return false; }
    // 18 ReceiveMessagesOnPollGroup → int (count)
    virtual int      ReceiveMessagesOnPollGroup(uint32_t, void**, int)               { return 0; }
    // 19 GetConnectionInfo(hConn, *pInfo)
    virtual bool     GetConnectionInfo(uint32_t, void*)                              { return false; }
    // 20 GetConnectionRealTimeStatus(hConn, *pStatus, nLanes, *pLanes)
    virtual int      GetConnectionRealTimeStatus(uint32_t, void*, int, void*)        { return 3; }
    // 21 GetDetailedConnectionStatus(hConn, *pszBuf, cbBuf) → int (status)
    virtual int      GetDetailedConnectionStatus(uint32_t, char* buf, int cap) {
        if (buf && cap > 0) buf[0] = '\0';
        return -1;
    }
    // 22 GetListenSocketAddress(hSocket, *address)
    virtual bool     GetListenSocketAddress(uint32_t, void*)                         { return false; }
    // 23 CreateSocketPair(*pOutConnection1, *pOutConnection2, bUseNetworkLoopback,
    //                     *pIdentity1, *pIdentity2)
    virtual bool     CreateSocketPair(uint32_t* a, uint32_t* b, bool, const void*, const void*) {
        if (a) *a = 0;
        if (b) *b = 0;
        return false;
    }
    // 24 ConfigureConnectionLanes(hConn, nNumLanes, *pLanePriorities, *pLaneWeights)
    virtual int      ConfigureConnectionLanes(uint32_t, int, const int*, const uint16_t*) { return 3; }
    // 25 GetIdentity(*pIdentity). Fill the SteamNetworkingIdentity
    //   struct with our current SteamID (SetSteamID64 equivalent). SDK
    //   contract for the struct:
    //     int32 m_eType (16 = k_ESteamNetworkingIdentityType_SteamID)
    //     int32 m_cbSize (8 for SteamID64)
    //     union { uint64 m_steamID64; ... }  // 128 bytes (max)
    //   Total = 136 bytes; we only touch the first 16 + zero the tail.
    //   Modern netsockets games probe this at init to confirm logon;
    //   returning false / leaving the struct zeroed makes them treat
    //   the local user as "anonymous" and refuse to host/connect.
    virtual bool     GetIdentity(void* pIdentity) {
        if (!pIdentity) return false;
        const uint64_t sid = pushed().steam_id.load();
        if (sid == 0) return false;
        // First 16B = type + cbSize + SteamID64. Real struct is 136B
        // but the union tail past the steam_id is union-padding the
        // caller doesn't read after seeing type=SteamID.
        struct NetIdentitySteamIDPrefix {
            int32_t  e_type;
            int32_t  cb_size;
            uint64_t steam_id64;
        };
        std::memset(pIdentity, 0, 136);
        auto* out = reinterpret_cast<NetIdentitySteamIDPrefix*>(pIdentity);
        out->e_type     = 16;  // k_ESteamNetworkingIdentityType_SteamID
        out->cb_size    = sizeof(uint64_t);
        out->steam_id64 = sid;
        return true;
    }
    // 26 InitAuthentication → ESteamNetworkingAvailability (-102 = CannotTry)
    virtual int      InitAuthentication()                                            { return -102; }
    // 27 GetAuthenticationStatus(*pDetails)
    virtual int      GetAuthenticationStatus(void*)                                  { return -102; }
    // 28 ReceivedRelayAuthTicket(*pvTicket, cbTicket, *pOutParsedTicket)
    virtual bool     ReceivedRelayAuthTicket(const void*, int, void*)                { return false; }
    // 29 FindRelayAuthTicketForServer(*identityGameServer, nRemoteVirtualPort, *pOutParsedTicket)
    virtual int      FindRelayAuthTicketForServer(const void*, int, void*)           { return 0; }
    // 30 ConnectToHostedDedicatedServer(*identityTarget, nRemoteVirtualPort, nOptions, *pOptions)
    virtual uint32_t ConnectToHostedDedicatedServer(const void*, int, int, const void*) { return 0; }
    // 31 GetHostedDedicatedServerPort
    virtual uint16_t GetHostedDedicatedServerPort()                                  { return 0; }
    // 32 GetHostedDedicatedServerPOPID → SteamNetworkingPOPID (uint32)
    virtual uint32_t GetHostedDedicatedServerPOPID()                                 { return 0; }
    // 33 GetHostedDedicatedServerAddress(*pRouting) → EResult
    virtual int      GetHostedDedicatedServerAddress(void*)                          { return 3; }
    // 34 CreateHostedDedicatedServerListenSocket(nLocalVirtualPort, nOptions, *pOptions)
    virtual uint32_t CreateHostedDedicatedServerListenSocket(int, int, const void*)  { return 0; }
    // 35 GetGameCoordinatorServerLogin(*pLoginInfo, *pcbSignedBlob, *pBlob) → EResult
    virtual int      GetGameCoordinatorServerLogin(void*, int*, void*)               { return 3; }
    // 36 ConnectP2PCustomSignaling
    virtual uint32_t ConnectP2PCustomSignaling(void*, const void*, int, int, const void*) { return 0; }
    // 37 ReceivedP2PCustomSignal
    virtual bool     ReceivedP2PCustomSignal(const void*, int, void*)                { return false; }
    // 38 GetCertificateRequest(*pcbBlob, *pBlob, *pErrorMsg)
    virtual bool     GetCertificateRequest(int*, void*, void*)                       { return false; }
    // 39 SetCertificate(*pCertificate, cbCertificate, *pErrorMsg)
    virtual bool     SetCertificate(const void*, int, void*)                         { return false; }
    // 40 ResetIdentity(*pIdentity)
    virtual void     ResetIdentity(const void*)                                      {}
    // 41 RunCallbacks
    virtual void     RunCallbacks()                                                  {}
    // 42 BeginAsyncRequestFakeIP(nNumPorts)
    virtual bool     BeginAsyncRequestFakeIP(int)                                    { return false; }
    // 43 GetFakeIP(idxFirstPort, *pInfo)
    virtual void     GetFakeIP(int, void*)                                           {}
    // 44 CreateListenSocketP2PFakeIP(idxFakePort, nOptions, *pOptions)
    virtual uint32_t CreateListenSocketP2PFakeIP(int, int, const void*)              { return 0; }
    // 45 GetRemoteFakeIPForConnection(hConn, *pOutAddr) → EResult
    virtual int      GetRemoteFakeIPForConnection(uint32_t, void*)                   { return 3; }
    // 46 CreateFakeUDPPort(idxFakeServerPort) → ISteamNetworkingFakeUDPPort*
    virtual void*    CreateFakeUDPPort(int)                                          { return nullptr; }
};

// ---------------------------------------------------------------------------
// ISteamNetworkingUtils (version "SteamNetworkingUtils004"). Helpers for
// configuration, identity formatting, address parsing, and global
// callback registration. Most games initialize Networking via this
// interface (SetGlobalCallback_SteamNetConnectionStatusChanged on
// startup). Failing the lookup is a crash; failing the calls is just
// "no callbacks fire" which games handle.
class ISteamNetworkingUtilsStub {
public:
    // 0  AllocateMessage(cbAllocateBuffer) → ISteamNetworkingMessage*
    virtual void*    AllocateMessage(int)                                            { return nullptr; }
    // 1  InitRelayNetworkAccess
    virtual void     InitRelayNetworkAccess()                                        {}
    // 2  GetRelayNetworkStatus(*pDetails) → ESteamNetworkingAvailability
    virtual int      GetRelayNetworkStatus(void*)                                    { return -102; }
    // 3  GetLocalPingLocation(*pResult) → float (age sec)
    virtual float    GetLocalPingLocation(void*)                                     { return -1.0f; }
    // 4  EstimatePingTimeBetweenTwoLocations(*pLoc1, *pLoc2)
    virtual int      EstimatePingTimeBetweenTwoLocations(const void*, const void*)   { return -1; }
    // 5  EstimatePingTimeFromLocalHost(*pLoc)
    virtual int      EstimatePingTimeFromLocalHost(const void*)                      { return -1; }
    // 6  ConvertPingLocationToString(*pLoc, *pszBuf, cbBuf)
    virtual void     ConvertPingLocationToString(const void*, char* buf, int cap) {
        if (buf && cap > 0) buf[0] = '\0';
    }
    // 7  ParsePingLocationString(*pszString, *pResult)
    virtual bool     ParsePingLocationString(const char*, void*)                     { return false; }
    // 8  CheckPingDataUpToDate(flMaxAgeSeconds)
    virtual bool     CheckPingDataUpToDate(float)                                    { return true; }
    // 9  GetPingToDataCenter(popID, *pViaRelayPoP)
    virtual int      GetPingToDataCenter(uint32_t, uint32_t*)                        { return -1; }
    // 10 GetDirectPingToPOP(popID)
    virtual int      GetDirectPingToPOP(uint32_t)                                    { return -1; }
    // 11 GetPOPCount
    virtual int      GetPOPCount()                                                   { return 0; }
    // 12 GetPOPList(*pList, nListSz)
    virtual int      GetPOPList(uint32_t*, int)                                      { return 0; }
    // 13 GetLocalTimestamp → SteamNetworkingMicroseconds (int64)
    virtual int64_t  GetLocalTimestamp() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // 14 SetDebugOutputFunction(eDetailLevel, *pfnFunc)
    virtual void     SetDebugOutputFunction(int, void*)                              {}
    // 15 IsFakeIPv4(nIPv4)
    virtual bool     IsFakeIPv4(uint32_t)                                            { return false; }
    // 16 GetIPv4FakeIPType(nIPv4)
    virtual int      GetIPv4FakeIPType(uint32_t)                                     { return 0; }
    // 17 GetRealIdentityForFakeIP(*fakeIP, *pOutRealIdentity)
    virtual int      GetRealIdentityForFakeIP(const void*, void*)                    { return 3; }
    // 18 SetGlobalConfigValueInt32(eValue, val)
    virtual bool     SetGlobalConfigValueInt32(int, int)                             { return false; }
    // 19 SetGlobalConfigValueFloat(eValue, val)
    virtual bool     SetGlobalConfigValueFloat(int, float)                           { return false; }
    // 20 SetGlobalConfigValueString(eValue, *val)
    virtual bool     SetGlobalConfigValueString(int, const char*)                    { return false; }
    // 21 SetGlobalConfigValuePtr(eValue, *val)
    virtual bool     SetGlobalConfigValuePtr(int, void*)                             { return false; }
    // 22 SetConnectionConfigValueInt32(hConn, eValue, val)
    virtual bool     SetConnectionConfigValueInt32(uint32_t, int, int)               { return false; }
    // 23 SetConnectionConfigValueFloat(hConn, eValue, val)
    virtual bool     SetConnectionConfigValueFloat(uint32_t, int, float)             { return false; }
    // 24 SetConnectionConfigValueString(hConn, eValue, *val)
    virtual bool     SetConnectionConfigValueString(uint32_t, int, const char*)      { return false; }
    // 25 SetConfigValue(eValue, eScopeType, scopeObj, eDataType, *pArg)
    virtual bool     SetConfigValue(int, int, intptr_t, int, const void*)            { return false; }
    // 26 SetConfigValueStruct(*opt, eScopeType, scopeObj)
    virtual bool     SetConfigValueStruct(const void*, int, intptr_t)                { return false; }
    // 27 GetConfigValue(eValue, eScopeType, scopeObj, *pOutDataType, *pResult, *cbResult)
    virtual int      GetConfigValue(int, int, intptr_t, int*, void*, uint64_t*)      { return -1; }
    // 28 GetConfigValueInfo(eValue, *pOutName, *pOutDataType, *pOutScope, *pOutNextValue)
    virtual const char* GetConfigValueInfo(int, int*, int*, int*)                    { return nullptr; }
    // 29 IterateGenericEditableConfigValues(eCurrent, bEnumerateDevVars)
    virtual int      IterateGenericEditableConfigValues(int, bool)                   { return 0; }
    // 30 SteamNetworkingIPAddr_ToString(*addr, *buf, cbBuf, bWithPort).
    //   Format an 18-byte SteamNetworkingIPAddr (16B IPv6 union + 2B
    //   port) as a string. v4-mapped (m_8zeros == 0 && m_ffff == 0xffff)
    //   renders as "1.2.3.4:port"; otherwise IPv6 colon notation.
    //   Games log this in matchmaking UI + dedicated-server probes.
    virtual void     SteamNetworkingIPAddr_ToString(const void* pAddr,
                                                     char* buf, uint32_t cap,
                                                     bool with_port) {
        if (!buf || cap == 0) return;
        buf[0] = '\0';
        if (!pAddr) return;
        const auto* p = reinterpret_cast<const uint8_t*>(pAddr);
        // IPv6 layout: bytes 0..15 = address; bytes 16..17 = port (BE).
        // IPv4-mapped: bytes 0..9 = 0, bytes 10..11 = 0xff 0xff,
        //              bytes 12..15 = v4 octets.
        bool v4mapped = true;
        for (int i = 0; i < 10; ++i) if (p[i] != 0) { v4mapped = false; break; }
        if (v4mapped && (p[10] != 0xff || p[11] != 0xff)) v4mapped = false;
        const uint16_t port_be = (uint16_t(p[16]) << 8) | uint16_t(p[17]);
        if (v4mapped) {
            if (with_port) {
                std::snprintf(buf, cap, "%u.%u.%u.%u:%u",
                              p[12], p[13], p[14], p[15], port_be);
            } else {
                std::snprintf(buf, cap, "%u.%u.%u.%u",
                              p[12], p[13], p[14], p[15]);
            }
        } else {
            // Plain non-collapsed IPv6 hex, sufficient for log lines.
            if (with_port) {
                std::snprintf(buf, cap,
                    "[%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x]:%u",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                    p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
                    port_be);
            } else {
                std::snprintf(buf, cap,
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                    p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            }
        }
    }
    // 31 SteamNetworkingIPAddr_ParseString(*pAddr, *pszStr). Parses
    //   "a.b.c.d" or "a.b.c.d:port" into a v4-mapped 18-byte address.
    //   IPv6 parsing deferred — games rarely hand us literal v6 strings.
    virtual bool     SteamNetworkingIPAddr_ParseString(void* pAddr, const char* s) {
        if (!pAddr || !s || !*s) return false;
        unsigned a=0,b=0,c=0,d=0,port=0;
        int matched = std::sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port);
        if (matched < 4 || a>255 || b>255 || c>255 || d>255 || port>65535) {
            // Try without port.
            matched = std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
            if (matched < 4 || a>255 || b>255 || c>255 || d>255) return false;
            port = 0;
        }
        auto* out = reinterpret_cast<uint8_t*>(pAddr);
        std::memset(out, 0, 18);
        out[10] = 0xff;
        out[11] = 0xff;
        out[12] = static_cast<uint8_t>(a);
        out[13] = static_cast<uint8_t>(b);
        out[14] = static_cast<uint8_t>(c);
        out[15] = static_cast<uint8_t>(d);
        out[16] = static_cast<uint8_t>((port >> 8) & 0xff);
        out[17] = static_cast<uint8_t>(port & 0xff);
        return true;
    }
    // 32 SteamNetworkingIPAddr_GetFakeIPType(*addr)
    virtual int      SteamNetworkingIPAddr_GetFakeIPType(const void*)                { return 0; }
    // 33 SteamNetworkingIdentity_ToString(*identity, *buf, cbBuf).
    //   Format type-tagged identity as a string per SDK convention.
    //   "steamid:<id>" for SteamID, "ip:<addr>" for IPAddress, "" for
    //   invalid/unknown. Games log identities in matchmaking traces.
    virtual void     SteamNetworkingIdentity_ToString(const void* pId, char* buf, uint32_t cap) {
        if (!buf || cap == 0) return;
        buf[0] = '\0';
        if (!pId) return;
        struct NetIdentityHead {
            int32_t  e_type;
            int32_t  cb_size;
            uint64_t steam_id64;  // SteamID variant
        };
        const auto* h = reinterpret_cast<const NetIdentityHead*>(pId);
        if (h->e_type == 16 /*SteamID*/) {
            std::snprintf(buf, cap, "steamid:%llu",
                          static_cast<unsigned long long>(h->steam_id64));
        } else if (h->e_type == 1 /*IPAddress*/) {
            char tmp[64] = {};
            SteamNetworkingIPAddr_ToString(
                reinterpret_cast<const uint8_t*>(pId) + 8, tmp, sizeof(tmp), true);
            std::snprintf(buf, cap, "ip:%s", tmp);
        }
    }
    // 34 SteamNetworkingIdentity_ParseString(*pIdentity, *pszStr)
    virtual bool     SteamNetworkingIdentity_ParseString(void*, const char*)         { return false; }
};

// ---------------------------------------------------------------------------
// ISteamNetworkingMessages (version "SteamNetworkingMessages002"). The
// connection-less message-passing API — kept around for games that
// were ported from ISteamNetworking but want messages-style semantics
// without managing connections.
class ISteamNetworkingMessagesStub {
public:
    // 0 SendMessageToUser(*identityRemote, *pubData, cubData, nSendFlags, nRemoteChannel)
    virtual int  SendMessageToUser(const void*, const void*, uint32_t, int, int) { return 3; }
    // 1 ReceiveMessagesOnChannel(nLocalChannel, *ppOutMessages, nMaxMessages)
    virtual int  ReceiveMessagesOnChannel(int, void**, int)                      { return 0; }
    // 2 AcceptSessionWithUser(*identityRemote)
    virtual bool AcceptSessionWithUser(const void*)                              { return false; }
    // 3 CloseSessionWithUser(*identityRemote)
    virtual bool CloseSessionWithUser(const void*)                               { return false; }
    // 4 CloseChannelWithUser(*identityRemote, nLocalChannel)
    virtual bool CloseChannelWithUser(const void*, int)                          { return false; }
    // 5 GetSessionConnectionInfo(*identityRemote, *pConnectionInfo, *pQuickStatus)
    virtual int  GetSessionConnectionInfo(const void*, void*, void*)             { return 0; }
};

// ---------------------------------------------------------------------------
// Process-singleton stub instances. Pointers handed out by
// ISteamClient.GetISteam* are these. The classes themselves are not
// thread-safe yet; that's fine because the stubs are stateless reads.
// ---------------------------------------------------------------------------

static ISteamUtilsStub        g_steam_utils;
static ISteamUserStub         g_steam_user;
static ISteamAppsStub         g_steam_apps;
static ISteamFriendsStub      g_steam_friends;
static ISteamRemoteStorageStub g_steam_remote_storage;
static ISteamUserStatsStub    g_steam_user_stats;
static ISteamInventoryStub    g_steam_inventory;
static ISteamScreenshotsStub  g_steam_screenshots;
static ISteamMusicStub        g_steam_music;
static ISteamAppListStub      g_steam_app_list;
static ISteamVideoStub        g_steam_video;
static ISteamParentalSettingsStub g_steam_parental;
static ISteamMatchmakingServersStub g_steam_matchmaking_servers;
static ISteamMatchmakingStub  g_steam_matchmaking;
static ISteamNetworkingStub   g_steam_networking;
static ISteamUGCStub          g_steam_ugc;
static ISteamGameServerStub   g_steam_game_server;
static ISteamMusicRemoteStub  g_steam_music_remote;
static ISteamHTMLSurfaceStub  g_steam_html_surface;
static ISteamInputStub        g_steam_input;
static ISteamPartiesStub      g_steam_parties;
static ISteamRemotePlayStub   g_steam_remote_play;
static ISteamNetworkingSocketsStub  g_steam_networking_sockets;
static ISteamNetworkingUtilsStub    g_steam_networking_utils;
static ISteamNetworkingMessagesStub g_steam_networking_messages;

extern "C" void* wn_get_isteam_utils()         { return &g_steam_utils; }
extern "C" void* wn_get_isteam_user()          { return &g_steam_user; }
extern "C" void* wn_get_isteam_apps()          { return &g_steam_apps; }
extern "C" void* wn_get_isteam_friends()       { return &g_steam_friends; }
extern "C" void* wn_get_isteam_remote_storage(){ return &g_steam_remote_storage; }
extern "C" void* wn_get_isteam_user_stats()    { return &g_steam_user_stats; }
extern "C" void* wn_get_isteam_inventory()     { return &g_steam_inventory; }
extern "C" void* wn_get_isteam_screenshots()   { return &g_steam_screenshots; }
extern "C" void* wn_get_isteam_music()         { return &g_steam_music; }
extern "C" void* wn_get_isteam_app_list()      { return &g_steam_app_list; }
extern "C" void* wn_get_isteam_video()         { return &g_steam_video; }
extern "C" void* wn_get_isteam_parental()      { return &g_steam_parental; }
extern "C" void* wn_get_isteam_matchmaking_servers() { return &g_steam_matchmaking_servers; }
extern "C" void* wn_get_isteam_matchmaking() { return &g_steam_matchmaking; }
extern "C" void* wn_get_isteam_networking()  { return &g_steam_networking; }
extern "C" void* wn_get_isteam_ugc()         { return &g_steam_ugc; }
extern "C" void* wn_get_isteam_game_server() { return &g_steam_game_server; }
extern "C" void* wn_get_isteam_music_remote() { return &g_steam_music_remote; }
extern "C" void* wn_get_isteam_html_surface() { return &g_steam_html_surface; }
extern "C" void* wn_get_isteam_input()        { return &g_steam_input; }
extern "C" void* wn_get_isteam_parties()      { return &g_steam_parties; }
extern "C" void* wn_get_isteam_remote_play()  { return &g_steam_remote_play; }
extern "C" void* wn_get_isteam_networking_sockets()  { return &g_steam_networking_sockets; }
extern "C" void* wn_get_isteam_networking_utils()    { return &g_steam_networking_utils; }
extern "C" void* wn_get_isteam_networking_messages() { return &g_steam_networking_messages; }

}  // namespace wn_libsteamclient
