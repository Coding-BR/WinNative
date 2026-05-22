// JNI setters that let Kotlin push fresh session state into the .so
// without a vtable change. The interface stubs (isteam_stubs.cpp,
// isteam_client.cpp) read these values when answering callers.
//
// Java class: com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
// (a thin Kotlin object that loads "steamclient" and calls these
// directly via `System.loadLibrary("steamclient")`).
//
// Lifetimes: pushed state is a process singleton; the setters can be
// called at any time from any thread. Strings are copied under
// state_mutex(); atomic scalars don't need the lock.

#include "wn_libsteamclient/runtime_state.h"
#include "wn_libsteamclient/callbacks.h"
#include "wn_libsteamclient/callback_registry.h"
#include "wn_libsteamclient/tcp_services.h"
// C-ABI bridge into the live wn-steam-client CMClient (linked against
// libwnsteam.so at build time, resolved by dynamic linker at app start).
#include "wn_steam/cm_bridge.h"

#include <jni.h>
#include <android/log.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cb = wn_libsteamclient::callbacks;
namespace lsc = wn_libsteamclient;

namespace {
// Helper — emits one PersonaStateChange_t into the callback queue.
// Used by every persona setter. flags is the EPersonaChange bitmask
// describing what changed (kPersonaChangeName for a name push, etc.).
void emit_persona_state_change(uint64_t steam_id, int32_t flags) {
    if (steam_id == 0) return;
    cb::PersonaStateChange payload{};
    payload.m_ulSteamID    = steam_id;
    payload.m_nChangeFlags = flags;
    lsc::push_callback(lsc::state().user.load(),
                       cb::kPersonaStateChange,
                       &payload, sizeof(payload));
}

// Callback bridge observer — invoked by CMClient on the CM transport
// thread when a CMsgClientPersonaState arrives. Mirrors the friend's
// fields into pushed_state + emits one PersonaStateChange_t with the
// appropriate change-flags bitmask describing what landed.
//
// All field copies happen inside the state mutex; emit happens after
// the lock is released (push_callback acquires its own queue mutex and
// the dispatcher could re-enter our setters → re-entrant lock would
// deadlock).
//
// Avatar hash is mirrored byte-exact; existing AvatarFetcher.enqueue
// flow on the Kotlin side is unaffected (this is C++ → C++, no Java
// involvement).
void on_persona_event(const WnCmPersonaEvent* ev) {
    if (!ev || ev->sid == 0) return;
    int32_t flags = 0;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        const bool is_self = (ev->sid == p.steam_id.load());
        // Self path writes pushed.persona_name + pushed.persona_state
        // (the singular self fields); friend path writes the per-sid
        // maps. Shared fields (avatar_hash, rich_presence,
        // game_played_app) live in the same maps and are keyed by sid
        // so self entries co-exist there for uniform read access via
        // ISteamFriends.GetSmallFriendAvatar(self_sid) etc.
        if (ev->name && ev->name[0]) {
            if (is_self) {
                if (p.persona_name != ev->name) {
                    p.persona_name = ev->name;
                    flags |= cb::kPersonaChangeName;
                }
            } else {
                std::string& slot = p.friend_persona_names[ev->sid];
                if (slot != ev->name) {
                    slot = ev->name;
                    flags |= cb::kPersonaChangeName;
                }
            }
        }
        if (ev->persona_state != UINT32_MAX) {
            uint32_t prev;
            if (is_self) {
                prev = static_cast<uint32_t>(p.persona_state.load());
            } else {
                auto it = p.friend_persona_states.find(ev->sid);
                prev = (it == p.friend_persona_states.end()) ? 0 : it->second;
            }
            if (prev != ev->persona_state) {
                if (is_self) {
                    p.persona_state.store(static_cast<int>(ev->persona_state));
                } else {
                    p.friend_persona_states[ev->sid] = ev->persona_state;
                }
                flags |= cb::kPersonaChangeStatus;
                // EPersonaState 0 = Offline; anything else is some flavor
                // of online (1=Online, 2=Busy, 3=Away, 4=Snooze,
                // 5=LookingToTrade, 6=LookingToPlay, 7=Invisible).
                // Games gate friend-list "X came online" toasts on these
                // specific flag bits — without them, the toast doesn't
                // fire. Same synthesis whether self or friend.
                if (prev == 0 && ev->persona_state != 0) {
                    flags |= cb::kPersonaChangeComeOnline;
                }
                if (prev != 0 && ev->persona_state == 0) {
                    flags |= cb::kPersonaChangeGoneOffline;
                }
            }
        }
        // game_played_app: same map for self + friends — keyed by sid.
        // 0 vs non-zero is the meaningful transition.
        {
            uint32_t& slot = p.friend_game_played_app[ev->sid];
            if (slot != ev->game_played_app) {
                slot = ev->game_played_app;
                flags |= cb::kPersonaChangeGamePlayed;
            }
        }
        if (ev->avatar_hash && ev->avatar_hash_len > 0) {
            std::vector<uint8_t> hash(
                ev->avatar_hash, ev->avatar_hash + ev->avatar_hash_len);
            auto& slot = p.friend_avatar_hashes[ev->sid];
            if (slot != hash) {
                slot = std::move(hash);
                flags |= cb::kPersonaChangeAvatar;
            }
        }
        // Rich-presence map. CMsgClientPersonaState carries the FULL
        // current RP set when the request flag had 0x800 set — so
        // replacing the cached vector wholesale is correct (matches
        // Steam's semantics: each PersonaState push is a complete
        // snapshot, not a delta).
        if (ev->rp_pairs && ev->rp_count > 0) {
            std::vector<std::pair<std::string, std::string>> fresh;
            fresh.reserve(ev->rp_count);
            for (size_t i = 0; i < ev->rp_count; ++i) {
                const auto& kv = ev->rp_pairs[i];
                fresh.emplace_back(
                    kv.key   ? kv.key   : "",
                    kv.value ? kv.value : "");
            }
            auto& slot = p.rich_presence[ev->sid];
            if (slot != fresh) {
                slot = std::move(fresh);
                // Rich-presence updates fire a SEPARATE callback type
                // (FriendRichPresenceUpdate_t, 1336) — NOT a flag bit on
                // PersonaStateChange_t. Emit it after releasing the
                // state mutex (re-entrancy safety; push_callback takes
                // its own lock).
            }
        }
    }
    if (flags != 0) emit_persona_state_change(ev->sid, flags);
    // FriendRichPresenceUpdate_t is independent of the PersonaStateChange
    // _t bitmask path. Emit when RP arrived in this slice — even if the
    // content matched (the SDK contract is "fire on receipt"; games may
    // re-query GetFriendRichPresence to confirm cached values are still
    // current).
    if (ev->rp_pairs && ev->rp_count > 0) {
        cb::FriendRichPresenceUpdate rp{};
        rp.m_steamIDFriend = ev->sid;
        rp.m_nAppID        = lsc::pushed().app_id.load();
        lsc::push_callback(lsc::state().user.load(),
                           cb::kFriendRichPresenceUpdate,
                           &rp, sizeof(rp));
    }
}

// Register the observer with cm_bridge at .so load time. __attribute__
// ((constructor)) fires after the dynamic linker resolves wn_cm_bridge
// _register_persona_observer (libwnsteam.so is a NEEDED dep). One-
// shot — observer slot is process-wide and our handler is idempotent.
__attribute__((constructor))
void register_persona_observer() {
    wn_cm_bridge_register_persona_observer(&on_persona_event);
}

// Reactive logon-state observer — CMClient state transitions arrive
// here. We translate to lsc::set_logged_on which idempotently emits
// SteamServersConnected_t / SteamServersDisconnected_t via push_callback.
//
// The Kotlin path (WnLibSteamClient.setLoggedOn) stays for cold-warm
// cases where wn-session ISN'T live (e.g. PrefManager-replay-only). When
// both fire (Kotlin sets true post-prefs warmup, then wn-session sets
// true after real CM logon), set_logged_on's prev==new dedup elides
// the double-emit.
void on_logon_state_event(bool logged_on) {
    lsc::set_logged_on(logged_on);
}

__attribute__((constructor))
void register_logon_state_observer() {
    wn_cm_bridge_register_logon_state_observer(&on_logon_state_event);
}

// Reactive friends-list observer — invoked by CMClient on every
// CMsgClientFriendsList ingest (initial post-logon snapshot + each
// incremental relationship change). Wholesale-replaces pushed.friends
// with the mutual-friend SID set; matches the existing
// nativeSetFriendsList semantics (full snapshot per push).
//
// No SDK callback is emitted — friends-list isn't a Steam callback
// type. Games re-read GetFriendCount / GetFriendByIndex on whatever
// schedule they choose; the pushed.friends vector is consistent at
// any read after the observer returns.
void on_friends_list_event(const uint64_t* sids, size_t count) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.friends.clear();
    if (sids && count > 0) {
        p.friends.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (sids[i] != 0) p.friends.push_back(sids[i]);
        }
    }
    // WN_TAG macro is defined further down the file; use the string literal
    // here so this observer (defined inside the helpers anonymous namespace)
    // can log without a forward-decl.
    __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
        "friends-list observer: %zu mutual friend(s) mirrored", p.friends.size());
}

__attribute__((constructor))
void register_friends_list_observer() {
    wn_cm_bridge_register_friends_list_observer(&on_friends_list_event);
}

// Reactive license-list observer — invoked by CMClient on every
// CMsgClientLicenseList ingest. Wholesale-replaces pushed.licenses
// with the current full set; supports ISteamApps.BIsSubscribedFrom
// FamilySharing + per-package metadata queries.
void on_license_list_event(const WnCmLicenseEntry* licenses, size_t count) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.licenses.clear();
    if (licenses && count > 0) {
        p.licenses.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& src = licenses[i];
            if (src.package_id == 0) continue;
            p.licenses[src.package_id] = lsc::PushedState::LicenseEntry{
                src.package_id,
                src.owner_id,
                src.time_created,
                src.license_type,
                src.flags,
                src.change_number,
                src.minute_limit,
                src.minutes_used,
            };
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
        "license-list observer: %zu license(s) mirrored", p.licenses.size());
}

__attribute__((constructor))
void register_license_list_observer() {
    wn_cm_bridge_register_license_list_observer(&on_license_list_event);
}

// Reactive account-info observer — invoked by CMClient on every
// CMsgClientAccountInfo ingest. Mirrors the flag + string fields into
// pushed-state so ISteamUser slots 26-29 and ISteamUtils GetIPCountry /
// SteamUser persona-name accessors return real data.
void on_account_info_event(const WnCmAccountInfo* info) {
    if (!info) return;
    auto& p = lsc::pushed();
    p.account_two_factor_enabled.store(info->two_factor_enabled);
    p.account_phone_verified.store(info->phone_verified);
    p.account_phone_identifying.store(info->phone_identifying);
    p.account_phone_requires_verification.store(info->phone_requires_verification);

    // persona_name + ip_country: copy out of caller's buffer under the
    // state mutex; the pointers do not outlive this callback. Don't
    // clobber a previously-good value with an empty incoming one.
    if (info->persona_name && info->persona_name_len > 0) {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        p.persona_name.assign(info->persona_name, info->persona_name_len);
    }
    if (info->ip_country && info->ip_country_len > 0) {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        p.ip_country.assign(info->ip_country, info->ip_country_len);
        p.ip_country_set.store(1);
    }

    __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
        "account-info observer: persona='%.*s' ip='%.*s' 2FA=%d phone_v=%d phone_id=%d phone_nv=%d",
        static_cast<int>(info->persona_name_len), info->persona_name ? info->persona_name : "",
        static_cast<int>(info->ip_country_len),   info->ip_country   ? info->ip_country   : "",
        info->two_factor_enabled, info->phone_verified,
        info->phone_identifying, info->phone_requires_verification);
}

__attribute__((constructor))
void register_account_info_observer() {
    wn_cm_bridge_register_account_info_observer(&on_account_info_event);
}

// Reactive ISteamMatchmaking lobby-data observer — invoked from CMClient
// on every server-pushed ClientMMSLobbyData (EMsg 6612). Mirrors the
// lobby state into pushed().active_lobbies and emits LobbyDataUpdate_t
// so any game-side callback listener that registered to know "this
// lobby changed" wakes up.
//
// The bridge owns the lifetime of the WnCmLobbyData pointer + nested
// arrays for exactly this call — we must copy strings/bytes into the
// pushed-state mirror before returning.
void on_lobby_data_event(const WnCmLobbyData* data) {
    if (!data) return;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto& L = p.active_lobbies[data->steam_id_lobby];
        L.app_id      = data->app_id;
        L.owner_sid   = data->steam_id_owner;
        L.max_members = data->max_members;
        L.lobby_type  = data->lobby_type;
        L.lobby_flags = data->lobby_flags;
        // Replace the member set in full — Steam sends the canonical
        // current list on every update; a removed member won't appear.
        L.members.clear();
        for (size_t i = 0; i < data->member_count; ++i) {
            const auto& m = data->members[i];
            auto& mb = L.members[m.steam_id];
            if (m.persona_name) mb.persona_name = m.persona_name;
            // m.metadata bytes are an opaque KV blob; full KV parse is
            // Phase C work — we stash the raw bytes by length in case
            // a later read wants them, under an internal key.
            if (m.metadata_bytes && m.metadata_len > 0) {
                mb.data["__raw_metadata"] = std::string(
                    reinterpret_cast<const char*>(m.metadata_bytes),
                    m.metadata_len);
            }
        }
    }
    // Emit LobbyDataUpdate_t (kLobbyDataUpdate = 505). Layout:
    //   uint64 m_ulSteamIDLobby  (0)
    //   uint64 m_ulSteamIDMember (0 = lobby-level update)
    //   uint8  m_bSuccess        (1 = OK)
    //   pad x7 to 24B total.
    struct LobbyDataUpdate { uint64_t lobby; uint64_t member; uint8_t success; uint8_t _pad[7]; };
    LobbyDataUpdate cb{};
    cb.lobby   = data->steam_id_lobby;
    cb.member  = 0;
    cb.success = 1;
    lsc::push_callback(lsc::state().user.load(), /*kLobbyDataUpdate*/ 505,
                       &cb, sizeof(cb));
}

__attribute__((constructor))
void register_lobby_data_observer() {
    wn_cm_bridge_register_lobby_data_observer(&on_lobby_data_event);
}

// Reactive lobby chat observer — fires on every CMsgClientMMSLobby
// ChatMsg push. Appends to pushed().lobby_chat_buffer[lobby] (bounded
// at 1024 entries — drop oldest when over) and emits LobbyChatMsg_t
// (callback 507) with the chat-id pointing at the new entry's
// position so the game's follow-up GetLobbyChatEntry succeeds.
void on_lobby_chat_msg_event(uint64_t lobby_sid, uint64_t sender_sid,
                              const uint8_t* data, size_t n) {
    if (lobby_sid == 0) return;
    uint32_t chat_id = 0;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto& ring = p.lobby_chat_buffer[lobby_sid];
        if (ring.size() >= 1024) ring.erase(ring.begin());
        lsc::PushedState::LobbyChatEntry e;
        e.sender_sid = sender_sid;
        e.chat_type  = 1; // EChatEntryType::ChatMsg
        if (data && n > 0) e.body.assign(data, data + n);
        ring.push_back(std::move(e));
        chat_id = static_cast<uint32_t>(ring.size() - 1);
    }
    // LobbyChatMsg_t (kLobbyChatMsg = 507). Layout:
    //   uint64 m_ulSteamIDLobby
    //   uint64 m_ulSteamIDUser
    //   uint8  m_eChatEntryType  (1 = ChatMsg)
    //   pad x3
    //   uint32 m_iChatID
    struct LobbyChatMsg {
        uint64_t lobby;
        uint64_t user;
        uint8_t  chat_type;
        uint8_t  _pad[3];
        uint32_t chat_id;
    };
    LobbyChatMsg cb{};
    cb.lobby     = lobby_sid;
    cb.user      = sender_sid;
    cb.chat_type = 1;
    cb.chat_id   = chat_id;
    lsc::push_callback(lsc::state().user.load(), /*kLobbyChatMsg*/ 507,
                       &cb, sizeof(cb));
}

__attribute__((constructor))
void register_lobby_chat_msg_observer() {
    wn_cm_bridge_register_lobby_chat_msg_observer(&on_lobby_chat_msg_event);
}

// Reactive membership observer — fires on UserJoinedLobby (6619) /
// UserLeftLobby (6620) pushes. Updates pushed().active_lobbies[sid]
// .members map + emits LobbyChatUpdate_t (callback 506).
void on_lobby_membership_event(int32_t joined,
                                uint64_t lobby_sid,
                                uint64_t user_sid,
                                const char* persona_name) {
    if (lobby_sid == 0 || user_sid == 0) return;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto& L = p.active_lobbies[lobby_sid];
        if (joined) {
            auto& mb = L.members[user_sid];
            if (persona_name) mb.persona_name = persona_name;
        } else {
            L.members.erase(user_sid);
        }
    }
    // LobbyChatUpdate_t (kLobbyChatUpdate = 506). Layout (32B):
    //   uint64 m_ulSteamIDLobby
    //   uint64 m_ulSteamIDUserChanged
    //   uint64 m_ulSteamIDMakingChange  (self-leave: == user_changed)
    //   uint32 m_rgfChatMemberStateChange  (bitmask)
    //   uint32 pad
    // EChatMemberStateChange: Entered=0x1, Left=0x2, Disconnected=0x4,
    // Kicked=0x8, Banned=0x10.
    struct LobbyChatUpdate {
        uint64_t lobby;
        uint64_t user_changed;
        uint64_t making_change;
        uint32_t state_change;
        uint32_t _pad;
    };
    LobbyChatUpdate cb{};
    cb.lobby         = lobby_sid;
    cb.user_changed  = user_sid;
    cb.making_change = user_sid;  // best effort — we don't know admin
    cb.state_change  = joined ? 0x1u : 0x2u;
    lsc::push_callback(lsc::state().user.load(), /*kLobbyChatUpdate*/ 506,
                       &cb, sizeof(cb));
}

__attribute__((constructor))
void register_lobby_membership_observer() {
    wn_cm_bridge_register_lobby_membership_observer(&on_lobby_membership_event);
}

// Reactive server-real-time observer — invoked from CMClient on a
// ClientLogonResponse(ok) carrying rtime32_server_time. Captures the
// CM-supplied epoch alongside a local steady_clock anchor so
// ISteamUtils.GetServerRealTime can advance the clock between logons.
void on_server_realtime_event(uint32_t server_realtime) {
    if (server_realtime == 0) return;
    auto& p = lsc::pushed();
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    p.server_realtime.store(server_realtime);
    p.server_realtime_anchor_local_ms.store(static_cast<int64_t>(now_ms));
    __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
        "server-realtime observer: %u (anchored at local %lld ms)",
        server_realtime, static_cast<long long>(now_ms));
}

__attribute__((constructor))
void register_server_realtime_observer() {
    wn_cm_bridge_register_server_realtime_observer(&on_server_realtime_event);
}
}  // namespace

#define WN_TAG "WnLibSteamClient"
#define WN_LOGI(...) __android_log_print(ANDROID_LOG_INFO, WN_TAG, __VA_ARGS__)

namespace {
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    if (!c) return {};
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}
}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetSteamId(
        JNIEnv* /*env*/, jclass /*cls*/, jlong steamId64) {
    auto& p = lsc::pushed();
    p.steam_id.store(static_cast<uint64_t>(steamId64));
    p.account_id.store(static_cast<uint32_t>(static_cast<uint64_t>(steamId64) & 0xFFFFFFFFu));
    WN_LOGI("set_steam_id(%llu)",
            static_cast<unsigned long long>(steamId64));
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetLoggedOn(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean loggedOn) {
    bool now = (loggedOn == JNI_TRUE);
    bool prev = lsc::state().logged_on.load();
    // set_logged_on idempotency means double-fires across paths
    // (LogonWithRefreshToken vtable + Kotlin setLoggedOn) are safe.
    lsc::set_logged_on(now);
    WN_LOGI("set_logged_on(%d) prev=%d emitted_cb=%d",
            now ? 1 : 0, prev ? 1 : 0, (now != prev) ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetPersonaName(
        JNIEnv* env, jclass /*cls*/, jstring jname) {
    auto& p = lsc::pushed();
    std::string name = jstr(env, jname);
    uint64_t self;
    bool changed;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        changed = (p.persona_name != name);
        p.persona_name = std::move(name);
        self = p.steam_id.load();
    }
    if (changed) {
        emit_persona_state_change(self, cb::kPersonaChangeName);
    }
    WN_LOGI("set_persona_name(\"%s\") changed=%d", p.persona_name.c_str(),
            changed ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetPersonaState(
        JNIEnv* /*env*/, jclass /*cls*/, jint state) {
    auto& p = lsc::pushed();
    int prev = p.persona_state.exchange(static_cast<int>(state));
    if (prev == state) return;
    int32_t flags = cb::kPersonaChangeStatus;
    // 0 = Offline, 1 = Online (per EPersonaState). Synthesize the
    // come-online / gone-offline flag whenever we cross that boundary
    // — Steam's overlay listens specifically for these.
    if (prev == 0 && state != 0) flags |= cb::kPersonaChangeComeOnline;
    if (prev != 0 && state == 0) flags |= cb::kPersonaChangeGoneOffline;
    emit_persona_state_change(p.steam_id.load(), flags);
    // Broadcast the change to Steam friends via the wn-steam-client CM
    // bridge. Best-effort — when no active CMClient is registered
    // (cold boot, signed-out, suspended for bionic), the call returns
    // false and we just keep the local cache update. When a CMClient
    // is alive, this fires CMsgClientChangeStatus → CMsgClientPersonaState
    // back from Steam → emits PersonaStateChange_t for all friends.
    wn_cm_set_persona_state(state);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppId(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    uint32_t app = static_cast<uint32_t>(appId);
    uint32_t prev = lsc::pushed().app_id.exchange(app);
    if (prev == app) return;
    // On transition (including bind 0→N, switch N→M, clear N→0), tell
    // Steam the user's now-running game. Friends overlay then shows
    // "Playing X" / "In game" instead of plain "Online". Best-effort —
    // no-op when no active CMClient.
    wn_cm_notify_games_played(app);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetIPCountry(
        JNIEnv* env, jclass /*cls*/, jstring jcc) {
    auto& p = lsc::pushed();
    std::string cc = jstr(env, jcc);
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.ip_country = std::move(cc);
    p.ip_country_set.store(1);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetUiLanguage(
        JNIEnv* env, jclass /*cls*/, jstring jlang) {
    auto& p = lsc::pushed();
    std::string lang = jstr(env, jlang);
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.ui_language = std::move(lang);
}

// Pushes the user's owned AppIDs (from Steam license expansion). Powers
// ISteamApps.BIsSubscribedApp. Replaces the entire set on each call —
// callers should push the COMPLETE list every time so we don't keep
// stale entries.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetOwnedApps(
        JNIEnv* env, jclass /*cls*/, jintArray appIds) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.owned_apps.clear();
    if (!appIds) return;
    jsize n = env->GetArrayLength(appIds);
    if (n <= 0) return;
    jint* arr = env->GetIntArrayElements(appIds, nullptr);
    if (!arr) return;
    p.owned_apps.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (arr[i] > 0) p.owned_apps.insert(static_cast<uint32_t>(arr[i]));
    }
    env->ReleaseIntArrayElements(appIds, arr, JNI_ABORT);
    WN_LOGI("set_owned_apps: %zu entries", p.owned_apps.size());
}

// Pushes the user's locally-installed AppIDs (from our DownloadService
// / Room install registry). Powers ISteamApps.BIsAppInstalled.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetInstalledApps(
        JNIEnv* env, jclass /*cls*/, jintArray appIds) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.installed_apps.clear();
    if (!appIds) return;
    jsize n = env->GetArrayLength(appIds);
    if (n <= 0) return;
    jint* arr = env->GetIntArrayElements(appIds, nullptr);
    if (!arr) return;
    p.installed_apps.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (arr[i] > 0) p.installed_apps.insert(static_cast<uint32_t>(arr[i]));
    }
    env->ReleaseIntArrayElements(appIds, arr, JNI_ABORT);
    WN_LOGI("set_installed_apps: %zu entries", p.installed_apps.size());
}

// Sets the install-dir path for a specific AppID. Powers
// ISteamApps.GetAppInstallDir(appId, buf, n). Called once per app.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppInstallDir(
        JNIEnv* env, jclass /*cls*/, jint appId, jstring jdir) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::string dir = jstr(env, jdir);
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (dir.empty()) {
        p.app_install_dirs.erase(static_cast<uint32_t>(appId));
    } else {
        p.app_install_dirs[static_cast<uint32_t>(appId)] = std::move(dir);
    }
}

// Pushes the friends list (long[] of SteamID64) for ISteamFriends
// queries. Replaces the entire list each call.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendsList(
        JNIEnv* env, jclass /*cls*/, jlongArray steamIds) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.friends.clear();
    if (!steamIds) return;
    jsize n = env->GetArrayLength(steamIds);
    if (n <= 0) return;
    jlong* arr = env->GetLongArrayElements(steamIds, nullptr);
    if (!arr) return;
    p.friends.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (arr[i] != 0) p.friends.push_back(static_cast<uint64_t>(arr[i]));
    }
    env->ReleaseLongArrayElements(steamIds, arr, JNI_ABORT);
    WN_LOGI("set_friends_list: %zu entries", p.friends.size());
}

// Push the per-app PICS public-branch buildid. Powers ISteamApps
// .GetAppBuildId (slot 23). appId <= 0 / buildId == 0 clears.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppBuildId(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId, jint buildId) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (buildId <= 0) {
        p.app_build_ids.erase(static_cast<uint32_t>(appId));
    } else {
        p.app_build_ids[static_cast<uint32_t>(appId)] =
            static_cast<uint32_t>(buildId);
    }
}

// Push per-app human-readable names in bulk. appIds[i] paired with
// names[i]. Replaces / inserts each entry (does NOT clear other
// entries). Empty name clears that appId. Powers ISteamAppList
// .GetAppName (slot 2).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppNames(
        JNIEnv* env, jclass /*cls*/, jintArray appIds, jobjectArray names) {
    if (!appIds || !names) return;
    jsize n = env->GetArrayLength(appIds);
    if (n <= 0 || env->GetArrayLength(names) != n) return;
    jint* ids = env->GetIntArrayElements(appIds, nullptr);
    if (!ids) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    size_t set_count = 0, clear_count = 0;
    for (jsize i = 0; i < n; ++i) {
        if (ids[i] <= 0) continue;
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(names, i));
        if (!js) {
            p.app_names.erase(static_cast<uint32_t>(ids[i]));
            ++clear_count;
            continue;
        }
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c && *c) {
            p.app_names[static_cast<uint32_t>(ids[i])] = c;
            ++set_count;
        } else {
            p.app_names.erase(static_cast<uint32_t>(ids[i]));
            ++clear_count;
        }
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    }
    env->ReleaseIntArrayElements(appIds, ids, JNI_ABORT);
    WN_LOGI("set_app_names: set=%zu clear=%zu total=%zu",
            set_count, clear_count, p.app_names.size());
}

// Forward-declare singleton accessors so the cluster of probes below
// can resolve them; the canonical declarations live further down (and
// in isteam_client.cpp) — this is just a local repeat to give the
// linker something to bind.
extern "C" void* wn_get_isteam_apps();
extern "C" void* wn_get_isteam_remote_storage();
extern "C" void* wn_get_isteam_user();
extern "C" void* wn_get_isteam_friends();
extern "C" void* wn_get_isteam_utils();

// Diagnostic — synthetically dispatch the cm_bridge account-info
// observer with a 4-bool payload. Verifies the observer registration +
// pushed-state mirror path offline without needing CM round-trip.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectAccountInfo(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean twoFA, jboolean phoneV,
        jboolean phoneId, jboolean phoneNV) {
    WnCmAccountInfo info{};
    info.two_factor_enabled = twoFA == JNI_TRUE;
    info.phone_verified     = phoneV == JNI_TRUE;
    info.phone_identifying  = phoneId == JNI_TRUE;
    info.phone_requires_verification = phoneNV == JNI_TRUE;
    wn_cm_bridge_inject_test_account_info(&info);
}

// Push account-info flags from CMsgClientAccountInfo ingest. flagKind:
// 0=phone_verified, 1=two_factor_enabled, 2=phone_identifying,
// 3=phone_requires_verification. Powers ISteamUser slots 26-29.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAccountFlag(
        JNIEnv* /*env*/, jclass /*cls*/, jint flagKind, jboolean on) {
    auto& p = lsc::pushed();
    switch (flagKind) {
        case 0: p.account_phone_verified.store(on); break;
        case 1: p.account_two_factor_enabled.store(on); break;
        case 2: p.account_phone_identifying.store(on); break;
        case 3: p.account_phone_requires_verification.store(on); break;
        default: break;
    }
}
// Diagnostic — invoke ISteamUser slots 26/27/28/29 via vtable.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticUserBool(
        JNIEnv* /*env*/, jclass /*cls*/, jint slot) {
    if (slot < 26 || slot > 29) return JNI_FALSE;
    void* obj = wn_get_isteam_user();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[slot]);
    return fn(obj) ? JNI_TRUE : JNI_FALSE;
}
// Push a user-set nickname for an account. nickname=null/empty erases.
// Powers ISteamFriends.GetPlayerNickname (slot 11).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetPlayerNickname(
        JNIEnv* env, jclass /*cls*/, jlong sid, jstring jNickname) {
    if (sid == 0) return;
    auto& p = lsc::pushed();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        const uint64_t key = static_cast<uint64_t>(sid);
        if (!jNickname) {
            changed = (p.player_nicknames.erase(key) > 0);
        } else {
            const char* c = env->GetStringUTFChars(jNickname, nullptr);
            if (!c || *c == '\0') {
                if (c) env->ReleaseStringUTFChars(jNickname, c);
                changed = (p.player_nicknames.erase(key) > 0);
            } else {
                std::string newName(c);
                env->ReleaseStringUTFChars(jNickname, c);
                auto it = p.player_nicknames.find(key);
                if (it == p.player_nicknames.end()) {
                    p.player_nicknames[key] = std::move(newName);
                    changed = true;
                } else if (it->second != newName) {
                    it->second = std::move(newName);
                    changed = true;
                }
            }
        }
    }
    // SDK contract: emit PersonaStateChange_t with kPersonaChangeNickname
    // so the Steam Overlay friends-list refreshes the nickname column +
    // any game-side observer of friend-display changes sees the update.
    // Only fire when something actually changed (dedup re-set of same
    // nickname). Emitted OUTSIDE the state_mutex to avoid nested-lock
    // risk in the callback queue.
    if (changed) {
        emit_persona_state_change(static_cast<uint64_t>(sid),
                                  cb::kPersonaChangeNickname);
    }
}
// Diagnostic — invokes GetPlayerNickname (slot 11). Returns the
// resolved nickname (or null).
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetPlayerNickname(
        JNIEnv* env, jclass /*cls*/, jlong sid) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return nullptr;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = const char* (*)(void*, uint64_t);
    auto fn = reinterpret_cast<Fn>(vt[11]);
    const char* nick = fn(obj, static_cast<uint64_t>(sid));
    return nick ? env->NewStringUTF(nick) : nullptr;
}

// Diagnostic — invokes ISteamUtils::CheckFileSignature (slot 19).
// Returns the hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCheckFileSignature(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_utils();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[19]);
    const char* c = jName ? env->GetStringUTFChars(jName, nullptr) : nullptr;
    uint64_t h = fn(obj, c);
    if (c) env->ReleaseStringUTFChars(jName, c);
    return static_cast<jlong>(h);
}

// Direct pushed-state getters — read what libsteamclient.so itself
// would return for the corresponding slot. The legacy state-dump op
// reads WnSteamBootstrap.{steamId,personaName,...} which only fire
// when the bootstrap dlopen path is active. With Hybrid mode retired
// (libsteamclient.so is the sole client), those bootstrap reads
// always return 0 — making the diagnostic useless. These getters
// read pushed-state directly so the state-dump reflects reality.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedSteamId(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jlong>(lsc::pushed().steam_id.load());
}
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedPersonaName(
        JNIEnv* env, jclass /*cls*/) {
    std::string name;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        name = lsc::pushed().persona_name;
    }
    return env->NewStringUTF(name.c_str());
}
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedIpCountry(
        JNIEnv* env, jclass /*cls*/) {
    std::string c;
    if (lsc::pushed().ip_country_set.load() != 0) {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        c = lsc::pushed().ip_country;
    }
    return env->NewStringUTF(c.c_str());
}
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedUiLanguage(
        JNIEnv* env, jclass /*cls*/) {
    std::string s;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        s = lsc::pushed().ui_language;
    }
    return env->NewStringUTF(s.c_str());
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedServerRealTime(
        JNIEnv* /*env*/, jclass /*cls*/) {
    auto anchor   = lsc::pushed().server_realtime.load();
    auto anchor_local_ms = lsc::pushed().server_realtime_anchor_local_ms.load();
    if (anchor == 0 || anchor_local_ms == 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    auto elapsed_s = (now_ms - anchor_local_ms) / 1000;
    if (elapsed_s < 0) elapsed_s = 0;
    return static_cast<jint>(anchor + static_cast<uint32_t>(elapsed_s));
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedPersonaState(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(lsc::pushed().persona_state.load());
}
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedLoggedOn(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return lsc::state().logged_on.load() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedAppId(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(lsc::pushed().app_id.load());
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedOwnedAppCount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return static_cast<jint>(lsc::pushed().owned_apps.size());
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedInstalledAppCount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return static_cast<jint>(lsc::pushed().installed_apps.size());
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedFriendCount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return static_cast<jint>(lsc::pushed().friends.size());
}
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedFirstFriend(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto& fs = lsc::pushed().friends;
    return fs.empty() ? 0L : static_cast<jlong>(fs.front());
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedCloudFileCount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return static_cast<jint>(lsc::pushed().cloud_files.size());
}
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedCloudEnabledAccount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return lsc::pushed().cloud_enabled_account.load() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedCloudEnabledApp(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return lsc::pushed().cloud_enabled_app.load() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeGetPushedEncryptedAppTicketSize(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    if (appId <= 0) return 0;
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto it = lsc::pushed().encrypted_app_tickets.find(
        static_cast<uint32_t>(appId));
    if (it == lsc::pushed().encrypted_app_tickets.end()) return 0;
    return static_cast<jint>(it->second.size());
}

// Diagnostic — invokes FileShare (slot 7). Returns hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileShare(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[7]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    uint64_t h = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return static_cast<jlong>(h);
}
// Diagnostic — invokes ISteamApps::GetFileDetails (slot 25). Note this
// is in ISteamApps for binary-integrity checks, not ISteamRemoteStorage.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticAppsGetFileDetails(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_apps();
    if (!obj || !jName) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[25]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    uint64_t h = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return static_cast<jlong>(h);
}

// Push the self Steam profile XP level. level<0 clears (back to 0).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetSelfPlayerLevel(
        JNIEnv* /*env*/, jclass /*cls*/, jint level) {
    lsc::pushed().self_player_level.store(level < 0 ? 0 : level);
}
// Push a self game-badge tier. nSeries + bFoil are part of the key so
// the same app can have multiple badges. tier<0 clears.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetSelfGameBadge(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId, jint nSeries,
        jboolean bFoil, jint tier) {
    if (appId <= 0) return;
    int32_t key = (static_cast<int32_t>(appId) & 0x0FFFFFFF)
                | ((nSeries & 0x07) << 28)
                | (bFoil ? (1 << 31) : 0);
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (tier < 0) p.self_game_badges.erase(key);
    else          p.self_game_badges[key] = tier;
}
// Diagnostic — GetPlayerSteamLevel (ISteamUser slot 24).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetPlayerSteamLevel(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[24]);
    return fn(obj);
}
// Diagnostic — GetGameBadgeLevel (ISteamUser slot 23).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetGameBadgeLevel(
        JNIEnv* /*env*/, jclass /*cls*/, jint nSeries, jboolean bFoil) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*, int, bool);
    auto fn = reinterpret_cast<Fn>(vt[23]);
    return fn(obj, nSeries, bFoil ? true : false);
}
// Diagnostic — RequestStoreAuthURL (slot 25). Returns hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRequestStoreAuthURL(
        JNIEnv* env, jclass /*cls*/, jstring jRedirect) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[25]);
    const char* c = jRedirect ? env->GetStringUTFChars(jRedirect, nullptr) : nullptr;
    uint64_t h = fn(obj, c);
    if (c) env->ReleaseStringUTFChars(jRedirect, c);
    return static_cast<jlong>(h);
}
// Diagnostic — GetMarketEligibility (slot 30). Returns hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetMarketEligibility(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[30]);
    return static_cast<jlong>(fn(obj));
}
// Diagnostic — GetDurationControl (slot 31). Returns hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetDurationControl(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[31]);
    return static_cast<jlong>(fn(obj));
}

// Push a per-friend Steam profile XP level. level<0 erases the entry
// (back to "unknown"). Powers ISteamFriends.GetFriendSteamLevel.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendSteamLevel(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sid, jint level) {
    if (sid == 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (level < 0) p.friend_steam_levels.erase(static_cast<uint64_t>(sid));
    else           p.friend_steam_levels[static_cast<uint64_t>(sid)] = level;
}
// Read whether an app has been marked content-corrupt. Used by the
// wn-session depot-downloader to poll for revalidation requests.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeIsAppMarkedCorrupt(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    if (appId <= 0) return JNI_FALSE;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return p.apps_marked_corrupt.count(static_cast<uint32_t>(appId)) > 0
           ? JNI_TRUE : JNI_FALSE;
}
// Clear the corrupt flag for an app — called by the downloader once a
// revalidation pass has been scheduled or completed.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeClearAppCorruptFlag(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.apps_marked_corrupt.erase(static_cast<uint32_t>(appId));
}
// Diagnostic — invokes ISteamUser::UserHasLicenseForApp (slot 18).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticUserHasLicense(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sid, jint appId) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 2;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*, uint64_t, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[18]);
    return fn(obj, static_cast<uint64_t>(sid), static_cast<uint32_t>(appId));
}
// Diagnostic — invokes ISteamApps::MarkContentCorrupt (slot 16).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticMarkContentCorrupt(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean missingOnly) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, bool);
    auto fn = reinterpret_cast<Fn>(vt[16]);
    return fn(obj, missingOnly ? true : false) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — invokes ISteamFriends::GetFriendSteamLevel (slot 10).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetFriendSteamLevel(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sid) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*, uint64_t);
    auto fn = reinterpret_cast<Fn>(vt[10]);
    return fn(obj, static_cast<uint64_t>(sid));
}

// Diagnostic — invokes ISteamUser::GetAuthTicketForWebApi (slot 14).
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetAuthTicketForWebApi(
        JNIEnv* env, jclass /*cls*/, jstring jIdentity) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[14]);
    const char* c = jIdentity ? env->GetStringUTFChars(jIdentity, nullptr) : nullptr;
    uint64_t h = fn(obj, c);
    if (c) env->ReleaseStringUTFChars(jIdentity, c);
    return static_cast<jlong>(h);
}
// Diagnostic — invokes ISteamFriends::GetFriendRelationship (slot 5).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetFriendRelationship(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sid) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*, uint64_t);
    auto fn = reinterpret_cast<Fn>(vt[5]);
    return fn(obj, static_cast<uint64_t>(sid));
}
// Diagnostic — invokes ISteamFriends::HasFriend (slot 17).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticHasFriend(
        JNIEnv* /*env*/, jclass /*cls*/, jlong sid, jint flags) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint64_t, int);
    auto fn = reinterpret_cast<Fn>(vt[17]);
    return fn(obj, static_cast<uint64_t>(sid), flags) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — invokes ISteamUser::GetUserDataFolder (slot 6). Returns
// the resolved folder name (or null on failure).
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetUserDataFolder(
        JNIEnv* env, jclass /*cls*/) {
    void* obj = wn_get_isteam_user();
    if (!obj) return nullptr;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, char*, int);
    auto fn = reinterpret_cast<Fn>(vt[6]);
    char buf[512];
    buf[0] = '\0';
    if (!fn(obj, buf, sizeof(buf))) return nullptr;
    return env->NewStringUTF(buf);
}
// Diagnostic — invoke BSetDurationControlOnlineState (slot 32) via
// vtable. Always true with our wiring.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticSetDurationControl(
        JNIEnv* /*env*/, jclass /*cls*/, jint state) {
    void* obj = wn_get_isteam_user();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, int);
    auto fn = reinterpret_cast<Fn>(vt[32]);
    return fn(obj, state) ? JNI_TRUE : JNI_FALSE;
}

// Toggle per-app boolean flags. flagKind=0 → app_low_violence,
// flagKind=1 → app_vac_banned. on=true inserts; on=false erases. Used
// at logon/PICS-ingest time. Powers ISteamApps slots 1 + 3.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppFlag(
        JNIEnv* /*env*/, jclass /*cls*/, jint flagKind, jint appId, jboolean on) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto& set = (flagKind == 0) ? p.app_low_violence
                                 : p.app_vac_banned;
    if (on) set.insert(static_cast<uint32_t>(appId));
    else    set.erase(static_cast<uint32_t>(appId));
}

// Diagnostic — invoke ISteamApps slots 0/1/2/3 via vtable.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticAppsBool(
        JNIEnv* /*env*/, jclass /*cls*/, jint slot) {
    void* obj = wn_get_isteam_apps();
    if (!obj || slot < 0 || slot > 3) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[slot]);
    return fn(obj) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — invoke SetDlcContext (slot 29) via vtable.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticSetDlcContext(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[29]);
    return fn(obj, static_cast<uint32_t>(appId)) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — FileForget (slot 5) on ISteamRemoteStorage.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileForget(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[5]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    bool ok = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — FilePersisted (slot 14) on ISteamRemoteStorage.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFilePersisted(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[14]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    bool ok = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Push the local "remote" directory path for an app (= the dir Steam
// mirrors cloud files into; e.g. .../userdata/<acct>/<appId>/remote).
// Empty / null path clears the entry. Powers ISteamRemoteStorage
// FileWrite/FileRead/FileDelete — without this push slots 0/1/6
// return false / 0.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppCloudRemoteDir(
        JNIEnv* env, jclass /*cls*/, jint appId, jstring jPath) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!jPath) {
        p.app_cloud_remote_dirs.erase(static_cast<uint32_t>(appId));
        return;
    }
    const char* c = env->GetStringUTFChars(jPath, nullptr);
    if (!c || *c == '\0') {
        if (c) env->ReleaseStringUTFChars(jPath, c);
        p.app_cloud_remote_dirs.erase(static_cast<uint32_t>(appId));
        return;
    }
    p.app_cloud_remote_dirs[static_cast<uint32_t>(appId)] = std::string(c);
    env->ReleaseStringUTFChars(jPath, c);
}

// Push the active beta branch name for an app. null/empty branch
// clears the entry (treated as "public" branch). Powers
// ISteamApps.GetCurrentBetaName (slot 15).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppCurrentBeta(
        JNIEnv* env, jclass /*cls*/, jint appId, jstring jBranch) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!jBranch) {
        p.app_current_beta.erase(static_cast<uint32_t>(appId));
        return;
    }
    const char* c = env->GetStringUTFChars(jBranch, nullptr);
    if (!c || *c == '\0') {
        if (c) env->ReleaseStringUTFChars(jBranch, c);
        p.app_current_beta.erase(static_cast<uint32_t>(appId));
        return;
    }
    p.app_current_beta[static_cast<uint32_t>(appId)] = std::string(c);
    env->ReleaseStringUTFChars(jBranch, c);
}

// Forward-declare the apps singleton accessor — defined in
// isteam_client.cpp; re-declared at line ~1525 of this file for the
// diagnostic cluster, this earlier copy lets the slot-15 diagnostic
// nest near its setter without reordering the file.
extern "C" void* wn_get_isteam_apps();

extern "C" void* wn_get_isteam_remote_storage();
// Diagnostic — invokes ISteamRemoteStorage::FileWrite (slot 0) on the
// bound app's remote dir. Caller is responsible for setAppId binding.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileWrite(
        JNIEnv* env, jclass /*cls*/, jstring jName, jbyteArray jData) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName || !jData) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, const char*, const void*, int);
    auto fn = reinterpret_cast<Fn>(vt[0]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    jsize len = env->GetArrayLength(jData);
    jbyte* buf = env->GetByteArrayElements(jData, nullptr);
    bool ok = fn(obj, name, buf, static_cast<int>(len));
    env->ReleaseByteArrayElements(jData, buf, JNI_ABORT);
    env->ReleaseStringUTFChars(jName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — invokes ISteamRemoteStorage::FileRead (slot 1). Returns
// the bytes read (truncated to maxBytes); null on read failure.
JNIEXPORT jbyteArray JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileRead(
        JNIEnv* env, jclass /*cls*/, jstring jName, jint maxBytes) {
    if (!jName || maxBytes <= 0) return nullptr;
    void* obj = wn_get_isteam_remote_storage();
    if (!obj) return nullptr;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = int (*)(void*, const char*, void*, int);
    auto fn = reinterpret_cast<Fn>(vt[1]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    std::vector<char> buf(static_cast<size_t>(maxBytes));
    int n = fn(obj, name, buf.data(), maxBytes);
    env->ReleaseStringUTFChars(jName, name);
    if (n <= 0) return nullptr;
    jbyteArray out = env->NewByteArray(n);
    env->SetByteArrayRegion(out, 0, n, reinterpret_cast<jbyte*>(buf.data()));
    return out;
}
// Diagnostic — FileWriteStreamOpen (slot 9). Returns stream handle.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudStreamOpen(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[9]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    uint64_t h = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return static_cast<jlong>(h);
}
// Diagnostic — FileWriteStreamWriteChunk (slot 10).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudStreamWriteChunk(
        JNIEnv* env, jclass /*cls*/, jlong hStream, jbyteArray jData) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jData) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint64_t, const void*, int);
    auto fn = reinterpret_cast<Fn>(vt[10]);
    jsize len = env->GetArrayLength(jData);
    jbyte* buf = env->GetByteArrayElements(jData, nullptr);
    bool ok = fn(obj, static_cast<uint64_t>(hStream), buf, static_cast<int>(len));
    env->ReleaseByteArrayElements(jData, buf, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — FileWriteStreamClose (slot 11).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudStreamClose(
        JNIEnv* /*env*/, jclass /*cls*/, jlong hStream) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint64_t);
    auto fn = reinterpret_cast<Fn>(vt[11]);
    return fn(obj, static_cast<uint64_t>(hStream)) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — FileWriteStreamCancel (slot 12).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudStreamCancel(
        JNIEnv* /*env*/, jclass /*cls*/, jlong hStream) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint64_t);
    auto fn = reinterpret_cast<Fn>(vt[12]);
    return fn(obj, static_cast<uint64_t>(hStream)) ? JNI_TRUE : JNI_FALSE;
}
// Diagnostic — invokes FileWriteAsync (slot 2). Returns the hCall
// (uint64) allocated for the async op (0 on resolve / write failure).
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileWriteAsync(
        JNIEnv* env, jclass /*cls*/, jstring jName, jbyteArray jData) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName || !jData) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*, const void*, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[2]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    jsize len = env->GetArrayLength(jData);
    jbyte* buf = env->GetByteArrayElements(jData, nullptr);
    uint64_t h = fn(obj, name, buf, static_cast<uint32_t>(len));
    env->ReleaseByteArrayElements(jData, buf, JNI_ABORT);
    env->ReleaseStringUTFChars(jName, name);
    return static_cast<jlong>(h);
}
// Diagnostic — invokes FileReadAsync (slot 3). Returns the hCall.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileReadAsync(
        JNIEnv* env, jclass /*cls*/, jstring jName, jint nOffset, jint cubToRead) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName || cubToRead <= 0) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*, const char*, uint32_t, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[3]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    uint64_t h = fn(obj, name, static_cast<uint32_t>(nOffset),
                   static_cast<uint32_t>(cubToRead));
    env->ReleaseStringUTFChars(jName, name);
    return static_cast<jlong>(h);
}
// Diagnostic — invokes FileReadAsyncComplete (slot 4). Returns the
// buffered bytes on success, null on unknown handle / oversize buffer.
JNIEXPORT jbyteArray JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileReadAsyncComplete(
        JNIEnv* env, jclass /*cls*/, jlong hCall, jint cubToRead) {
    if (hCall == 0 || cubToRead <= 0) return nullptr;
    void* obj = wn_get_isteam_remote_storage();
    if (!obj) return nullptr;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint64_t, void*, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[4]);
    std::vector<char> buf(static_cast<size_t>(cubToRead));
    bool ok = fn(obj, static_cast<uint64_t>(hCall), buf.data(),
                 static_cast<uint32_t>(cubToRead));
    if (!ok) return nullptr;
    jbyteArray out = env->NewByteArray(cubToRead);
    env->SetByteArrayRegion(out, 0, cubToRead, reinterpret_cast<jbyte*>(buf.data()));
    return out;
}
// Diagnostic — invokes ISteamRemoteStorage::FileDelete (slot 6).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCloudFileDelete(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    void* obj = wn_get_isteam_remote_storage();
    if (!obj || !jName) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, const char*);
    auto fn = reinterpret_cast<Fn>(vt[6]);
    const char* name = env->GetStringUTFChars(jName, nullptr);
    bool ok = fn(obj, name);
    env->ReleaseStringUTFChars(jName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamApps::GetCurrentBetaName (slot 15) and
// returns the written branch name (or null on false / empty).
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetCurrentBetaName(
        JNIEnv* env, jclass /*cls*/) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return nullptr;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, char*, int);
    auto fn = reinterpret_cast<Fn>(vt[15]);
    char buf[128];
    buf[0] = '\0';
    if (!fn(obj, buf, sizeof(buf))) return nullptr;
    return env->NewStringUTF(buf);
}

// Push the per-app active download progress. bytesTotal == 0 clears
// the entry (download terminated). Called by SteamService during
// active depot-downloader sessions. Powers
// ISteamApps.GetDlcDownloadProgress (slot 22).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppDownloadProgress(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId,
        jlong bytesDownloaded, jlong bytesTotal) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (bytesTotal <= 0) {
        p.app_dl_progress.erase(static_cast<uint32_t>(appId));
        return;
    }
    auto& e = p.app_dl_progress[static_cast<uint32_t>(appId)];
    e.bytes_downloaded = static_cast<uint64_t>(std::max<jlong>(0, bytesDownloaded));
    e.bytes_total      = static_cast<uint64_t>(bytesTotal);
}

// Diagnostic — invokes ISteamApps::GetDlcDownloadProgress (slot 22) and
// returns the result packed as a long: bit 63 = true marker; bits 0-62
// otherwise zero. Out-params are read back via separate getters since
// JNI can't return multiple longs cleanly — but in practice the probe
// just verifies true/false based on injected state, and the result
// returns a packed downloaded/total pair via the high-order encoding.
// We use a simpler 2-call shape: call returns true/false, then read the
// counters from a sticky out-buffer.
static uint64_t s_diag_dl_downloaded = 0;
static uint64_t s_diag_dl_total      = 0;
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetDlcDownloadProgress(
        JNIEnv* /*env*/, jclass /*cls*/, jint appId) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint32_t, uint64_t*, uint64_t*);
    auto fn = reinterpret_cast<Fn>(vt[22]);
    s_diag_dl_downloaded = 0;
    s_diag_dl_total      = 0;
    return fn(obj, static_cast<uint32_t>(appId),
              &s_diag_dl_downloaded, &s_diag_dl_total) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetDlcDownloadProgressBytes(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jlong>(s_diag_dl_downloaded);
}
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetDlcDownloadProgressTotal(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jlong>(s_diag_dl_total);
}

// Push the per-app installed depot list. Powers ISteamApps
// .GetInstalledDepots (slot 17). Replaces the entry for [appId];
// empty / null array clears it.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppInstalledDepots(
        JNIEnv* env, jclass /*cls*/, jint appId, jintArray depotIds) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!depotIds) {
        p.app_installed_depots.erase(static_cast<uint32_t>(appId));
        return;
    }
    jsize n = env->GetArrayLength(depotIds);
    if (n <= 0) {
        p.app_installed_depots.erase(static_cast<uint32_t>(appId));
        return;
    }
    jint* arr = env->GetIntArrayElements(depotIds, nullptr);
    std::vector<uint32_t> depots;
    depots.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (arr[i] > 0) depots.push_back(static_cast<uint32_t>(arr[i]));
    }
    env->ReleaseIntArrayElements(depotIds, arr, JNI_ABORT);
    p.app_installed_depots[static_cast<uint32_t>(appId)] = std::move(depots);
    WN_LOGI("set_app_installed_depots: app=%d count=%zu",
            appId, p.app_installed_depots[static_cast<uint32_t>(appId)].size());
}

// Push the per-app DLC list. Three parallel arrays must be the same
// length: dlcAppIds, dlcNames (UTF-8), available flags. Replaces the
// entire entry for parentAppId; empty / null arrays clear it.
// Powers ISteamApps.GetDLCCount + BGetDLCDataByIndex.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppDlcs(
        JNIEnv* env, jclass /*cls*/, jint parentAppId,
        jintArray dlcAppIds, jobjectArray dlcNames, jbooleanArray available) {
    if (parentAppId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!dlcAppIds) {
        p.app_dlcs.erase(static_cast<uint32_t>(parentAppId));
        return;
    }
    jsize n = env->GetArrayLength(dlcAppIds);
    if (n <= 0) {
        p.app_dlcs.erase(static_cast<uint32_t>(parentAppId));
        return;
    }
    jint*     ids = env->GetIntArrayElements(dlcAppIds, nullptr);
    jboolean* av  = (available && env->GetArrayLength(available) == n)
                        ? env->GetBooleanArrayElements(available, nullptr) : nullptr;
    auto read_name = [&](jsize i) -> std::string {
        if (!dlcNames || env->GetArrayLength(dlcNames) <= i) return {};
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(dlcNames, i));
        if (!js) return {};
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string out = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return out;
    };
    std::vector<wn_libsteamclient::PushedState::DlcEntry> entries;
    entries.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (ids[i] <= 0) continue;
        wn_libsteamclient::PushedState::DlcEntry e;
        e.app_id    = static_cast<uint32_t>(ids[i]);
        e.name      = read_name(i);
        e.available = av ? (av[i] == JNI_TRUE) : true;
        entries.push_back(std::move(e));
    }
    env->ReleaseIntArrayElements(dlcAppIds, ids, JNI_ABORT);
    if (av) env->ReleaseBooleanArrayElements(available, av, JNI_ABORT);
    p.app_dlcs[static_cast<uint32_t>(parentAppId)] = std::move(entries);
    WN_LOGI("set_app_dlcs: parent=%d count=%zu",
            parentAppId, p.app_dlcs[static_cast<uint32_t>(parentAppId)].size());
}

// Push the per-app subscribed-and-installed Workshop item list.
// Powers ISteamUGC slots 70-75 (GetNumSubscribedItems / GetSubscribed
// Items / GetItemState / GetItemInstallInfo / GetItemDownloadInfo /
// DownloadItem). All four arrays must be the same length —
// publishedFileIds, installDirs (UTF-8 absolute paths), sizesBytes,
// timestamps (unix32 secs). Replaces the entire entry for [appId];
// empty / null arrays clear it. SteamService writes this at game
// launch reading WorkshopModsGenerator's staging dir.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppWorkshopItems(
        JNIEnv* env, jclass /*cls*/, jint appId,
        jlongArray publishedFileIds, jobjectArray installDirs,
        jlongArray sizesBytes, jlongArray timestamps) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!publishedFileIds) {
        p.subscribed_workshop_items.erase(static_cast<uint32_t>(appId));
        return;
    }
    jsize n = env->GetArrayLength(publishedFileIds);
    if (n <= 0) {
        p.subscribed_workshop_items.erase(static_cast<uint32_t>(appId));
        return;
    }
    jlong* ids   = env->GetLongArrayElements(publishedFileIds, nullptr);
    jlong* sizes = (sizesBytes && env->GetArrayLength(sizesBytes) == n)
                       ? env->GetLongArrayElements(sizesBytes, nullptr) : nullptr;
    jlong* tims  = (timestamps && env->GetArrayLength(timestamps) == n)
                       ? env->GetLongArrayElements(timestamps, nullptr) : nullptr;
    auto read_str = [&](jsize i) -> std::string {
        if (!installDirs || env->GetArrayLength(installDirs) <= i) return {};
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(installDirs, i));
        if (!js) return {};
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string out = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return out;
    };
    std::unordered_map<uint64_t, wn_libsteamclient::PushedState::WorkshopItemInfo> items;
    items.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        if (ids[i] <= 0) continue;
        wn_libsteamclient::PushedState::WorkshopItemInfo info;
        info.install_dir = read_str(i);
        info.size_bytes  = sizes ? static_cast<uint64_t>(sizes[i]) : 0u;
        info.timestamp   = tims  ? static_cast<uint32_t>(tims[i]) : 0u;
        info.installed   = true;
        items.emplace(static_cast<uint64_t>(ids[i]), std::move(info));
    }
    env->ReleaseLongArrayElements(publishedFileIds, ids, JNI_ABORT);
    if (sizes) env->ReleaseLongArrayElements(sizesBytes, sizes, JNI_ABORT);
    if (tims)  env->ReleaseLongArrayElements(timestamps, tims,  JNI_ABORT);
    if (items.empty()) {
        p.subscribed_workshop_items.erase(static_cast<uint32_t>(appId));
    } else {
        p.subscribed_workshop_items[static_cast<uint32_t>(appId)] = std::move(items);
    }
    WN_LOGI("set_app_workshop_items: app=%d count=%zu", appId,
            p.subscribed_workshop_items[static_cast<uint32_t>(appId)].size());
}

// Push the per-app ISteamInventory item-definition catalog. Schema-flat
// batch transfer: defIds[N] paired with propCountsPerDef[N]; the
// per-def property entries are concatenated into propKeys / propVals
// in defIds order (sum of propCountsPerDef == propKeys.length ==
// propVals.length). Replaces the entire entry for [appId]; empty /
// null arrays clear it. Powers ISteamInventory slots 20-22.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetInventoryItemDefs(
        JNIEnv* env, jclass /*cls*/, jint appId,
        jintArray defIds, jintArray propCountsPerDef,
        jobjectArray propKeys, jobjectArray propVals) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!defIds) {
        p.inventory_item_defs.erase(static_cast<uint32_t>(appId));
        return;
    }
    jsize n = env->GetArrayLength(defIds);
    if (n <= 0) {
        p.inventory_item_defs.erase(static_cast<uint32_t>(appId));
        return;
    }
    if (!propCountsPerDef || env->GetArrayLength(propCountsPerDef) != n) return;
    jint* ids    = env->GetIntArrayElements(defIds, nullptr);
    jint* counts = env->GetIntArrayElements(propCountsPerDef, nullptr);
    auto read_str = [&](jobjectArray arr, jsize i) -> std::string {
        if (!arr || env->GetArrayLength(arr) <= i) return {};
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (!js) return {};
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string out = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return out;
    };
    std::unordered_map<int32_t, std::unordered_map<std::string, std::string>> table;
    table.reserve(n);
    jsize cursor = 0;
    for (jsize i = 0; i < n; ++i) {
        if (ids[i] <= 0) { cursor += counts[i]; continue; }
        std::unordered_map<std::string, std::string> props;
        const jsize end = cursor + counts[i];
        props.reserve(counts[i]);
        for (jsize j = cursor; j < end; ++j) {
            auto k = read_str(propKeys, j);
            if (k.empty()) continue;
            props.emplace(std::move(k), read_str(propVals, j));
        }
        cursor = end;
        table.emplace(static_cast<int32_t>(ids[i]), std::move(props));
    }
    env->ReleaseIntArrayElements(defIds, ids, JNI_ABORT);
    env->ReleaseIntArrayElements(propCountsPerDef, counts, JNI_ABORT);
    if (table.empty()) {
        p.inventory_item_defs.erase(static_cast<uint32_t>(appId));
    } else {
        p.inventory_item_defs[static_cast<uint32_t>(appId)] = std::move(table);
    }
    WN_LOGI("set_inventory_item_defs: app=%d count=%zu", appId,
            p.inventory_item_defs[static_cast<uint32_t>(appId)].size());
}

// Per-friend persona STATE + game_played_app_id. Surfaces through
// ISteamFriends.GetFriendPersonaState (slot 6) + GetFriendGamePlayed
// (slot 8). Pushed alongside setFriendPersonaName by SteamService's
// persona observer; the trio (name + state + app) lands together
// from a single CMsgClientPersonaState body. Each is no-op if
// steamId64 == 0.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendPersonaState(
        JNIEnv* /*env*/, jclass /*cls*/, jlong steamId64, jint state) {
    if (steamId64 == 0) return;
    int32_t flags = 0;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto sid = static_cast<uint64_t>(steamId64);
        if (state < 0) {
            // Negative = erase entry; treat as transition-to-Offline IF
            // we had a non-zero state cached (so games' overlay sees the
            // GoneOffline edge).
            auto it = p.friend_persona_states.find(sid);
            if (it != p.friend_persona_states.end() && it->second != 0) {
                flags = cb::kPersonaChangeStatus | cb::kPersonaChangeGoneOffline;
            }
            p.friend_persona_states.erase(sid);
        } else {
            // Track whether the entry was already in the map (prev_known)
            // so a "set to 0" on a fresh sid doesn't synthesize a
            // misleading 0→0 no-op skip. Always write — the map insert
            // is the canonical signal that game-side reads will see 0
            // (Offline) rather than absent. Only the callback emit is
            // gated on a real transition.
            uint32_t prev = 0;
            bool prev_known = false;
            auto it = p.friend_persona_states.find(sid);
            if (it != p.friend_persona_states.end()) {
                prev = it->second;
                prev_known = true;
            }
            p.friend_persona_states[sid] = static_cast<uint32_t>(state);
            if (!prev_known || prev != static_cast<uint32_t>(state)) {
                flags = cb::kPersonaChangeStatus;
                if (prev == 0 && state != 0) flags |= cb::kPersonaChangeComeOnline;
                if (prev != 0 && state == 0) flags |= cb::kPersonaChangeGoneOffline;
            }
        }
    }
    if (flags != 0) emit_persona_state_change(static_cast<uint64_t>(steamId64), flags);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendGamePlayed(
        JNIEnv* /*env*/, jclass /*cls*/, jlong steamId64, jint appId) {
    if (steamId64 == 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (appId <= 0) {
        p.friend_game_played_app.erase(static_cast<uint64_t>(steamId64));
    } else {
        p.friend_game_played_app[static_cast<uint64_t>(steamId64)] =
            static_cast<uint32_t>(appId);
    }
}

// Per-friend persona name. Called once per friend, when wn-session
// learns a friend's persona via CMsgClientPersonaState.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendPersonaName(
        JNIEnv* env, jclass /*cls*/, jlong steamId64, jstring jname) {
    if (steamId64 == 0) return;
    auto& p = lsc::pushed();
    std::string name = jstr(env, jname);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        const uint64_t sid = static_cast<uint64_t>(steamId64);
        if (name.empty()) {
            changed = (p.friend_persona_names.erase(sid) > 0);
        } else {
            auto it = p.friend_persona_names.find(sid);
            changed = (it == p.friend_persona_names.end() || it->second != name);
            p.friend_persona_names[sid] = std::move(name);
        }
    }
    if (changed) {
        emit_persona_state_change(static_cast<uint64_t>(steamId64),
                                  cb::kPersonaChangeName);
    }
}

// Push the bound app's command-line argv string (joined with spaces).
// Surfaces through ISteamApps.GetLaunchCommandLine (slot 26). Empty
// string clears it.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetLaunchCommandLine(
        JNIEnv* env, jclass /*cls*/, jstring jcli) {
    std::string cl = jstr(env, jcli);
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.launch_command_line = std::move(cl);
    WN_LOGI("set_launch_command_line(\"%s\")", p.launch_command_line.c_str());
}

// Push whether the bound app is family-shared (vs directly licensed).
// Surfaces through ISteamApps.BIsSubscribedFromFamilySharing (slot 27).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppFamilyShared(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean familyShared) {
    lsc::pushed().app_is_family_shared.store(familyShared == JNI_TRUE);
}

// Push the bytes from a real CMsgClientRequestEncryptedAppTicket
// response into the per-app ticket cache. wn-session calls this after
// its CM round-trip completes; subsequent ISteamUser.RequestEncryptedApp
// Ticket / GetEncryptedAppTicket calls then serve the real bytes
// instead of the synthetic "WNETKT" placeholder. Empty body clears
// the cache entry (forces a fresh fetch).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetEncryptedAppTicket(
        JNIEnv* env, jclass /*cls*/, jint appId, jbyteArray body, jint eresult) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    if (!body) {
        p.encrypted_app_tickets.erase(static_cast<uint32_t>(appId));
    } else {
        jsize n = env->GetArrayLength(body);
        if (n <= 0) {
            p.encrypted_app_tickets.erase(static_cast<uint32_t>(appId));
        } else {
            std::vector<uint8_t> buf(n);
            env->GetByteArrayRegion(body, 0, n, reinterpret_cast<jbyte*>(buf.data()));
            p.encrypted_app_tickets[static_cast<uint32_t>(appId)] = std::move(buf);
        }
    }
    p.encrypted_app_ticket_eresult.store(static_cast<int32_t>(eresult));
    WN_LOGI("set_encrypted_app_ticket: app=%d bytes=%d eresult=%d",
            appId, body ? env->GetArrayLength(body) : 0, eresult);
}

// Reports a Steam logon failure (EResult + still-retrying flag) into
// the callback queue as SteamServerConnectFailure_t. The wn-session
// CM-logon-failed observer calls this when it gets an EResult != OK
// from ClientLogOnResponse (e.g. EResult=15 AccessDenied,
// EResult=84 RateLimit, EResult=5 InvalidPassword). Games gating on
// the connect-failure callback (multiplayer auth retry, "Steam
// unreachable" UI) see the result instead of silently hanging.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeReportLogonFailure(
        JNIEnv* /*env*/, jclass /*cls*/, jint eresult, jboolean stillRetrying) {
    cb::SteamServerConnectFailure payload{};
    payload.m_eResult        = static_cast<int32_t>(eresult);
    payload.m_bStillRetrying = (stillRetrying == JNI_TRUE);
    lsc::push_callback(lsc::state().user.load(),
                       cb::kSteamServerConnectFailure,
                       &payload, sizeof(payload));
    WN_LOGI("report_logon_failure: eresult=%d stillRetrying=%d "
            "(SteamServerConnectFailure_t emitted)",
            eresult, payload.m_bStillRetrying ? 1 : 0);
}

// Pushes Steam server realtime (unix seconds) + records a steady_clock
// anchor so GetServerRealTime can advance the clock between logons.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetServerRealTime(
        JNIEnv* /*env*/, jclass /*cls*/, jint serverRealTimeUnix) {
    auto& p = lsc::pushed();
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    p.server_realtime.store(static_cast<uint32_t>(serverRealTimeUnix));
    p.server_realtime_anchor_local_ms.store(static_cast<int64_t>(now_ms));
}

// Push the account-level Steam Cloud enablement flag. Powers
// ISteamRemoteStorage.IsCloudEnabledForAccount.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetCloudEnabledForAccount(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean enabled) {
    lsc::pushed().cloud_enabled_account.store(enabled == JNI_TRUE);
}

// Push the currently-bound game's cloud enablement flag. Powers
// ISteamRemoteStorage.IsCloudEnabledForApp.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetCloudEnabledForApp(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean enabled) {
    lsc::pushed().cloud_enabled_app.store(enabled == JNI_TRUE);
}

// Push the cloud quota (total + available bytes). Zero values are
// surfaced as-is — many games treat 0/0 as "quota unknown".
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetCloudQuota(
        JNIEnv* /*env*/, jclass /*cls*/, jlong totalBytes, jlong availBytes) {
    auto& p = lsc::pushed();
    p.cloud_quota_total.store(static_cast<uint64_t>(totalBytes));
    p.cloud_quota_available.store(static_cast<uint64_t>(availBytes));
}

// Push the cloud file list. Three parallel arrays must be the same
// length: names, sizes (bytes), timestamps (unix seconds). Replaces
// the entire list — callers push the COMPLETE snapshot each time.
// Empty or mismatched-length arrays clear the list.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetCloudFiles(
        JNIEnv* env, jclass /*cls*/, jobjectArray names, jintArray sizes,
        jlongArray timestamps) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.cloud_files.clear();
    if (!names || !sizes || !timestamps) return;
    jsize n  = env->GetArrayLength(names);
    jsize ns = env->GetArrayLength(sizes);
    jsize nt = env->GetArrayLength(timestamps);
    if (n <= 0 || n != ns || n != nt) return;
    jint*  sbuf = env->GetIntArrayElements(sizes, nullptr);
    jlong* tbuf = env->GetLongArrayElements(timestamps, nullptr);
    if (!sbuf || !tbuf) {
        if (sbuf) env->ReleaseIntArrayElements(sizes, sbuf, JNI_ABORT);
        if (tbuf) env->ReleaseLongArrayElements(timestamps, tbuf, JNI_ABORT);
        return;
    }
    p.cloud_files.reserve(n);
    for (jsize i = 0; i < n; ++i) {
        auto jname = reinterpret_cast<jstring>(env->GetObjectArrayElement(names, i));
        std::string name;
        if (jname) {
            const char* c = env->GetStringUTFChars(jname, nullptr);
            if (c) { name = c; env->ReleaseStringUTFChars(jname, c); }
            env->DeleteLocalRef(jname);
        }
        if (name.empty()) continue;
        wn_libsteamclient::PushedState::CloudFileEntry e;
        e.name      = std::move(name);
        e.size      = static_cast<int32_t>(sbuf[i]);
        e.timestamp = static_cast<int64_t>(tbuf[i]);
        p.cloud_files.push_back(std::move(e));
    }
    env->ReleaseIntArrayElements(sizes, sbuf, JNI_ABORT);
    env->ReleaseLongArrayElements(timestamps, tbuf, JNI_ABORT);
    size_t pushed_count = p.cloud_files.size();
    uint32_t app = p.app_id.load();
    // RemoteStorageAppSyncedClient_t — fire iff we have a bound game.
    // appId=0 (prewarm) push is meaningful for our own state but isn't
    // a "sync completed for app X" event a game would gate on, so we
    // skip the callback in that case to avoid spurious wakeups on
    // generic library-mode pushes.
    if (app != 0) {
        cb::RemoteStorageAppSyncedClient payload{};
        payload.m_nAppID         = app;
        payload.m_eResult        = pushed_count > 0 ? 1 : 2;  // OK / Fail
        payload.m_unNumDownloads = static_cast<int32_t>(pushed_count);
        lsc::push_callback(lsc::state().user.load(),
                           cb::kRemoteStorageAppSyncedClient,
                           &payload, sizeof(payload));
    }
    WN_LOGI("set_cloud_files: %zu entries app=%u (cb emitted=%d)",
            pushed_count, app, app != 0 ? 1 : 0);
}

// Replace the achievement schema with a fresh set. Five parallel arrays
// must be the same length: apiNames, displayNames, descriptions, icons
// (URL or empty), hidden flags. Calling with empty/null arrays clears
// the schema. Per-entry progress (achieved / unlock_time) is set via
// nativeSetAchievementProgress.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAchievementSchema(
        JNIEnv* env, jclass /*cls*/, jobjectArray apiNames,
        jobjectArray displayNames, jobjectArray descriptions,
        jobjectArray icons, jbooleanArray hidden) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.achievements.clear();
    p.achievement_index.clear();
    // New app context = new stat namespace. Drop any leftover dirty
    // markers from a previous app so we don't accidentally upload
    // stat values to the wrong game's profile on a stale StoreStats.
    // The stat_name_to_id map is rebuilt by the follow-up setStatIds
    // push, so we don't clear it here (a brief window with both old
    // and new mappings is fine; clear-and-rebuild would risk a race).
    p.dirty_stats_int.clear();
    p.dirty_stats_float.clear();
    if (!apiNames) { p.stats_ready.store(true); return; }
    jsize n  = env->GetArrayLength(apiNames);
    jsize nd = displayNames ? env->GetArrayLength(displayNames) : 0;
    jsize nx = descriptions ? env->GetArrayLength(descriptions) : 0;
    jsize ni = icons        ? env->GetArrayLength(icons)        : 0;
    jsize nh = hidden       ? env->GetArrayLength(hidden)       : 0;
    if (n <= 0) { p.stats_ready.store(true); return; }
    // Parallel-array length mismatches are clamped to the apiNames
    // count — missing slots become empty strings / false.
    jboolean* hbuf = (hidden && nh == n) ? env->GetBooleanArrayElements(hidden, nullptr) : nullptr;
    p.achievements.reserve(n);
    p.achievement_index.reserve(n);
    auto read = [&](jobjectArray arr, jsize len, jsize i) -> std::string {
        if (!arr || i >= len) return {};
        auto js = reinterpret_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (!js) return {};
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string out = c ? c : "";
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return out;
    };
    for (jsize i = 0; i < n; ++i) {
        wn_libsteamclient::PushedState::AchievementEntry e;
        e.api_name     = read(apiNames,     n,  i);
        if (e.api_name.empty()) continue;
        // Seed the per-locale maps with the caller-supplied single
        // string under the "english" key — that's the lingua franca
        // every achievement_schema VDF ships with. Per-locale variants
        // get layered in later via nativeAddAchievementLocale.
        std::string dn = read(displayNames, nd, i);
        std::string ds = read(descriptions, nx, i);
        if (!dn.empty()) e.display_names.emplace("english", std::move(dn));
        if (!ds.empty()) e.descriptions.emplace("english", std::move(ds));
        e.icon         = read(icons,        ni, i);
        e.hidden       = hbuf && hbuf[i] == JNI_TRUE;
        e.icon_handle  = static_cast<int32_t>(p.achievements.size()) + 1;  // non-zero
        p.achievement_index.emplace(e.api_name, p.achievements.size());
        p.achievements.push_back(std::move(e));
    }
    if (hbuf) env->ReleaseBooleanArrayElements(hidden, hbuf, JNI_ABORT);
    p.stats_ready.store(true);
    size_t pushed_count = p.achievements.size();
    // Emit UserStatsReceived_t so any game that called
    // ISteamUserStats.RequestCurrentStats and is pumping callbacks via
    // SteamAPI_RunCallbacks (which routes through Steam_BGetCallback)
    // sees the response fire. Games gate every other ISteamUserStats
    // call on receiving this — without it, GetNumAchievements is
    // never even queried even though the data is in pushed state.
    cb::UserStatsReceived payload{};
    payload.m_nGameID      = static_cast<uint64_t>(p.app_id.load());
    payload.m_eResult      = pushed_count > 0 ? 1 : 2;  // 1=k_EResultOK, 2=k_EResultFail
    payload.m_steamIDUser  = p.steam_id.load();
    // Use the bookkept global HSteamUser (0 if not yet alloc'd; games
    // that read m_hSteamUser strictly equate to their own user handle
    // will skip — non-fatal, the rare game in that bucket re-pumps).
    int h_user = lsc::state().user.load();
    lsc::push_callback(h_user, cb::kUserStatsReceived, &payload, sizeof(payload));
    WN_LOGI("set_achievement_schema: %zu entries (UserStatsReceived_t emitted "
            "user=%d game=%llu eresult=%d)",
            pushed_count, h_user,
            static_cast<unsigned long long>(payload.m_nGameID),
            payload.m_eResult);
}

// Push the name→numeric-id map for stats. Powers SetStatInt/Float's
// server-upload path. Replaces any prior map (full snapshot per
// push). ids[i] is the numeric stat_id for names[i].
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetStatIds(
        JNIEnv* env, jclass /*cls*/, jobjectArray jNames, jintArray jIds) {
    if (!jNames || !jIds) return;
    jsize n = env->GetArrayLength(jNames);
    if (n <= 0 || env->GetArrayLength(jIds) != n) return;
    jint* ids = env->GetIntArrayElements(jIds, nullptr);
    auto& p = lsc::pushed();
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        p.stat_name_to_id.clear();
        for (jsize i = 0; i < n; ++i) {
            jstring js = static_cast<jstring>(env->GetObjectArrayElement(jNames, i));
            if (!js) continue;
            const char* c = env->GetStringUTFChars(js, nullptr);
            if (c && *c && ids[i] >= 0) {
                p.stat_name_to_id[c] = static_cast<uint32_t>(ids[i]);
            }
            if (c) env->ReleaseStringUTFChars(js, c);
            env->DeleteLocalRef(js);
        }
    }
    env->ReleaseIntArrayElements(jIds, ids, JNI_ABORT);
    WN_LOGI("set_stat_ids: %d entries", n);
}

// Layer the bit-pack mapping on top of the schema — block_id +
// bit_index per achievement so StoreStats can construct the right
// CMsgClientStoreUserStats2 payload. Parallel arrays:
//   apiNames[i] → (blockIds[i], bitIndices[i])
// blockId == -1 means "no bit-pack mapping for this entry" (entries
// without an explicit bits-block in the schema). Idempotent — calls
// with the same data are no-ops. Achievements not present in the
// current schema are silently skipped.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAchievementBlockBits(
        JNIEnv* env, jclass /*cls*/, jobjectArray apiNames,
        jintArray blockIds, jintArray bitIndices) {
    if (!apiNames || !blockIds || !bitIndices) return;
    jsize n = env->GetArrayLength(apiNames);
    if (n <= 0) return;
    if (env->GetArrayLength(blockIds) != n ||
        env->GetArrayLength(bitIndices) != n) {
        WN_LOGI("set_achievement_block_bits: array length mismatch (n=%d) — ignoring", n);
        return;
    }
    jint* blocks = env->GetIntArrayElements(blockIds, nullptr);
    jint* bits   = env->GetIntArrayElements(bitIndices, nullptr);
    auto& p = lsc::pushed();
    size_t applied = 0;
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        for (jsize i = 0; i < n; ++i) {
            jstring js = static_cast<jstring>(env->GetObjectArrayElement(apiNames, i));
            if (!js) continue;
            const char* c = env->GetStringUTFChars(js, nullptr);
            if (!c) { env->DeleteLocalRef(js); continue; }
            auto it = p.achievement_index.find(c);
            if (it != p.achievement_index.end() && it->second < p.achievements.size()) {
                auto& ach = p.achievements[it->second];
                ach.block_id  = blocks[i];
                ach.bit_index = bits[i];
                ++applied;
            }
            env->ReleaseStringUTFChars(js, c);
            env->DeleteLocalRef(js);
        }
    }
    env->ReleaseIntArrayElements(blockIds, blocks, JNI_ABORT);
    env->ReleaseIntArrayElements(bitIndices, bits, JNI_ABORT);
    WN_LOGI("set_achievement_block_bits: applied %zu / %d", applied, n);
}

// Per-locale variant of an achievement's display name + description.
// Layered on top of nativeSetAchievementSchema (which seeded the
// "english" key). No-op when the api name isn't in the schema yet
// OR when both displayName and description are empty. Empty values
// for either field skip the corresponding map insert — partial
// localizations are common in Steam schemas (e.g. some langs have
// only display_name populated, descriptions fall back to english).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeAddAchievementLocale(
        JNIEnv* env, jclass /*cls*/, jstring jApiName, jstring jLocale,
        jstring jDisplayName, jstring jDescription) {
    if (!jApiName || !jLocale) return;
    std::string api    = jstr(env, jApiName);
    std::string locale = jstr(env, jLocale);
    std::string dn     = jstr(env, jDisplayName);
    std::string ds     = jstr(env, jDescription);
    if (api.empty() || locale.empty() || (dn.empty() && ds.empty())) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto it = p.achievement_index.find(api);
    if (it == p.achievement_index.end()) return;
    auto& a = p.achievements[it->second];
    if (!dn.empty()) a.display_names[locale] = std::move(dn);
    if (!ds.empty()) a.descriptions[locale]  = std::move(ds);
}

// Update the (achieved, unlockTime) tuple for a single achievement.
// No-op when the api name isn't in the schema yet.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAchievementProgress(
        JNIEnv* env, jclass /*cls*/, jstring jApiName, jboolean achieved,
        jint unlockTimeUnix) {
    if (!jApiName) return;
    std::string name = jstr(env, jApiName);
    if (name.empty()) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto it = p.achievement_index.find(name);
    if (it == p.achievement_index.end()) return;
    auto& a = p.achievements[it->second];
    a.achieved    = (achieved == JNI_TRUE);
    a.unlock_time = static_cast<uint32_t>(unlockTimeUnix);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetStatInt(
        JNIEnv* env, jclass /*cls*/, jstring jName, jint value) {
    if (!jName) return;
    std::string name = jstr(env, jName);
    if (name.empty()) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.stats_int[std::move(name)] = static_cast<int32_t>(value);
}

JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetStatFloat(
        JNIEnv* env, jclass /*cls*/, jstring jName, jfloat value) {
    if (!jName) return;
    std::string name = jstr(env, jName);
    if (name.empty()) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    p.stats_float[std::move(name)] = value;
}

// Diagnostic — returns the current pushed achievement count from
// pushed().achievements WITHOUT going through the ISteamUserStats vtable.
// Verifies the setter / pushed-state singleton plumbing in isolation; if
// this returns N and the vtable path returns 0 we've localized the bug
// to the vtable dispatch side.
// Diagnostic — returns pushed().achievements.size() without going
// through the ISteamUserStats vtable. Kept post-debug because the
// HybridModeReceiver state-dump uses it to verify singleton coherence
// (a mismatch with bs.numAchievements() localizes plumbing bugs).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticAchievementCount(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    return static_cast<jint>(lsc::pushed().achievements.size());
}

// Diagnostic — returns the current callback queue depth (non-destructive
// peek). Games consume callbacks via Steam_BGetCallback; this lets the
// HybridModeReceiver state op confirm pushes are landing without
// draining the queue. Returns -1 if the lock can't be acquired.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticCallbackDepth(
        JNIEnv* /*env*/, jclass /*cls*/) {
    auto& s = lsc::state();
    std::lock_guard<std::mutex> lk(s.callback_mu);
    return static_cast<jint>(s.callback_queue.size());
}

// Diagnostic — returns the number of inbound TCP connections the
// Steam3Master + SteamClientService listeners have accepted since
// process start. Lets the cron loop verify the listeners are reachable
// + see uptick when Wine's lsteamclient.dll opens an IPC channel.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticTcpAccepted(
        JNIEnv* /*env*/, jclass /*cls*/) {
    return static_cast<jint>(lsc::accepted_connection_count());
}

// Diagnostic — pushes a synthetic CallResult under a fresh hCall, then
// queries it via ISteamUtils.GetAPICallResult (vtable slot 13) on
// our stub. Returns the eresult byte we observe, or -1 on failure.
// Verifies the slot-13 delegation matches the flat-C Steam_GetAPICallResult
// path that lives in api_entry.cpp.
extern "C" void* wn_get_isteam_utils();
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticUtilsGetAPICallResult(
        JNIEnv* /*env*/, jclass /*cls*/, jint iCallback, jint eresultIn) {
    uint64_t h = lsc::alloc_api_call_handle();
    int32_t body = eresultIn;
    lsc::push_call_result(h, static_cast<int>(iCallback),
                          &body, sizeof(body), /*io_failure=*/false);
    void* obj = wn_get_isteam_utils();
    if (!obj) return -1;
    long* vt = *reinterpret_cast<long**>(obj);
    using GetResultFn = bool (*)(void*, uint64_t, void*, int, int, bool*);
    auto getr = reinterpret_cast<GetResultFn>(vt[13]);
    int32_t out = -1;
    bool failed = false;
    bool ok = getr(obj, h, &out, sizeof(out), static_cast<int>(iCallback), &failed);
    return ok ? out : -1;
}

// Diagnostic — exercises ISteamUser.RequestEncryptedAppTicket (slot 21)
// then GetEncryptedAppTicket (slot 22) via the vtable. Returns the
// hCall in element 0 of a 1-element long[] AND fills [outBody] with
// whatever GetEncryptedAppTicket served back. The two-call sequence
// mirrors what a real game does: kick the async fetch, then read the
// cached bytes once the response lands.
extern "C" void* wn_get_isteam_user();
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRequestEncryptedAppTicket(
        JNIEnv* env, jclass /*cls*/, jbyteArray outBody) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using ReqFn = uint64_t (*)(void*, void*, int);
    auto req = reinterpret_cast<ReqFn>(vt[21]);
    uint64_t h = req(obj, nullptr, 0);
    using GetFn = bool (*)(void*, void*, int, uint32_t*);
    auto get = reinterpret_cast<GetFn>(vt[22]);
    uint8_t scratch[128] = {0};
    uint32_t actual = 0;
    bool ok = get(obj, scratch, sizeof(scratch), &actual);
    if (ok && outBody && env->GetArrayLength(outBody) >= static_cast<jsize>(actual)) {
        env->SetByteArrayRegion(outBody, 0, static_cast<jsize>(actual),
                                reinterpret_cast<const jbyte*>(scratch));
    }
    return static_cast<jlong>(h);
}

// Diagnostic — calls ISteamUser.GetAuthSessionTicket (vtable slot 13)
// via our stub directly. Returns the HAuthTicket (uint32) packed into
// jint. Lets the test harness verify the synthetic ticket path
// without needing a Wine-side game.
extern "C" void* wn_get_isteam_user();
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetAuthTicket(
        JNIEnv* env, jclass /*cls*/, jbyteArray jbuf) {
    void* obj = wn_get_isteam_user();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using GetTicketFn = uint64_t (*)(void*, void*, int, uint32_t*, const void*);
    auto get_ticket = reinterpret_cast<GetTicketFn>(vt[13]);
    // Buffer to fill — allocate a small one so we can copy back to Kotlin.
    uint8_t  scratch[64] = {0};
    uint32_t actual = 0;
    uint64_t h = get_ticket(obj, scratch, sizeof(scratch), &actual, nullptr);
    if (jbuf && env->GetArrayLength(jbuf) >= static_cast<jsize>(actual)) {
        env->SetByteArrayRegion(jbuf, 0, static_cast<jsize>(actual),
                                reinterpret_cast<const jbyte*>(scratch));
    }
    return static_cast<jint>(h);
}

// Diagnostic — exercises the full SteamAPI_RegisterCallback +
// SteamAPI_RunCallbacks dispatch chain WITHOUT needing a Wine-side
// game. Builds a minimal CCallbackBase-shaped object in-process whose
// Run() bumps a counter; registers it for UserStatsReceived_t (1101),
// then expects the next RunCallbacks pump to invoke Run once for
// every queued callback of that id. Returns the post-pump Run count.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RunCallbacks(void);

namespace {

// 16-byte CCallbackBase-shaped layout (vptr + flags + id), prefix of
// a real CCallback<T,P>. We extend with a state slot the Run() body
// can write into so the test verifies dispatch happened.
struct DiagnosticCallback {
    void**       vptr;
    uint8_t      flags;
    uint8_t      _pad0[3];
    int32_t      iCallback;
    // Probe-only fields:
    int32_t      runs;          // bumped on each Run()
    int32_t      last_user;     // copied from msg payload[0] (m_nGameID low 32)
    int32_t      last_eresult;  // copied from msg payload offset 8 (m_eResult)
};

void diagnostic_run(DiagnosticCallback* self, void* payload) {
    if (!self) return;
    ++self->runs;
    if (payload) {
        // UserStatsReceived_t: m_nGameID@0 (8B), m_eResult@8 (4B).
        // We're not strict about offsets — this is a probe.
        self->last_user    = static_cast<int32_t>(
            *reinterpret_cast<const uint32_t*>(payload));
        self->last_eresult = *reinterpret_cast<const int32_t*>(
            static_cast<const uint8_t*>(payload) + 8);
    }
}

void diagnostic_run_result(DiagnosticCallback*, void*, bool, uint64_t) {}
int  diagnostic_get_size(DiagnosticCallback*) { return 24; }  // sizeof(UserStatsReceived_t)

// Static vtable matching CCallbackBase + CCallResult layout: slot 0
// Run(void*), slot 1 Run(void*,bool,SteamAPICall_t), slot 2
// GetCallbackSizeBytes(). Order matters — verified against the SDK's
// public headers.
void* const kDiagnosticVtable[] = {
    reinterpret_cast<void*>(&diagnostic_run),
    reinterpret_cast<void*>(&diagnostic_run_result),
    reinterpret_cast<void*>(&diagnostic_get_size),
};

DiagnosticCallback g_diagnostic_cb;
bool g_diagnostic_registered = false;

}  // namespace

// Probe state for the CCallResult-side test. Layout mirrors the
// Callback probe but Run() is at vtable slot 1 with the
// (payload, bIOFailure, hCall) signature.
namespace {
struct DiagnosticCallResultCb {
    void**       vptr;
    uint8_t      flags;
    uint8_t      _pad0[3];
    int32_t      iCallback;
    // Probe-only fields:
    int32_t      runs;
    uint64_t     last_h_call;
    int32_t      last_io_failure;
    int32_t      last_eresult;
};

void diag_cr_run(DiagnosticCallResultCb*, void*) {}  // slot 0 unused for CCallResult
void diag_cr_run_result(DiagnosticCallResultCb* self, void* payload,
                        bool ioFailure, uint64_t hCall) {
    if (!self) return;
    ++self->runs;
    self->last_h_call    = hCall;
    self->last_io_failure = ioFailure ? 1 : 0;
    if (payload) {
        // Payload's first 4 bytes go into last_eresult so the
        // diagnostic can verify the right body landed.
        self->last_eresult = *reinterpret_cast<const int32_t*>(payload);
    }
}
int  diag_cr_get_size(DiagnosticCallResultCb*) { return 0; }

void* const kDiagnosticCallResultVtable[] = {
    reinterpret_cast<void*>(&diag_cr_run),
    reinterpret_cast<void*>(&diag_cr_run_result),
    reinterpret_cast<void*>(&diag_cr_get_size),
};

DiagnosticCallResultCb g_diag_cr_cb;
}  // namespace

// Diagnostic — exercises the full async-result chain WITHOUT issuing a
// real async op. Allocates a fresh hCall, pushes a synthetic result
// (4-byte int eresult body), registers a CCallResult probe, runs
// SteamAPI_RunCallbacks, and returns the run-count. Verifies vtable[1]
// dispatch end-to-end.
extern "C" __attribute__((visibility("default")))
void SteamAPI_RunCallbacks(void);
extern "C" __attribute__((visibility("default")))
void SteamAPI_RegisterCallResult(void*, uint64_t);

JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticPushAndDrainCallResult(
        JNIEnv* /*env*/, jclass /*cls*/, jint callbackId, jint eresult) {
    // Allocate a fresh hCall + push a synthetic result.
    uint64_t h_call = lsc::alloc_api_call_handle();
    int32_t  body_eresult = eresult;
    lsc::push_call_result(h_call, static_cast<int>(callbackId),
                          &body_eresult, sizeof(body_eresult),
                          /*io_failure=*/false);

    // Register the probe cb. Reset fields so the result is observable.
    g_diag_cr_cb.vptr      = const_cast<void**>(kDiagnosticCallResultVtable);
    g_diag_cr_cb.flags     = 0;
    g_diag_cr_cb.iCallback = static_cast<int32_t>(callbackId);
    g_diag_cr_cb.runs           = 0;
    g_diag_cr_cb.last_h_call    = 0;
    g_diag_cr_cb.last_io_failure = -1;
    g_diag_cr_cb.last_eresult   = 0;
    SteamAPI_RegisterCallResult(&g_diag_cr_cb, h_call);

    // Pump — dispatches via vtable[1].
    SteamAPI_RunCallbacks();

    // Pack: hi32 = runs, lo32 = eresult observed. Caller decodes.
    return (static_cast<jlong>(g_diag_cr_cb.runs) << 32) |
           (static_cast<jlong>(g_diag_cr_cb.last_eresult) & 0xFFFFFFFFL);
}

JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRegisterAndDrain(
        JNIEnv* /*env*/, jclass /*cls*/, jint iCallback) {
    // Register on first call, leave registered for re-use. Reset the
    // probe counters each call so consecutive invocations are
    // independently observable.
    g_diagnostic_cb.vptr      = const_cast<void**>(kDiagnosticVtable);
    g_diagnostic_cb.flags     = 0;
    g_diagnostic_cb.iCallback = static_cast<int32_t>(iCallback);
    g_diagnostic_cb.runs         = 0;
    g_diagnostic_cb.last_user    = 0;
    g_diagnostic_cb.last_eresult = 0;
    if (!g_diagnostic_registered) {
        lsc::register_callback(&g_diagnostic_cb, static_cast<int>(iCallback));
        g_diagnostic_registered = true;
    }
    // Run the pump — drains the queue + dispatches to our cb.
    SteamAPI_RunCallbacks();
    return g_diagnostic_cb.runs;
}

// Diagnostic — releases + recreates the pipe so the SteamShutdown_t /
// SteamServersDisconnected_t emit path runs without tearing the whole
// .so down. Returns true if a pipe existed and was released. Reads
// the pipe handle out of state(), calls release_pipe (which emits
// the callbacks), then re-allocates a pipe so subsequent diagnostics
// still work.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticShutdownPipe(
        JNIEnv* /*env*/, jclass /*cls*/) {
    int pipe = lsc::state().pipe.load();
    if (pipe == 0) return JNI_FALSE;
    // Emit SteamShutdown_t directly here — Steam_BReleaseSteamPipe
    // does it before release_pipe; we mirror that path so callers
    // get the same callback sequence.
    namespace cb = wn_libsteamclient::callbacks;
    cb::SteamShutdown sd{};
    lsc::push_callback(lsc::state().user.load(),
                       cb::kSteamShutdown, &sd, 0);
    bool ok = lsc::release_pipe(pipe);
    // Re-allocate so the test harness doesn't leave the .so in a
    // pipe-less state (subsequent diagnostics would fail).
    (void)lsc::alloc_pipe();
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — directly invokes ISteamUserStats::StoreStats on our stub.
// Used by seedTestStats to verify UserStatsStored_t emission without
// needing a Wine-side lsteamclient.dll bridge. Returns true if the stub
// reported success (stats_ready was set).
extern "C" void* wn_get_isteam_user_stats();
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticStoreStats(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_user_stats();
    if (!obj) return JNI_FALSE;
    // vtable slot 10 = StoreStats() — see isteam_stubs.cpp's
    // ISteamUserStatsStub. Same dispatch pattern the bootstrap uses.
    long* vt = *reinterpret_cast<long**>(obj);
    using StoreFn = bool (*)(void*);
    auto fn = reinterpret_cast<StoreFn>(vt[10]);
    return fn(obj) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamUserStats::SetAchievement (slot 7) on our
// stub. Used to mark an achievement pending_store so a follow-up
// StoreStats emits UserAchievementStored_t for it.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticSetAchievement(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    if (!jName) return JNI_FALSE;
    void* obj = wn_get_isteam_user_stats();
    if (!obj) return JNI_FALSE;
    std::string name = jstr(env, jName);
    long* vt = *reinterpret_cast<long**>(obj);
    using SetFn = bool (*)(void*, const char*);
    auto fn = reinterpret_cast<SetFn>(vt[7]);
    return fn(obj, name.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamUserStats::IndicateAchievementProgress
// (slot 13) on our stub. Emits a UserAchievementStored_t with
// curProgress/maxProgress filled. Returns true on success per stub
// contract (false if achievement unknown or already unlocked).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticIndicateAchievementProgress(
        JNIEnv* env, jclass /*cls*/, jstring jName, jint cur, jint max) {
    if (!jName) return JNI_FALSE;
    void* obj = wn_get_isteam_user_stats();
    if (!obj) return JNI_FALSE;
    std::string name = jstr(env, jName);
    long* vt = *reinterpret_cast<long**>(obj);
    using IndFn = bool (*)(void*, const char*, uint32_t, uint32_t);
    auto fn = reinterpret_cast<IndFn>(vt[13]);
    return fn(obj, name.c_str(),
              static_cast<uint32_t>(cur),
              static_cast<uint32_t>(max)) ? JNI_TRUE : JNI_FALSE;
}

// Push rich-presence (key, value) for a remote friend. Called by
// wn-session when CMsgClientPersonaState delivers RP for a peer (or
// for synthetic seeding from the diagnostic op). Empty/null value
// removes the key, matching SetRichPresence semantics. Emits
// FriendRichPresenceUpdate_t so callers gated on the callback proceed.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendRichPresence(
        JNIEnv* env, jclass /*cls*/, jlong jSteamId, jstring jKey, jstring jValue) {
    uint64_t steam_id = static_cast<uint64_t>(jSteamId);
    if (steam_id == 0 || !jKey) return;
    std::string key   = jstr(env, jKey);
    std::string value = jstr(env, jValue);
    if (key.empty()) return;
    auto& p = lsc::pushed();
    {
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto& rp = p.rich_presence[steam_id];
        auto it = std::find_if(rp.begin(), rp.end(),
            [&](const auto& kv) { return kv.first == key; });
        if (value.empty()) {
            if (it != rp.end()) rp.erase(it);
        } else if (it == rp.end()) {
            rp.emplace_back(std::move(key), std::move(value));
        } else {
            it->second = std::move(value);
        }
    }
    cb::FriendRichPresenceUpdate ev{};
    ev.m_steamIDFriend = steam_id;
    ev.m_nAppID        = p.app_id.load();
    lsc::push_callback(lsc::state().user.load(),
                       cb::kFriendRichPresenceUpdate,
                       &ev, sizeof(ev));
}

// Diagnostic — invokes ISteamFriends::SetPersonaName (slot 1) via
// vtable. Returns the SteamAPICall_t hCall the stub allocated.
// Going through the vtable exercises the bridge call to
// wn_cm_set_persona_name (CMsgClientChangeStatus broadcast), which a
// pure Kotlin → nativeSetPersonaName setter path doesn't.
extern "C" void* wn_get_isteam_friends();
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticSetPersonaName(
        JNIEnv* env, jclass /*cls*/, jstring jName) {
    if (!jName) return 0;
    void* obj = wn_get_isteam_friends();
    if (!obj) return 0;
    std::string name = jstr(env, jName);
    long* vt = *reinterpret_cast<long**>(obj);
    using SetFn = uint64_t (*)(void*, const char*);
    auto fn = reinterpret_cast<SetFn>(vt[1]);
    return static_cast<jlong>(fn(obj, name.c_str()));
}

// Diagnostic — invokes ISteamFriends::RequestFriendRichPresence (slot 48)
// via vtable. Void return; observable side effect is the cm_bridge log +
// FriendRichPresenceUpdate_t in the callback queue.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRequestFriendRichPresence(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return;
    long* vt = *reinterpret_cast<long**>(obj);
    using ReqFn = void (*)(void*, uint64_t);
    auto fn = reinterpret_cast<ReqFn>(vt[48]);
    fn(obj, static_cast<uint64_t>(jSteamId));
}

// Diagnostic — invokes ISteamFriends::RequestUserInformation (slot 37)
// via vtable. Returns true if the stub is requesting the data (cm_
// bridge fired), false if data is already cached.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRequestUserInformation(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId, jboolean jNameOnly) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using ReqFn = bool (*)(void*, uint64_t, bool);
    auto fn = reinterpret_cast<ReqFn>(vt[37]);
    return fn(obj, static_cast<uint64_t>(jSteamId), jNameOnly == JNI_TRUE)
           ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — synthetically dispatches the logon-state observer so the
// reactive CMClient → libsteamclient.so path can be verified offline
// without a real wn-session sign-in. Same effect as if CMClient had
// transitioned through Connecting → Connected → LoggedOn (or the inverse).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectLogonState(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean jLoggedOn) {
    wn_cm_bridge_inject_test_logon_state(jLoggedOn == JNI_TRUE);
}

// Diagnostic — synthetically dispatches the license-list observer.
// jPackageIds / jOwnerIds: parallel arrays (same length). Time-created
// is auto-filled from current unix time; license_type, flags, change
// _number default to 0 — sufficient for family-share read tests.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectLicenseList(
        JNIEnv* env, jclass /*cls*/, jintArray jPackageIds, jintArray jOwnerIds) {
    if (!jPackageIds) {
        wn_cm_bridge_inject_test_license_list(nullptr, 0);
        return;
    }
    jsize n = env->GetArrayLength(jPackageIds);
    if (n <= 0) {
        wn_cm_bridge_inject_test_license_list(nullptr, 0);
        return;
    }
    jint* pkg_ids = env->GetIntArrayElements(jPackageIds, nullptr);
    jint* own_ids = jOwnerIds ? env->GetIntArrayElements(jOwnerIds, nullptr) : nullptr;
    if (!pkg_ids) return;
    std::vector<WnCmLicenseEntry> entries;
    entries.reserve(static_cast<size_t>(n));
    uint32_t now = static_cast<uint32_t>(::time(nullptr));
    for (jsize i = 0; i < n; ++i) {
        WnCmLicenseEntry e{};
        e.package_id   = static_cast<uint32_t>(pkg_ids[i]);
        e.owner_id     = (own_ids && i < env->GetArrayLength(jOwnerIds))
                            ? static_cast<uint32_t>(own_ids[i]) : 0u;
        e.time_created = now;
        entries.push_back(e);
    }
    wn_cm_bridge_inject_test_license_list(entries.data(), entries.size());
    env->ReleaseIntArrayElements(jPackageIds, pkg_ids, JNI_ABORT);
    if (own_ids) env->ReleaseIntArrayElements(jOwnerIds, own_ids, JNI_ABORT);
}

// Diagnostic — read pushed.licenses[package_id].owner_id direct.
// Returns -1 if package not in map.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetLicenseOwner(
        JNIEnv* /*env*/, jclass /*cls*/, jint jPackageId) {
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto it = p.licenses.find(static_cast<uint32_t>(jPackageId));
    if (it == p.licenses.end()) return -1;
    return static_cast<jint>(it->second.owner_id);
}

// Diagnostic — invokes ISteamApps::GetEarliestPurchaseUnixTime (slot 8)
// via vtable. Returns the min(time_created) across the app's source
// packages, or 0.
extern "C" void* wn_get_isteam_apps();
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetEarliestPurchaseUnixTime(
        JNIEnv* /*env*/, jclass /*cls*/, jint jAppId) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint32_t (*)(void*, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[8]);
    return static_cast<jint>(fn(obj, static_cast<uint32_t>(jAppId)));
}

// Diagnostic — invokes ISteamApps::BIsSubscribedFromFreeWeekend (slot 9)
// via vtable. Reads pushed.app_id, looks up source packages, checks
// any license_type==FreeWeekend.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticBIsSubscribedFromFreeWeekend(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[9]);
    return fn(obj) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamApps::BIsSubscribedFromFamilySharing
// (slot 27) via vtable. Reads bound app + self steamID + license map.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticBIsSubscribedFromFamilySharing(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[27]);
    return fn(obj) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamUserStats::UpdateAvgRateStat (slot 5)
// via vtable, then reads the resulting stats_float[name] as the
// computed running average.
JNIEXPORT jfloat JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticUpdateAvgRateStat(
        JNIEnv* env, jclass /*cls*/, jstring jName,
        jfloat jCountThisSession, jdouble jSessionLength) {
    if (!jName) return 0.0f;
    void* obj = wn_get_isteam_user_stats();
    if (!obj) return 0.0f;
    std::string name = jstr(env, jName);
    long* vt = *reinterpret_cast<long**>(obj);
    using UpdateFn = bool (*)(void*, const char*, float, double);
    auto upd = reinterpret_cast<UpdateFn>(vt[5]);
    if (!upd(obj, name.c_str(), jCountThisSession, jSessionLength)) return 0.0f;
    // Read back: vtable slot 2 = GetStat(float).
    using GetFn = bool (*)(void*, const char*, float*);
    auto get = reinterpret_cast<GetFn>(vt[2]);
    float out = 0.0f;
    get(obj, name.c_str(), &out);
    return static_cast<jfloat>(out);
}

// Diagnostic — invokes ISteamApps::GetAppOwner (slot 20) via vtable.
// Returns the CSteamID64 of the bound app's owner — self for direct
// licenses, the family-share owner for shared apps, 0 for not-owned.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetAppOwner(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = uint64_t (*)(void*);
    auto fn = reinterpret_cast<Fn>(vt[20]);
    return static_cast<jlong>(fn(obj));
}

// Diagnostic — directly insert a single license with minute_limit /
// minutes_used set. Used by the BIsTimedTrial probe — the standard
// nativeDiagnosticInjectLicenseList doesn't expose the trial fields
// (parallel-array signature too narrow). Calling this AFTER inject
// patches the entry in-place; calling it FIRST creates a new entry.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectTrialLicense(
        JNIEnv* /*env*/, jclass /*cls*/, jint jPackageId,
        jint jMinuteLimit, jint jMinutesUsed) {
    if (jPackageId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto& slot = p.licenses[static_cast<uint32_t>(jPackageId)];
    slot.package_id    = static_cast<uint32_t>(jPackageId);
    slot.minute_limit  = jMinuteLimit;
    slot.minutes_used  = jMinutesUsed;
}

// Diagnostic — invokes ISteamApps::BIsDlcInstalled (slot 7) via vtable.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticBIsDlcInstalled(
        JNIEnv* /*env*/, jclass /*cls*/, jint jAppId) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return JNI_FALSE;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint32_t);
    auto fn = reinterpret_cast<Fn>(vt[7]);
    return fn(obj, static_cast<uint32_t>(jAppId)) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamApps::BIsTimedTrial (slot 28) via vtable.
// Returns a packed long: hi bit = bool result; next 32 bits = seconds_
// allowed; low 32 bits = seconds_played. (Result false → all zero.)
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticBIsTimedTrial(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_apps();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using Fn = bool (*)(void*, uint32_t*, uint32_t*);
    auto fn = reinterpret_cast<Fn>(vt[28]);
    uint32_t allowed = 0, played = 0;
    bool ok = fn(obj, &allowed, &played);
    if (!ok) return 0;
    // pack: bit 63 = true marker, bits 32-62 = allowed (mask to fit),
    // bits 0-31 = played.
    return (1LL << 63) |
           (static_cast<int64_t>(allowed) << 32) |
           static_cast<int64_t>(played);
}

// Push the per-app source-package list. Empty/null array clears the
// entry. Same shape as nativeSetAppInstalledDepots etc. Each app can
// have multiple source packages (base package + DLC packages that
// grant it). Powers slots 8 + 9.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetAppSourcePackages(
        JNIEnv* env, jclass /*cls*/, jint appId, jintArray packageIds) {
    if (appId <= 0) return;
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto key = static_cast<uint32_t>(appId);
    if (!packageIds) {
        p.app_source_packages.erase(key);
        return;
    }
    jsize n = env->GetArrayLength(packageIds);
    if (n <= 0) {
        p.app_source_packages.erase(key);
        return;
    }
    jint* arr = env->GetIntArrayElements(packageIds, nullptr);
    if (!arr) return;
    std::vector<uint32_t> pkgs;
    pkgs.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) {
        if (arr[i] > 0) pkgs.push_back(static_cast<uint32_t>(arr[i]));
    }
    env->ReleaseIntArrayElements(packageIds, arr, JNI_ABORT);
    if (pkgs.empty()) {
        p.app_source_packages.erase(key);
    } else {
        p.app_source_packages[key] = std::move(pkgs);
    }
}

// Diagnostic — synthetically dispatches the friends-list observer.
// Mirrors a real CMsgClientFriendsList arrival.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectFriendsList(
        JNIEnv* env, jclass /*cls*/, jlongArray jSids) {
    if (!jSids) {
        wn_cm_bridge_inject_test_friends_list(nullptr, 0);
        return;
    }
    jsize n = env->GetArrayLength(jSids);
    if (n <= 0) {
        wn_cm_bridge_inject_test_friends_list(nullptr, 0);
        return;
    }
    jlong* arr = env->GetLongArrayElements(jSids, nullptr);
    if (!arr) return;
    static_assert(sizeof(jlong) == sizeof(uint64_t), "jlong/uint64 size mismatch");
    wn_cm_bridge_inject_test_friends_list(
        reinterpret_cast<const uint64_t*>(arr), static_cast<size_t>(n));
    env->ReleaseLongArrayElements(jSids, arr, JNI_ABORT);
}

// Diagnostic — synthetically dispatches the persona observer with a
// fully-controlled event payload. Lets offline tests verify the full
// observer surface (name + state + game + avatar + RP map) end-to-end
// without needing a real CMsgClientPersonaState arrival.
//
// jName: empty / null skips the name field
// jPersonaState: -1 means \"absent\" (UINT32_MAX in the POD); 0..7 = real
// jGameAppId: 0 = not in game
// jAvatarHash: null / empty skips avatar hash (no kPersonaChangeAvatar)
// jRpKeys / jRpValues: paired arrays; same length required
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectPersonaEvent(
        JNIEnv* env, jclass /*cls*/,
        jlong jSteamId,
        jint  jPersonaState,
        jint  jGameAppId,
        jstring jName,
        jbyteArray jAvatarHash,
        jobjectArray jRpKeys,
        jobjectArray jRpValues) {
    WnCmPersonaEvent ev{};
    ev.sid              = static_cast<uint64_t>(jSteamId);
    ev.persona_state    = (jPersonaState < 0) ? UINT32_MAX
                                              : static_cast<uint32_t>(jPersonaState);
    ev.game_played_app  = (jGameAppId <= 0) ? 0 : static_cast<uint32_t>(jGameAppId);

    // Strings are short-lived — borrow into them under the call's
    // stack frame. Observer must copy if it wants to retain.
    std::string name_storage;
    if (jName) {
        name_storage = jstr(env, jName);
        if (!name_storage.empty()) ev.name = name_storage.c_str();
    }

    std::vector<uint8_t> hash_storage;
    if (jAvatarHash) {
        jsize n = env->GetArrayLength(jAvatarHash);
        if (n > 0) {
            hash_storage.resize(static_cast<size_t>(n));
            env->GetByteArrayRegion(jAvatarHash, 0, n,
                reinterpret_cast<jbyte*>(hash_storage.data()));
            ev.avatar_hash     = hash_storage.data();
            ev.avatar_hash_len = hash_storage.size();
        }
    }

    // RP key/value pairs — parallel string-storage vectors keep the
    // c_str() pointers valid for the dispatch call.
    std::vector<std::string> key_storage, value_storage;
    std::vector<WnCmRichPresenceKV> rp_kv;
    if (jRpKeys && jRpValues) {
        jsize kn = env->GetArrayLength(jRpKeys);
        jsize vn = env->GetArrayLength(jRpValues);
        jsize count = std::min(kn, vn);
        key_storage.reserve(count);
        value_storage.reserve(count);
        rp_kv.reserve(count);
        for (jsize i = 0; i < count; ++i) {
            auto k_obj = reinterpret_cast<jstring>(env->GetObjectArrayElement(jRpKeys, i));
            auto v_obj = reinterpret_cast<jstring>(env->GetObjectArrayElement(jRpValues, i));
            key_storage.push_back(jstr(env, k_obj));
            value_storage.push_back(jstr(env, v_obj));
            if (k_obj) env->DeleteLocalRef(k_obj);
            if (v_obj) env->DeleteLocalRef(v_obj);
            rp_kv.push_back({key_storage.back().c_str(),
                             value_storage.back().c_str()});
        }
        if (!rp_kv.empty()) {
            ev.rp_pairs = rp_kv.data();
            ev.rp_count = rp_kv.size();
        }
    }

    wn_cm_bridge_dispatch_persona(&ev);
}

// Diagnostic — injects synthetic ownership-ticket bytes into CMClient's
// WnTicketCache so the cache-hit path of GetAuthSessionTicket can be
// exercised offline. Returns true on success. Production code populates
// the cache via real CMsgClientGetAppOwnershipTicket round-trips.
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticInjectOwnershipTicket(
        JNIEnv* env, jclass /*cls*/, jint jAppId, jbyteArray jBytes) {
    if (jAppId <= 0 || !jBytes) return JNI_FALSE;
    jsize n = env->GetArrayLength(jBytes);
    if (n <= 0) return JNI_FALSE;
    std::vector<uint8_t> tmp(static_cast<size_t>(n));
    env->GetByteArrayRegion(jBytes, 0, n, reinterpret_cast<jbyte*>(tmp.data()));
    return wn_cm_bridge_inject_test_ownership_ticket(
        static_cast<uint32_t>(jAppId), tmp.data(), tmp.size())
        ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — reads the cached ownership ticket for an appId from
// CMClient via the bridge. Returns the ticket length (0 = cache miss
// or invalid args). When [out] is large enough, fills with the bytes;
// otherwise the function still reports the size so callers can resize
// and retry.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetCachedOwnershipTicket(
        JNIEnv* env, jclass /*cls*/, jint jAppId, jbyteArray jOut) {
    if (jAppId <= 0) return 0;
    size_t out_len = 0;
    if (!jOut) {
        // Size-only query: nullptr buf, but still need a real out_len ptr.
        wn_cm_get_cached_app_ownership_ticket(static_cast<uint32_t>(jAppId),
                                              nullptr, 0, &out_len);
        return static_cast<jint>(out_len);
    }
    jsize max = env->GetArrayLength(jOut);
    std::vector<uint8_t> tmp(static_cast<size_t>(max));
    bool ok = wn_cm_get_cached_app_ownership_ticket(
        static_cast<uint32_t>(jAppId),
        tmp.data(), static_cast<size_t>(max), &out_len);
    if (!ok) return static_cast<jint>(out_len);  // 0 = miss; >0 = need bigger buf
    env->SetByteArrayRegion(jOut, 0, static_cast<jsize>(out_len),
                            reinterpret_cast<jbyte*>(tmp.data()));
    return static_cast<jint>(out_len);
}

// Diagnostic — drives the bulk cm_bridge entry point with an array of
// SteamID64s. Returns true if the bridge dispatched to a live CMClient.
// Bypasses the per-sid cache check — bulk requests don't short-circuit
// (games passing the bulk array usually WANT to refresh everything).
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRequestUserInfoBulk(
        JNIEnv* env, jclass /*cls*/, jlongArray jSids, jint jFlags) {
    if (!jSids) return JNI_FALSE;
    jsize n = env->GetArrayLength(jSids);
    if (n <= 0) return JNI_FALSE;
    jlong* arr = env->GetLongArrayElements(jSids, nullptr);
    if (!arr) return JNI_FALSE;
    static_assert(sizeof(jlong) == sizeof(uint64_t), "jlong/uint64 size mismatch");
    bool ok = wn_cm_request_user_info_bulk(reinterpret_cast<const uint64_t*>(arr),
                                            static_cast<size_t>(n),
                                            static_cast<int32_t>(jFlags));
    env->ReleaseLongArrayElements(jSids, arr, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — invokes ISteamFriends::ClearRichPresence (slot 44) via
// vtable. Drives the empty-map Player.SetRichPresence#1 broadcast +
// emits FriendRichPresenceUpdate_t for self.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticClearRichPresence(
        JNIEnv* /*env*/, jclass /*cls*/) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return;
    long* vt = *reinterpret_cast<long**>(obj);
    using ClrFn = void (*)(void*);
    auto fn = reinterpret_cast<ClrFn>(vt[44]);
    fn(obj);
}

// Diagnostic — invokes ISteamFriends::SetRichPresence (slot 43) via
// vtable. Forces the call to go through the real stub path so the
// pending_store flag, callback emission, and TLS string lifecycle
// behave exactly as a game using libsteamclient would experience.
extern "C" void* wn_get_isteam_friends();
JNIEXPORT jboolean JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticSetRichPresence(
        JNIEnv* env, jclass /*cls*/, jstring jKey, jstring jValue) {
    void* obj = wn_get_isteam_friends();
    if (!obj || !jKey) return JNI_FALSE;
    std::string key   = jstr(env, jKey);
    std::string value = jstr(env, jValue);
    long* vt = *reinterpret_cast<long**>(obj);
    using SetFn = bool (*)(void*, const char*, const char*);
    auto fn = reinterpret_cast<SetFn>(vt[43]);
    return fn(obj, key.c_str(),
              value.empty() ? nullptr : value.c_str()) ? JNI_TRUE : JNI_FALSE;
}

// Diagnostic — reads friend_persona_states[steamId] directly from the
// open-source .so's pushed state (bypasses bootstrap, which requires
// g_state.initialized). Returns the EPersonaState (uint32) or -1 if
// the friend isn't in the map.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetFriendPersonaState(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId) {
    uint64_t sid = static_cast<uint64_t>(jSteamId);
    auto& p = lsc::pushed();
    std::lock_guard<std::mutex> lk(lsc::state_mutex());
    auto it = p.friend_persona_states.find(sid);
    if (it == p.friend_persona_states.end()) return -1;
    return static_cast<jint>(it->second);
}

// Diagnostic — returns the per-CSteamID rich-presence key count
// (slot 46 of ISteamFriends). Used to verify the read surface lands
// on the same map the write surface populated.
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticRichPresenceKeyCount(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId) {
    void* obj = wn_get_isteam_friends();
    if (!obj) return 0;
    long* vt = *reinterpret_cast<long**>(obj);
    using CountFn = int (*)(void*, uint64_t);
    auto fn = reinterpret_cast<CountFn>(vt[46]);
    return fn(obj, static_cast<uint64_t>(jSteamId));
}

// Push the raw avatar-hash bytes for a friend (typically 20 bytes —
// the SHA-1 over the CDN-stored avatar). Called by wn-session when a
// CMsgClientPersonaState arrives with EClientPersonaStateFlag_Avatar
// set. The hash isn't enough to render — a separate HTTPS fetch
// against avatars.akamai.steamstatic.com is required — but storing it
// here lets:
//   1. Games' GetFriendPersonaState callbacks know an avatar is
//      pending (PersonaStateChange_t with kPersonaChangeAvatar)
//   2. The Kotlin avatar-fetcher coroutine know which friends have a
//      hash worth fetching
//
// Idempotent: hash byte-compare against stored value; only emit the
// PersonaStateChange_t on actual content change (matches Steam — no
// redundant callbacks on identical refreshes).
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetFriendAvatarHash(
        JNIEnv* env, jclass /*cls*/, jlong jSteamId, jbyteArray jHash) {
    uint64_t sid = static_cast<uint64_t>(jSteamId);
    if (sid == 0) return;
    bool changed = false;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto& slot = p.friend_avatar_hashes[sid];
        if (!jHash) {
            if (!slot.empty()) { slot.clear(); changed = true; }
        } else {
            jsize n = env->GetArrayLength(jHash);
            std::vector<uint8_t> bytes(static_cast<size_t>(n));
            if (n > 0) {
                env->GetByteArrayRegion(jHash, 0, n,
                    reinterpret_cast<jbyte*>(bytes.data()));
            }
            if (bytes != slot) {
                slot = std::move(bytes);
                changed = true;
            }
        }
    }
    if (!changed) return;
    cb::PersonaStateChange payload{};
    payload.m_ulSteamID    = sid;
    payload.m_nChangeFlags = cb::kPersonaChangeAvatar;
    lsc::push_callback(lsc::state().user.load(),
                       cb::kPersonaStateChange,
                       &payload, sizeof(payload));
}

// Diagnostic — returns the stored avatar-hash hex (lowercase, no
// separator) for the friend, or empty string if no hash is known.
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetFriendAvatarHashHex(
        JNIEnv* env, jclass /*cls*/, jlong jSteamId) {
    uint64_t sid = static_cast<uint64_t>(jSteamId);
    std::string hex;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        auto it = p.friend_avatar_hashes.find(sid);
        if (it != p.friend_avatar_hashes.end()) {
            static constexpr char kHex[] = "0123456789abcdef";
            hex.reserve(it->second.size() * 2);
            for (uint8_t b : it->second) {
                hex.push_back(kHex[(b >> 4) & 0xF]);
                hex.push_back(kHex[b & 0xF]);
            }
        }
    }
    return env->NewStringUTF(hex.c_str());
}

// Push an avatar image for a friend (or self). Tier is 0/1/2 for
// small/medium/large; any other value is rejected. Allocates a fresh
// monotonic handle, stores the RGBA bytes under image_registry, and
// updates friend_avatars[steamID].{small,medium,large}. Emits
// AvatarImageLoaded_t so games waiting on the callback proceed to
// render. Returns the allocated handle (0 on bad args).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativePushFriendAvatar(
        JNIEnv* env, jclass /*cls*/,
        jlong jSteamId, jint jTier, jint jWidth, jint jHeight, jbyteArray jRgba) {
    if (jSteamId == 0 || jWidth <= 0 || jHeight <= 0 || !jRgba) return 0;
    if (jTier < 0 || jTier > 2) return 0;
    jsize n = env->GetArrayLength(jRgba);
    int expected = jWidth * jHeight * 4;
    if (n != expected) return 0;

    int32_t handle;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        handle = p.next_image_handle++;
        auto& img = p.image_registry[handle];
        img.width  = jWidth;
        img.height = jHeight;
        img.rgba.resize(static_cast<size_t>(n));
        env->GetByteArrayRegion(jRgba, 0, n,
            reinterpret_cast<jbyte*>(img.rgba.data()));
        auto& a = p.friend_avatars[static_cast<uint64_t>(jSteamId)];
        switch (jTier) {
            case 0: a.small  = handle; break;
            case 1: a.medium = handle; break;
            case 2: a.large  = handle; break;
        }
    }
    cb::AvatarImageLoaded ev{};
    ev.m_steamID = static_cast<uint64_t>(jSteamId);
    ev.m_iImage  = handle;
    ev.m_iWide   = jWidth;
    ev.m_iTall   = jHeight;
    lsc::push_callback(lsc::state().user.load(),
                       cb::kAvatarImageLoaded,
                       &ev, sizeof(ev));
    return handle;
}

// Diagnostic — round-trips ISteamFriends slot 34/35/36 (Get{Small,
// Medium,Large}FriendAvatar) → ISteamUtils slot 5 (GetImageSize).
// [tier] is 0/1/2; any other value returns 0. Packs into a single
// jlong: hi32 = handle, lo32 = (width<<16)|height. lo32==0 = unknown
// dimensions.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetTieredAvatarSize(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId, jint jTier) {
    if (jTier < 0 || jTier > 2) return 0;
    void* friends = wn_get_isteam_friends();
    if (!friends) return 0;
    int slot = 34 + jTier;  // 34=small, 35=medium, 36=large
    long* vt_f = *reinterpret_cast<long**>(friends);
    using GetAv = int (*)(void*, uint64_t);
    auto get_av = reinterpret_cast<GetAv>(vt_f[slot]);
    int handle = get_av(friends, static_cast<uint64_t>(jSteamId));
    if (handle <= 0) return 0;
    void* utils = wn_get_isteam_utils();
    if (!utils) return (static_cast<int64_t>(handle) << 32);
    long* vt_u = *reinterpret_cast<long**>(utils);
    using SizeFn = bool (*)(void*, int, uint32_t*, uint32_t*);
    auto fn_size = reinterpret_cast<SizeFn>(vt_u[5]);
    uint32_t w = 0, h = 0;
    if (!fn_size(utils, handle, &w, &h)) return (static_cast<int64_t>(handle) << 32);
    uint32_t lo = (w << 16) | (h & 0xFFFF);
    return (static_cast<int64_t>(handle) << 32) | lo;
}

// Diagnostic — round-trips ISteamFriends slot 34 (GetSmallFriendAvatar)
// → ISteamUtils slot 5 (GetImageSize) on the stub vtables. Packs the
// result into a single jlong: hi32 = handle, lo32 = (width<<16)|height.
// width/height of 0 (lo32==0) means GetImageSize returned false.
JNIEXPORT jlong JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetSmallAvatarSize(
        JNIEnv* /*env*/, jclass /*cls*/, jlong jSteamId) {
    void* friends = wn_get_isteam_friends();
    if (!friends) return 0;
    long* vt_f = *reinterpret_cast<long**>(friends);
    using GetAv = int (*)(void*, uint64_t);
    auto get_small = reinterpret_cast<GetAv>(vt_f[34]);
    int handle = get_small(friends, static_cast<uint64_t>(jSteamId));
    if (handle <= 0) return 0;
    void* utils = wn_get_isteam_utils();
    if (!utils) return (static_cast<int64_t>(handle) << 32);
    long* vt_u = *reinterpret_cast<long**>(utils);
    using SizeFn = bool (*)(void*, int, uint32_t*, uint32_t*);
    auto fn_size = reinterpret_cast<SizeFn>(vt_u[5]);
    uint32_t w = 0, h = 0;
    bool ok = fn_size(utils, handle, &w, &h);
    if (!ok) return (static_cast<int64_t>(handle) << 32);
    uint32_t lo = (w << 16) | (h & 0xFFFF);
    return (static_cast<int64_t>(handle) << 32) | lo;
}

// Diagnostic — reads RGBA bytes for an image handle via slot 6
// (GetImageRGBA). Returns number of bytes copied (or 0 on failure).
JNIEXPORT jint JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeDiagnosticGetImageRGBA(
        JNIEnv* env, jclass /*cls*/, jint jHandle, jbyteArray jOut) {
    if (jHandle <= 0 || !jOut) return 0;
    void* utils = wn_get_isteam_utils();
    if (!utils) return 0;
    jsize n = env->GetArrayLength(jOut);
    std::vector<uint8_t> tmp(static_cast<size_t>(n));
    long* vt = *reinterpret_cast<long**>(utils);
    using RgbaFn = bool (*)(void*, int, uint8_t*, int);
    auto fn = reinterpret_cast<RgbaFn>(vt[6]);
    if (!fn(utils, jHandle, tmp.data(), n)) return 0;
    env->SetByteArrayRegion(jOut, 0, n, reinterpret_cast<jbyte*>(tmp.data()));
    return n;
}

// Emit GameOverlayActivated_t (callback id 731) with the given active
// flag. Game-side SDK consumers register for this to pause/unpause —
// the canonical pattern is `if (active) PauseGame(); else ResumeGame();`.
// Re-entry is harmless because the callback queue de-duplicates by
// snapshot at dispatch time, not by content.
//
// State coherence: we also stash the latest active state in the
// pushed-state cache (g_overlay_active atomic) so a late-registering
// consumer can poll-and-sync via GetSteamFriends or a dedicated query
// if/when one is added. The SDK doesn't expose a getter for this in
// public headers, so the field is currently emission-only.
JNIEXPORT void JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativeSetGameOverlayActive(
        JNIEnv* /*env*/, jclass /*cls*/, jboolean jActive) {
    bool active = (jActive == JNI_TRUE);
    auto& p = lsc::pushed();
    // Idempotency: only emit on state transitions, not on redundant
    // sets. SDK clients tolerate either, but de-duping at the source
    // keeps the queue clean and matches Steam's actual behavior of
    // emitting once per overlay open/close transition.
    bool prev = p.overlay_active.exchange(active);
    if (prev == active) return;
    cb::GameOverlayActivated ev{};
    ev.m_bActive = active;
    lsc::push_callback(lsc::state().user.load(),
                       cb::kGameOverlayActivated,
                       &ev, sizeof(ev));
}

// Poll the next ActivateGameOverlay* request enqueued by the
// ISteamFriends overlay slots and hand it to Kotlin as a String.
// Returns null when the queue is empty. The Kotlin side parses the
// "kind:arg1|sid|appid" delimited form and fires Intent.ACTION_VIEW
// for "webpage" / "store" / "user" entries (the latter two synthesize
// canonical Steam Community URLs from the SteamID / appid).
//
// Delimited form: <kind>\x01<arg1>\x01<sid_dec>\x01<app_id_dec>
//   - kind ∈ {"webpage", "store", "user", "invite", "dialog"}
//   - arg1: URL / dialog name (may be empty)
//   - sid_dec: decimal uint64 SteamID (0 if N/A)
//   - app_id_dec: decimal uint32 AppID (0 if N/A)
JNIEXPORT jstring JNICALL
Java_com_winlator_cmod_feature_stores_steam_wnsteam_WnLibSteamClient_nativePollOverlayRequest(
        JNIEnv* env, jclass /*cls*/) {
    lsc::PushedState::OverlayRequest r;
    {
        auto& p = lsc::pushed();
        std::lock_guard<std::mutex> lk(lsc::state_mutex());
        if (p.overlay_request_queue.empty()) return nullptr;
        r = std::move(p.overlay_request_queue.front());
        p.overlay_request_queue.pop_front();
    }
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf),
        "%s\x01%s\x01%llu\x01%u",
        r.kind.c_str(),
        r.arg1.c_str(),
        static_cast<unsigned long long>(r.sid),
        r.app_id);
    if (n <= 0) return nullptr;
    return env->NewStringUTF(buf);
}

}  // extern "C"
