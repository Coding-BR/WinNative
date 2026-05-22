// C-ABI bridge into the live wn-steam-client CMClient instance for use
// from sibling .so libraries (libsteamclient.so) that link against
// libwnsteam.so but can't depend on the C++ class layout because they
// don't pull in OpenSSL / curl / IXWebSocket transitive headers.
//
// wn-session-jni registers the active CMClient via [wn_cm_bridge_set_active]
// when a session is created and clears it on destruction. Callers in
// libsteamclient.so dispatch through the C functions below; each one
// snapshots the registered weak_ptr to a strong reference under a
// shared_mutex, then invokes the corresponding CMClient method. No
// state owned by this header — purely a routing layer.
//
// Visibility: every exported function is marked `default` (overriding
// wnsteam's project-wide `-fvisibility=hidden`) so the dynamic linker
// resolves the libsteamclient.so → libwnsteam.so calls at load time.
//
// Lifetime: registration is by [`std::weak_ptr`] — the bridge doesn't
// extend the CMClient's lifetime. If the session is torn down while a
// libsteamclient.so call is in flight, the strong-ref snapshot keeps it
// alive only for the duration of the call. After that, weak_ptr::lock
// fails and subsequent calls return without effect.

#pragma once

#include <cstdint>
#include <memory>

namespace wn_steam {
class CMClient;

// Called from wn-session-jni when a CMClient is brought up / torn down.
// Both calls are idempotent; passing nullptr is equivalent to
// clear_active(). Internal weak_ptr is replaced wholesale (no list).
void wn_cm_bridge_set_active(std::shared_ptr<CMClient> client);
void wn_cm_bridge_clear_active();

}  // namespace wn_steam

// C entry points — callable from any .so that links libwnsteam.so.
// Return values: true = the active CMClient accepted the call;
// false = no active client or the call rejected the argument.
extern "C" {

// CMsgClientChangeStatus with the given EPersonaState (0=Offline
// 1=Online 2=Busy 3=Away 4=Snooze 5=LookingToTrade 6=LookingToPlay
// 7=Invisible). Negative is rejected.
__attribute__((visibility("default")))
bool wn_cm_set_persona_state(int32_t persona_state);

// CMsgClientChangeStatus with player_name set — Steam's "rename" path.
// `name` is required (null/empty rejected); `persona_state` is the
// EPersonaState to keep current (the CM API requires it on every
// ChangeStatus; passing 1 = Online is safe when the caller doesn't know
// the current state). The CM reply is server-pushed as
// CMsgClientPersonaState for self; CMClient's route_inbound_ caches
// it and self_persona() picks up the echoed name.
__attribute__((visibility("default")))
bool wn_cm_set_persona_name(const char* name, int32_t persona_state);

// CMsgClientRequestFriendData for one SteamID64. `flags` is an
// EClientPersonaStateFlag bitmask (1=PlayerName, 2=Status, 4=Gameid,
// 64=Avatar; 0 / negative defaults to the standard
// name|state|game|avatar set so callers don't have to know the bits).
// Reply arrives async as CMsgClientPersonaState which CMClient routes
// + caches; SteamService's persona observer re-pushes through
// libsteamclient.so's friend_persona setters so the read surface
// (GetFriendPersonaName etc.) sees the fresh values.
__attribute__((visibility("default")))
bool wn_cm_request_user_info(uint64_t steam_id, int32_t flags);

// Bulk variant — one CMsgClientRequestFriendData for an array of
// SteamID64s. CMClient batches them into a single round-trip; replies
// come back as one or more CMsgClientPersonaState pushes. Use this for
// friend-list lazy-load where iterating one-by-one through the single
// variant would burn N round-trips. `count` is the number of valid
// entries in `sids`; pass 0 → no-op, returns false. Same `flags`
// semantics as the single variant.
__attribute__((visibility("default")))
bool wn_cm_request_user_info_bulk(const uint64_t* sids, size_t count, int32_t flags);

// Reads the cached ownership ticket for `app_id` and copies the bytes
// into `out_buf` (capped at `max_len`). `*out_len` receives the actual
// ticket length on success (or the required length when max_len is too
// small — caller can re-call with a bigger buffer). Returns true on
// cache hit + at least partial copy; false when the cache has no entry
// or there's no active CMClient.
//
// SDK callers chain this with a 24-byte auth-session-ticket header to
// produce the buffer ISteamUser.GetAuthSessionTicket returns. This
// function does NOT fetch — pre-fetch is the live wn-session's job
// (CMClient::get_app_ownership_ticket fires CMsgClientGetAppOwnership
// Ticket and populates the cache on the response). Returning false
// when the cache is empty is the expected steady-state until pre-fetch
// runs.
__attribute__((visibility("default")))
bool wn_cm_get_cached_app_ownership_ticket(uint32_t app_id,
                                            uint8_t* out_buf,
                                            size_t max_len,
                                            size_t* out_len);

// Diagnostic-only — directly inject `bytes` into CMClient::tickets()
// as the cached ownership ticket for `app_id`. Lets test paths
// exercise the cache-hit branch of GetAuthSessionTicket without a
// real CM round-trip. Production code paths use CMClient::
// get_app_ownership_ticket which populates via a CMsgClientGetAppOwnership
// Ticket round-trip. Returns false when no active CMClient.
__attribute__((visibility("default")))
bool wn_cm_bridge_inject_test_ownership_ticket(uint32_t app_id,
                                                 const uint8_t* bytes,
                                                 size_t len);

// CMsgClientGamesPlayed — tell Steam the user is running `app_id` so
// friends see "Playing X" in their friend list / overlay. Pass 0 to
// clear (sends an empty games_played array — equivalent to "no games
// running right now"). Fire-and-forget; no-op when no active CMClient.
//
// Steam uses this broadcast to drive playtime accrual + presence; it's
// the same path real Steam.exe fires when launching any game. Without
// this, the user's friend list shows them as just "Online" rather than
// "Playing X" even though they're in a game.
__attribute__((visibility("default")))
bool wn_cm_notify_games_played(uint32_t app_id);

// Player.SetRichPresence#1 — broadcast the local user's full rich-
// presence map for `app_id` to Steam. Parallel arrays: `keys[i]` paired
// with `values[i]`. Empty arrays = clear the map. Fire-and-forget; the
// response is consumed internally + logged. Friends then see updated
// "in lobby X", "+connect 1.2.3.4:27015", "playing match" strings in
// their overlay. No-op when no active CMClient.
__attribute__((visibility("default")))
bool wn_cm_set_rich_presence(uint32_t app_id,
                              const char* const* keys,
                              const char* const* values,
                              size_t count);

// CMsgClientStoreUserStats2 — push the currently-modified achievement /
// stat values to Steam so the user's profile reflects them. Called
// from ISteamUserStats.StoreStats. Without this, achievements unlock
// only locally — they show in-game but never appear on the Steam
// profile or trigger the Steam-side popup overlay.
//
// `stat_ids` / `stat_values` are parallel arrays of length `count`.
// Stat ids are the 32-bit numeric identifiers from the achievement
// schema (NOT the api_name strings). `crc_stats` is the schema CRC32
// the .so received in CMsgClientGetUserStatsResponse. Fire-and-forget;
// the response (CMsgClientStoreUserStatsResponse) updates pushed-
// state's stats_ready bit. Returns false if not logged on or app_id
// is invalid.
__attribute__((visibility("default")))
bool wn_cm_store_user_stats(uint32_t app_id,
                              uint32_t crc_stats,
                              const uint32_t* stat_ids,
                              const uint32_t* stat_values,
                              size_t count);

// ----- Reactive callback bridge ---------------------------------------------
//
// libsteamclient.so registers an observer here that CMClient invokes when
// a CMsgClientPersonaState arrives — direct in-process callback, no Kotlin
// poll loop hop. The observer fires on the CM transport thread; observers
// must be quick (acquire state mutex, mutate pushed_state, queue any
// callbacks via the lock-free callback registry, return).
//
// Event POD — fields are pointer/length pairs to bytes inside CMClient's
// parse buffer; valid ONLY for the duration of the callback. Observers
// must copy any data they want to retain.
// Pointer-pair for a rich_presence KV entry inside WnCmPersonaEvent.
// Both pointers borrow into CMClient's parse buffer; valid only during
// the observer callback. Empty value = "key removed" per Steam's RP
// semantics.
struct WnCmRichPresenceKV {
    const char* key;
    const char* value;
};

struct WnCmPersonaEvent {
    uint64_t       sid;              // CSteamID64 (0 if message lacks one)
    uint32_t       persona_state;    // EPersonaState (0..7); UINT32_MAX = absent
    uint32_t       game_played_app;  // AppID; 0 = not in game
    const char*    name;             // UTF-8, null if absent
    const uint8_t* avatar_hash;      // SHA-1 (typically 20 bytes), null if absent
    size_t         avatar_hash_len;
    // Rich-presence map for this persona-state push (empty when not in
    // this slice). Each entry's key/value pointers borrow into the
    // CMClient parse buffer; observer must copy to retain. Vector is
    // packed at the field's end so the existing POD prefix layout is
    // backward-compatible (older observers ignore the field; new
    // observers iterate rp_pairs[0..rp_count)).
    const WnCmRichPresenceKV* rp_pairs;
    size_t                    rp_count;
};

typedef void (*WnCmPersonaObserverFn)(const WnCmPersonaEvent*);

// Register / clear the process-wide observer. Most recent registration
// wins (single-slot, atomic). Pass nullptr to clear.
__attribute__((visibility("default")))
void wn_cm_bridge_register_persona_observer(WnCmPersonaObserverFn fn);

// Internal — invoked by CMClient on each parsed friend entry in a
// CMsgClientPersonaState. No-op when no observer is registered.
__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_persona(const WnCmPersonaEvent* ev);

// ----- Logon-state observer -------------------------------------------------
//
// libsteamclient.so registers a callback that CMClient invokes whenever its
// ClientState transitions. Boolean: true = LoggedOn, false = anything else.
// Observer maps this to SteamServersConnected_t / SteamServersDisconnected_t
// emission (lock-release-then-emit; same re-entrancy pattern as the persona
// observer).

typedef void (*WnCmLogonStateObserverFn)(bool logged_on);

__attribute__((visibility("default")))
void wn_cm_bridge_register_logon_state_observer(WnCmLogonStateObserverFn fn);

// Internal — invoked by CMClient::set_state_locked_ on every state change.
__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_logon_state(bool logged_on);

// Diagnostic-only — synthetic dispatch without a real CMClient transition.
// Lets test paths exercise the observer dispatch path offline.
__attribute__((visibility("default")))
void wn_cm_bridge_inject_test_logon_state(bool logged_on);

// ----- Friends-list observer ------------------------------------------------
//
// CMClient invokes this whenever a CMsgClientFriendsList arrives —
// post-logon (full snapshot) + on every relationship change (incremental).
// Observer payload is the full current set of MUTUAL friend SIDs
// (relationship == 3). libsteamclient.so mirrors into pushed.friends so
// ISteamFriends.GetFriendCount / GetFriendByIndex return real data without
// Kotlin polling.
//
// Pointer lifetime: `sids` borrows into CMClient's locally-constructed
// vector for the dispatch call. Observer must copy if it retains.

typedef void (*WnCmFriendsListObserverFn)(const uint64_t* sids, size_t count);

__attribute__((visibility("default")))
void wn_cm_bridge_register_friends_list_observer(WnCmFriendsListObserverFn fn);

// Internal — invoked by CMClient on every CMsgClientFriendsList ingest.
__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_friends_list(const uint64_t* sids, size_t count);

// Diagnostic-only — synthetic friends-list dispatch.
__attribute__((visibility("default")))
void wn_cm_bridge_inject_test_friends_list(const uint64_t* sids, size_t count);

// ----- License-list observer ------------------------------------------------
//
// CMClient invokes this whenever a CMsgClientLicenseList arrives — initial
// post-logon snapshot + every license change (purchase, refund, gift).
// Observer payload is the full current license set as a parallel-array POD
// (package_id, owner_id, time_created, license_type, flags, change_number).
// libsteamclient.so mirrors into pushed.licenses for
// ISteamApps.BIsSubscribedFromFamilySharing + per-package metadata queries.

struct WnCmLicenseEntry {
    uint32_t package_id;
    uint32_t owner_id;
    uint32_t time_created;
    uint32_t license_type;
    uint32_t flags;
    int32_t  change_number;
    int32_t  minute_limit;     // 0 = unlimited
    int32_t  minutes_used;
};

typedef void (*WnCmLicenseListObserverFn)(const WnCmLicenseEntry* licenses,
                                            size_t count);

__attribute__((visibility("default")))
void wn_cm_bridge_register_license_list_observer(WnCmLicenseListObserverFn fn);

// Internal — invoked by CMClient on every CMsgClientLicenseList ingest.
__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_license_list(const WnCmLicenseEntry* licenses,
                                          size_t count);

// Diagnostic-only — synthetic license-list dispatch.
__attribute__((visibility("default")))
void wn_cm_bridge_inject_test_license_list(const WnCmLicenseEntry* licenses,
                                             size_t count);

// ----- Account-info observer ------------------------------------------------
//
// CMClient invokes this when CMsgClientAccountInfo (EMsg 768/2104) lands,
// post-logon. The flags drive ISteamUser slots 26-29 (BIsPhoneVerified /
// BIsTwoFactorEnabled / BIsPhoneIdentifying / BIsPhoneRequiringVerification).
// Without this, libsteamclient.so's pushed account flags stay at their
// startup defaults (false), and game-side marketplace / family-share /
// parental-controls flows silently see "not verified" for a verified user.
//
// Layout matches the four fields of the SDK contract:
//   persona_name              — protobuf field 1 (UTF-8, no NUL terminator)
//   ip_country                — protobuf field 2 (ISO 3166-1 alpha-2)
//   two_factor_enabled        — protobuf field 15 (two_factor_state) != 0
//   phone_verified            — protobuf field 17
//   phone_identifying         — protobuf field 19
//   phone_requires_verification — protobuf field 20
//
// persona_name / ip_country are owned by the caller (the CMClient ingest
// path) — observers must copy out of them inside the dispatch callback;
// the pointers do not outlive the dispatch.

struct WnCmAccountInfo {
    const char* persona_name;       // may be NULL if not provided
    size_t      persona_name_len;
    const char* ip_country;         // may be NULL if not provided
    size_t      ip_country_len;
    bool        two_factor_enabled;
    bool        phone_verified;
    bool        phone_identifying;
    bool        phone_requires_verification;
};

typedef void (*WnCmAccountInfoObserverFn)(const WnCmAccountInfo* info);

__attribute__((visibility("default")))
void wn_cm_bridge_register_account_info_observer(WnCmAccountInfoObserverFn fn);

// Internal — invoked by CMClient on every CMsgClientAccountInfo ingest.
__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_account_info(const WnCmAccountInfo* info);

// Diagnostic-only — synthetic dispatch.
__attribute__((visibility("default")))
void wn_cm_bridge_inject_test_account_info(const WnCmAccountInfo* info);

// Server-real-time observer — invoked once per CMsgClientLogonResponse
// (ok eresult) carrying the rtime32_server_time epoch the CM reported.
// Powers ISteamUtils.GetServerRealTime so games see a server-anchored
// timestamp instead of the local-clock fallback. Unix seconds.
typedef void (*WnCmServerRealTimeObserverFn)(uint32_t server_realtime);

__attribute__((visibility("default")))
void wn_cm_bridge_register_server_realtime_observer(WnCmServerRealTimeObserverFn fn);

__attribute__((visibility("default")))
void wn_cm_bridge_dispatch_server_realtime(uint32_t server_realtime);

// ---------------------------------------------------------------------------
// ISteamMatchmaking lobby browser (Phase A).
//
// All sid fields are full SteamID64. Filter ints follow SDK enums:
//   ELobbyComparison: -2 LEqual, -1 Less, 0 Equal, 1 Greater, 2 GEqual, 3 NotEqual
//   ELobbyFilterType: 0 String, 1 Numerical, 2 SlotsAvail, 3 NearValue, 4 Distance
// ---------------------------------------------------------------------------

typedef struct WnCmLobbyEntry {
    uint64_t steam_id;
    int32_t  max_members;
    int32_t  num_members;
    int32_t  lobby_type;
    int32_t  lobby_flags;
    int32_t  ping_ms;
    int64_t  weight;
    float    distance;
} WnCmLobbyEntry;

typedef struct WnCmLobbyMember {
    uint64_t       steam_id;
    const char*    persona_name;     // UTF-8, valid only during callback
    const uint8_t* metadata_bytes;
    size_t         metadata_len;
} WnCmLobbyMember;

typedef struct WnCmLobbyData {
    uint64_t                steam_id_lobby;
    uint64_t                steam_id_owner;
    uint32_t                app_id;
    int32_t                 max_members;
    int32_t                 num_members;
    int32_t                 lobby_type;
    int32_t                 lobby_flags;
    const uint8_t*          metadata_bytes;
    size_t                  metadata_len;
    const WnCmLobbyMember*  members;
    size_t                  member_count;
} WnCmLobbyData;

// On synthetic failure (timeout / disconnect / parse), eresult is negative.
// On success, eresult is the EResult from the server (1 = OK, 2 = Fail, etc.).
typedef void (*WnCmLobbyListCb)(uint64_t hCall,
                                int32_t eresult,
                                const WnCmLobbyEntry* lobbies,
                                size_t count);

typedef void (*WnCmLobbyDataObserverFn)(const WnCmLobbyData* data);

// Send CMsgClientMMSGetLobbyList. Returns false when CMClient is
// inactive / not-logged-on. Caller is responsible for firing a
// synthetic "no lobbies" callback to unblock the SDK CCallResult.
// filter_keys/values/comparisons/types are parallel arrays.
__attribute__((visibility("default")))
bool wn_cm_lobby_get_list(uint64_t hCall,
                          uint32_t app_id,
                          int32_t num_lobbies_requested,
                          const char* const* filter_keys,
                          const char* const* filter_values,
                          const int32_t* filter_comparisons,
                          const int32_t* filter_types,
                          size_t filter_count,
                          WnCmLobbyListCb cb);

__attribute__((visibility("default")))
void wn_cm_bridge_register_lobby_data_observer(WnCmLobbyDataObserverFn fn);

// CreateLobby — invokes cb with the new lobby SteamID on success or
// 0+negative eresult on failure. Returns false when CMClient is
// inactive (caller must synthesize the fail callback locally).
typedef void (*WnCmLobbyCreatedCb)(uint64_t hCall,
                                   int32_t eresult,
                                   uint64_t lobby_sid);
__attribute__((visibility("default")))
bool wn_cm_lobby_create(uint64_t hCall,
                        uint32_t app_id,
                        int32_t lobby_type,
                        int32_t max_members,
                        WnCmLobbyCreatedCb cb);

// JoinLobby — on success, the response contains the full member list
// + lobby metadata. The bridge marshals into a WnCmLobbyData* + invokes
// the LobbyDataObserver synthetically so the lobby cache is seeded
// before the LobbyEnter_t callback fires. Then the cb is invoked with
// the chat_room_enter_response (1=Success, 2=DoesNotExist, ...).
typedef void (*WnCmLobbyJoinedCb)(uint64_t hCall,
                                  int32_t chat_room_enter_response,
                                  uint64_t lobby_sid);
__attribute__((visibility("default")))
bool wn_cm_lobby_join(uint64_t hCall,
                      uint32_t app_id,
                      uint64_t lobby_sid,
                      WnCmLobbyJoinedCb cb);

// LeaveLobby — fire-and-forget. Returns false when CMClient is
// inactive; caller doesn't need to wait for any callback.
__attribute__((visibility("default")))
bool wn_cm_lobby_leave(uint32_t app_id, uint64_t lobby_sid);

// SetLobbyData — push KV-encoded metadata to Steam. After the
// SetLobbyDataResponse arrives, Steam separately pushes 6612
// LobbyData to every member; the existing on_lobby_data_event
// observer mirrors the new state into pushed().active_lobbies.
// The callback is purely "did the set succeed" — for "what's the
// new state" the game waits on the next LobbyDataUpdate_t.
//
// steam_id_member=0 → lobby-level data; nonzero → per-member data
// (only the calling user can set their own member data).
// metadata is the SDK's KV blob: null-terminated key, null-
// terminated value, repeated, terminated by an extra null byte.
typedef void (*WnCmLobbySetDataCb)(uint64_t hCall, int32_t eresult);
__attribute__((visibility("default")))
bool wn_cm_lobby_set_data(uint64_t hCall,
                          uint32_t app_id,
                          uint64_t lobby_sid,
                          uint64_t steam_id_member,
                          const uint8_t* metadata, size_t metadata_len,
                          int32_t max_members, int32_t lobby_type,
                          int32_t lobby_flags,
                          WnCmLobbySetDataCb cb);

// SendLobbyChatMsg — fire-and-forget. Steam relays to all lobby
// members as 6614 ClientMMSLobbyChatMsg push.
__attribute__((visibility("default")))
bool wn_cm_lobby_send_chat(uint32_t app_id, uint64_t lobby_sid,
                           const uint8_t* data, size_t n);

// SetLobbyOwner — host-only ownership transfer. EMsg 6615
// CMsgClientMMSSetLobbyOwner. Steam echoes 6612 LobbyData with the
// updated owner_sid so every member's GetLobbyOwner re-reads.
typedef void (*WnCmLobbySetOwnerCb)(uint64_t hCall, int32_t eresult);
__attribute__((visibility("default")))
bool wn_cm_lobby_set_owner(uint64_t hCall,
                           uint32_t app_id,
                           uint64_t lobby_sid,
                           uint64_t new_owner_sid,
                           WnCmLobbySetOwnerCb cb);

// InviteToLobby — fire-and-forget invite. EMsg 6621
// CMsgClientMMSInviteToLobby. Steam routes the notification to the
// invitee's online client.
__attribute__((visibility("default")))
bool wn_cm_lobby_invite_user(uint32_t app_id,
                             uint64_t lobby_sid,
                             uint64_t invitee_sid);

// Chat-msg push observer. Carries sender SID + bytes.
typedef void (*WnCmLobbyChatMsgObserverFn)(uint64_t lobby_sid,
                                           uint64_t sender_sid,
                                           const uint8_t* data,
                                           size_t n);
__attribute__((visibility("default")))
void wn_cm_bridge_register_lobby_chat_msg_observer(WnCmLobbyChatMsgObserverFn fn);

// Membership push observer — fired for both ClientMMSUserJoinedLobby
// (6619) and ClientMMSUserLeftLobby (6620). joined flag distinguishes
// them. Powers LobbyChatUpdate_t emission (callback 506).
typedef void (*WnCmLobbyMembershipObserverFn)(int32_t joined /*1=joined,0=left*/,
                                               uint64_t lobby_sid,
                                               uint64_t user_sid,
                                               const char* persona_name);
__attribute__((visibility("default")))
void wn_cm_bridge_register_lobby_membership_observer(WnCmLobbyMembershipObserverFn fn);

// Start the cross-process state-sync poller thread. Polls
// ${WN_STATE_DIR}/wn_lobby_req_<appid>.txt every 1s; for each request,
// invokes wn_cm_lobby_get_list against the in-process CMClient. The
// writer side of that call mirrors the lobby list to
// ${WN_STATE_DIR}/wn_lobby_<appid>.txt where a wine-side reader (no
// in-process CMClient) is polling.
//
// Spawned from steam_bootstrap.cpp's nativeInit after the callback
// pump thread starts. Idempotent (subsequent calls are no-ops).
__attribute__((visibility("default")))
void wn_cm_bridge_start_state_sync_poller(void);

}  // extern "C"
