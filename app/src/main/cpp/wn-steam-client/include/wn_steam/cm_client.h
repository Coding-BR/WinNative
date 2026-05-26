#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wn_steam/cmsg_protobuf_header.h"
#include "wn_steam/emsg.h"
#include "wn_steam/encrypted_channel.h"
#include "wn_steam/heartbeat.h"
#include "wn_steam/job_manager.h"
#include "wn_steam/pb/ccloud.h"
#include "wn_steam/pb/cinventory.h"
#include "wn_steam/pb/cpublishedfile.h"
#include "wn_steam/pb/ccontentserverdirectory.h"
#include "wn_steam/pb/cfamilygroups.h"
#include "wn_steam/pb/cplayer.h"
#include "wn_steam/pb/cmsg_client_get_app_ownership_ticket.h"
#include "wn_steam/pb/cmsg_client_request_encrypted_app_ticket.h"
#include "wn_steam/pb/cmsg_client_get_user_stats.h"
#include "wn_steam/pb/cmsg_client_store_user_stats.h"
#include "wn_steam/pb/cmsg_client_games_played.h"
#include "wn_steam/pb/cmsg_client_kick_playing_session.h"
#include "wn_steam/pb/cmsg_client_change_status.h"
#include "wn_steam/pb/cmsg_client_persona.h"
#include "wn_steam/pb/cmsg_client_mms_get_lobby_list.h"
#include "wn_steam/pb/cmsg_client_mms_lobby_data.h"
#include "wn_steam/pb/cmsg_client_mms_lobby_ops.h"
#include "wn_steam/pb/cmsg_client_friends_list.h"
#include "wn_steam/pb/cmsg_client_playing_session_state.h"
#include "wn_steam/pb/cmsg_client_get_depot_decryption_key.h"
#include "wn_steam/pb/cmsg_client_pics.h"
#include "wn_steam/transport.h"
#include "wn_steam/wine_bridge.h"
#include "wn_steam/wn_library_store.h"
#include "wn_steam/wn_ticket_cache.h"
#include <optional>
#include <unordered_map>

namespace wn_steam {

enum class ClientState : uint8_t {
    Disconnected,    // no transport / no channel
    Connecting,      // transport / handshake in progress
    Connected,       // encrypted channel established, not yet logged on
    LoggedOn,        // CMsgClientLogonResponse OK
};

// Thread-safe Steam CM client.
// Callbacks fire on the transport worker thread; do not block them.
class CMClient {
public:
    using StateCallback = std::function<void(ClientState)>;
    using ClientMessageCallback = std::function<void(EMsg emsg,
                                                     const CMsgProtoBufHeader& header,
                                                     std::span<const uint8_t> body)>;

    CMClient();
    ~CMClient();

    CMClient(const CMClient&)            = delete;
    CMClient& operator=(const CMClient&) = delete;

    void set_ca_bundle_path(const std::string& path);

    [[nodiscard]] bool connect(const std::string& url);

    void disconnect();

    // Send `CMsgClientLogOff` (EMsg::ClientLogOff = 706, empty body),
    // then disconnect. Use this instead of bare `disconnect()` when
    // the next code path on the same account will open a NEW Steam
    // session (the hybrid hand-off — wn-session passing the logon to
    // libsteamclient.so). A bare socket close leaves the session
    // "alive" on Steam's side for ~5 min until heartbeat timeout, so
    // a subsequent LogonWithRefreshToken with the same token hits
    // Steam's concurrent-login anti-abuse (EResult=15 AccessDenied,
    // observed on-device 2026-05-19 commit 90361f6). The clean
    // logoff makes Steam clear the session immediately. Best-effort:
    // queues the send, gives the channel a brief window to flush,
    // then disconnects regardless of response.
    void log_off_and_disconnect(std::chrono::milliseconds flush_window =
                                    std::chrono::milliseconds(500));

    [[nodiscard]] ClientState state() const noexcept { return state_.load(); }
    [[nodiscard]] uint64_t    steam_id() const noexcept { return steam_id_.load(); }
    [[nodiscard]] int32_t     session_id() const noexcept { return session_id_.load(); }
    [[nodiscard]] uint64_t    family_group_id() const noexcept { return family_group_id_.load(); }

    // Continuation fires on response, timeout, or disconnect.
    void call_service_method(std::string_view method_name,
                             bool authed,
                             std::span<const uint8_t> request_body,
                             JobContinuation cb,
                             std::chrono::seconds timeout = std::chrono::seconds{30});

    // routing_appid routes app-scoped messages to the right Steam backend.
    [[nodiscard]] bool send_proto_message(EMsg emsg,
                                          std::span<const uint8_t> body,
                                          uint32_t routing_appid = 0);

    [[nodiscard]] bool logon_with_refresh_token(
        const std::string& refresh_token,
        const std::string& account_name = "",
        uint64_t client_supplied_steam_id = 0);

    using PicsAccessTokenCallback =
        std::function<void(std::optional<pb::CMsgClientPICSAccessTokenResponse>)>;
    using PicsProductInfoCallback =
        std::function<void(std::optional<pb::CMsgClientPICSProductInfoResponse>)>;

    // Required for non-public package/app info.
    void pics_get_access_tokens(std::vector<uint32_t> packageids,
                                std::vector<uint32_t> appids,
                                PicsAccessTokenCallback cb,
                                std::chrono::seconds timeout = std::chrono::seconds{30});

    // Multi-part PICS responses are merged before the callback fires.
    void pics_get_product_info(std::vector<pb::PicsPackageInfoReq> packages,
                               std::vector<pb::PicsAppInfoReq> apps,
                               bool meta_data_only,
                               PicsProductInfoCallback cb,
                               std::chrono::seconds timeout = std::chrono::seconds{60});

    using PicsChangesSinceCallback =
        std::function<void(std::optional<pb::CMsgClientPICSChangesSinceResponse>)>;
    void pics_get_changes_since(uint32_t since_change_number,
                                PicsChangesSinceCallback cb,
                                std::chrono::seconds timeout = std::chrono::seconds{30});

    // Pre-warm app, DLC, and tickets before launch.
    using PrepareAppCallback = std::function<void(bool ok, std::string error)>;
    void prepare_app(uint32_t app_id,
                     std::vector<uint32_t> dlc_app_ids,
                     PrepareAppCallback cb,
                     std::chrono::seconds timeout = std::chrono::seconds{30});

    void notify_games_played(const pb::CMsgClientGamesPlayed& msg);

    // Player.SetRichPresence#1 — broadcast the local user's rich-presence
    // map for `app_id`. Fire-and-forget service method (no callback);
    // Steam pushes a CMsgClientPersonaState for self echoing the new
    // RP set, which CMClient's route_inbound_ + persona_observer
    // mirror back into pushed_state. No-op unless LoggedOn.
    void set_rich_presence(uint32_t app_id,
                            const std::vector<pb::CPlayer_SetRichPresence_KV>& kv);

    // CMsgClientKickPlayingSession — release this account's other active
    // playing session. Fire-and-forget; no-op unless LoggedOn.
    void kick_playing_session(bool only_stop_game);

    void set_persona_state(uint32_t persona_state);

    // CMsgClientChangeStatus carrying both persona_state AND a new
    // player_name — Steam's "rename" path. The CM doesn't ack
    // explicitly, but the server-pushed CMsgClientPersonaState for
    // self that follows will echo the new name (cached by
    // route_inbound_); self_persona() picks it up. Fire-and-forget;
    // no-op unless LoggedOn.
    void set_persona_name(const std::string& name,
                          uint32_t persona_state_keep_current = 1 /*Online*/);

    // CMsgClientRequestFriendData — request persona data for the local user.
    // The reply arrives async as a server-pushed CMsgClientPersonaState,
    // which route_inbound_ caches; read it back via self_persona().
    void request_user_persona();

    // CMsgClientRequestFriendData — request persona data for an arbitrary
    // set of SteamID64s. `persona_state_requested` is an
    // EClientPersonaStateFlag bitmask (1=PlayerName, ~0 / 0xFFFF for the
    // standard set). Replies arrive async as CMsgClientPersonaState
    // pushes; read them back via friend_personas(). No-op when not
    // logged on. Empty list returns immediately.
    void request_friend_personas(const std::vector<uint64_t>& sids,
                                 uint32_t persona_state_requested = 1);

    // The local user's most recently received persona (name/avatar/game),
    // or nullopt if no CMsgClientPersonaState for our SteamID has arrived.
    [[nodiscard]] std::optional<pb::PersonaStateFriend> self_persona() const;

    [[nodiscard]] bool is_playing_blocked() const noexcept {
        return playing_blocked_.load();
    }

    // Makes the next wait observe a post-kick server push, not stale state.
    void mark_playing_blocked() noexcept { playing_blocked_.store(true); }

    // Cached post-logon licenses; empty until the push arrives.
    [[nodiscard]] std::vector<pb::License> license_list() const;

    // The most recent CMsgClientFriendsList — list of friend SteamID64s
    // (full snapshot; incremental updates are merged in). Empty until the
    // post-logon push has arrived. Powers WnLibSteamClient.setFriendsList
    // so ISteamFriends.GetFriendCount/ByIndex/Relationship returns real
    // data to in-game queries.
    [[nodiscard]] std::vector<uint64_t> friends_list() const;

    // Snapshot of friend personas observed from CMsgClientPersonaState
    // pushes. One entry per non-self SteamID64 for which we've received
    // a name (other fields can be empty). Used to push friend display
    // names into libsteamclient.so so ISteamFriends.GetFriendPersonaName
    // resolves real names instead of an empty string.
    struct FriendPersonaSnapshot {
        uint64_t             sid                 = 0;
        std::string          player_name;
        uint32_t             persona_state       = 0;
        uint32_t             game_played_app_id  = 0;
        // Raw SHA-1 (20 bytes) of the CDN avatar JPG. Empty when the
        // server hasn't included it in a CMsgClientPersonaState push.
        // The hex form goes out in nativeGetFriendPersonas's JSON so
        // Kotlin can route it to nativeSetFriendAvatarHash without a
        // separate CM round-trip.
        std::vector<uint8_t> avatar_hash;
    };
    [[nodiscard]] std::vector<FriendPersonaSnapshot> friend_personas() const;

    // FamilyGroups.GetFamilyGroup#1 — enumerate the members of the local
    // user's Steam Family. family_group_id comes from the logon response;
    // the callback gets the group name + member SteamIDs, or nullopt on
    // failure / not logged on.
    using FamilyGroupCallback = std::function<void(
        std::optional<pb::CFamilyGroups_GetFamilyGroup_Response>)>;
    void get_family_group(uint64_t family_group_id, FamilyGroupCallback cb,
                          std::chrono::seconds timeout = std::chrono::seconds{30});

    // Private libraries return an empty games list.
    using OwnedGamesCallback = std::function<void(
        std::optional<pb::CPlayer_GetOwnedGames_Response>)>;
    void get_owned_games(uint64_t steam_id, OwnedGamesCallback cb,
                         std::chrono::seconds timeout = std::chrono::seconds{30});

    void set_on_state(StateCallback cb);
    void set_on_client_message(ClientMessageCallback cb);

    // Disable for download-only sessions before logon.
    void set_auto_populate_library(bool enabled) noexcept {
        auto_populate_library_.store(enabled);
    }

    [[nodiscard]] WnLibraryStore& library() noexcept { return library_; }
    [[nodiscard]] const WnLibraryStore& library() const noexcept { return library_; }

    [[nodiscard]] WnTicketCache& tickets() noexcept { return tickets_; }
    [[nodiscard]] const WnTicketCache& tickets() const noexcept { return tickets_; }

    // Steam3Master + SteamClientService listeners for lsteamclient.dll.
    [[nodiscard]] WineBridge& wine_bridge() noexcept { return wine_bridge_; }
    [[nodiscard]] const WineBridge& wine_bridge() const noexcept { return wine_bridge_; }

    // Cached on success.
    using AppOwnershipTicketCallback =
        std::function<void(std::optional<pb::CMsgClientGetAppOwnershipTicketResponse>)>;
    void get_app_ownership_ticket(uint32_t app_id,
                                  AppOwnershipTicketCallback cb,
                                  std::chrono::seconds timeout = std::chrono::seconds{30});

    // Used for Goldberg online auth.
    using EncryptedAppTicketCallback =
        std::function<void(std::optional<pb::CMsgClientRequestEncryptedAppTicketResponse>)>;
    void request_encrypted_app_ticket(uint32_t app_id,
                                      EncryptedAppTicketCallback cb,
                                      std::chrono::seconds timeout = std::chrono::seconds{30});

    // Non-OK empty schemas still reach the caller for diagnostics.
    using UserStatsCallback =
        std::function<void(std::optional<pb::CMsgClientGetUserStatsResponse>)>;
    void get_user_stats(uint32_t app_id,
                        UserStatsCallback cb,
                        std::chrono::seconds timeout = std::chrono::seconds{30});

    // Fire-and-forget stat or achievement write-back.
    void store_user_stats(uint32_t app_id, uint64_t steam_id,
                          uint32_t crc_stats,
                          const std::vector<std::pair<uint32_t, uint32_t>>& stats);

    // ISteamMatchmaking lobby browser — single-shot request/response.
    // Sends CMsgClientMMSGetLobbyList (EMsg 6607); response carries an
    // array of lobby SteamIDs the game then probes via RequestLobbyData
    // for full metadata. Filters (key+value+comparison+filter_type) are
    // SDK-shape — caller builds them from
    // ISteamMatchmaking.AddRequestLobbyList*Filter slot calls. nullopt
    // on timeout / disconnect / parse failure.
    using LobbyListCallback = std::function<void(
        std::optional<pb::CMsgClientMMSGetLobbyListResponse>)>;
    void lobby_get_list(uint32_t app_id,
                        std::vector<pb::CMsgClientMMSGetLobbyListFilter> filters,
                        int32_t num_lobbies_requested,
                        LobbyListCallback cb,
                        std::chrono::seconds timeout = std::chrono::seconds{30});

    // Observer fired from route_inbound_ when an unsolicited
    // CMsgClientMMSLobbyData (EMsg 6612) arrives — Steam pushes these
    // whenever a lobby the client is subscribed to changes (metadata
    // edit, member join/leave, owner change). The observer copies
    // payload into pushed().active_lobbies in libsteamclient.so and
    // emits the SDK's LobbyDataUpdate_t callback. Registration is one-
    // shot at session init via cm_bridge.
    using LobbyDataObserver =
        std::function<void(const pb::CMsgClientMMSLobbyData&)>;
    void set_lobby_data_observer(LobbyDataObserver obs);

    // Host-control trio (Phase B). Each emits its corresponding
    // CMsgClientMMS* through the encrypted channel with a JobManager-
    // tracked continuation; on response the lambda deserializes the
    // proto + delivers std::optional<Response> to the caller.
    using LobbyCreateCallback = std::function<void(
        std::optional<pb::CMsgClientMMSCreateLobbyResponse>)>;
    void lobby_create(uint32_t app_id, int32_t lobby_type,
                      int32_t max_members, LobbyCreateCallback cb,
                      std::chrono::seconds timeout = std::chrono::seconds{30});

    using LobbyJoinCallback = std::function<void(
        std::optional<pb::CMsgClientMMSJoinLobbyResponse>)>;
    void lobby_join(uint32_t app_id, uint64_t lobby_sid,
                    LobbyJoinCallback cb,
                    std::chrono::seconds timeout = std::chrono::seconds{30});

    // Fire-and-forget — Steam acknowledges via 6606 LeaveLobbyResponse
    // but the SDK contract doesn't gate any callback on it.
    void lobby_leave(uint32_t app_id, uint64_t lobby_sid);

    // SetLobbyData (lobby-level OR per-member). steam_id_member=0 means
    // the data applies to the lobby itself; non-zero is a member-data
    // update. Steam server-echoes via 6610 SetLobbyDataResponse +
    // pushes 6612 ClientMMSLobbyData to every member with the new
    // metadata; the existing LobbyData observer mirrors it into
    // pushed().active_lobbies so all clients converge.
    using LobbySetDataCallback = std::function<void(
        std::optional<pb::CMsgClientMMSSetLobbyDataResponse>)>;
    void lobby_set_data(uint32_t app_id, uint64_t lobby_sid,
                        uint64_t steam_id_member,
                        std::vector<uint8_t> metadata,
                        int32_t max_members, int32_t lobby_type,
                        int32_t lobby_flags,
                        LobbySetDataCallback cb,
                        std::chrono::seconds timeout = std::chrono::seconds{30});

    // SendLobbyChatMsg (fire-and-forget). Steam relays to all members
    // as 6614 ClientMMSLobbyChatMsg pushes; the observer below mirrors
    // each one into pushed().lobby_chat_buffer and emits LobbyChatMsg_t.
    void lobby_send_chat(uint32_t app_id, uint64_t lobby_sid,
                         std::vector<uint8_t> chat_bytes);

    // SetLobbyOwner — host-only ownership transfer. EMsg 6615
    // CMsgClientMMSSetLobbyOwner → 6616 Response. Steam also pushes
    // 6612 LobbyData on success so every member's GetLobbyOwner
    // re-reads the new value; we don't need to mirror separately.
    using LobbySetOwnerCallback = std::function<void(
        std::optional<pb::CMsgClientMMSSetLobbyOwnerResponse>)>;
    void lobby_set_owner(uint32_t app_id, uint64_t lobby_sid,
                         uint64_t new_owner_sid,
                         LobbySetOwnerCallback cb,
                         std::chrono::seconds timeout = std::chrono::seconds{30});

    // InviteToLobby (fire-and-forget). EMsg 6621
    // CMsgClientMMSInviteToLobby. Steam pushes a notification to the
    // invitee's Steam client — overlay popup OR Friends chat (if
    // they're offline / not in-game). No response message; we just
    // confirm the CM channel accepted the bytes.
    void lobby_invite_user(uint32_t app_id, uint64_t lobby_sid,
                           uint64_t invitee_sid);

    // Server-push observers for the in-lobby state-sync surface.
    using LobbyChatMsgObserver =
        std::function<void(const pb::CMsgClientMMSLobbyChatMsg&)>;
    using LobbyMembershipObserver =
        std::function<void(bool joined,
                           const pb::CMsgClientMMSUserJoinedOrLeftLobby&)>;
    void set_lobby_chat_msg_observer(LobbyChatMsgObserver obs);
    void set_lobby_membership_observer(LobbyMembershipObserver obs);

    // Single-shot depot decryption key request (Phase 5 foundation).
    // Response carries the AES-256 key used to decrypt the depot's content
    // manifest and every CDN chunk. nullopt on timeout / disconnect /
    // parse failure; a non-OK eresult is still delivered so the caller
    // can distinguish "not owned" from "transport error".
    using DepotDecryptionKeyCallback =
        std::function<void(std::optional<pb::CMsgClientGetDepotDecryptionKeyResponse>)>;
    void get_depot_decryption_key(uint32_t depot_id, uint32_t app_id,
                                  DepotDecryptionKeyCallback cb,
                                  std::chrono::seconds timeout = std::chrono::seconds{30});

    // Public/empty branches omit app_branch to match JavaSteam.
    using ManifestRequestCodeCallback = std::function<void(
        std::optional<pb::CContentServerDirectory_GetManifestRequestCode_Response>)>;
    void get_manifest_request_code(uint32_t app_id, uint32_t depot_id,
                                   uint64_t manifest_id, std::string branch,
                                   ManifestRequestCodeCallback cb,
                                   std::chrono::seconds timeout = std::chrono::seconds{30});

    // cell_id 0 lets Steam pick.
    using CdnServersCallback = std::function<void(
        std::optional<pb::CContentServerDirectory_GetServersForSteamPipe_Response>)>;
    void get_cdn_servers(uint32_t cell_id, CdnServersCallback cb,
                         std::chrono::seconds timeout = std::chrono::seconds{30});

    // Phase 6 — Steam Cloud save sync.
    //
    // Cloud user quota: Cloud.GetUserQuota#1. Returns total + used
    // bytes for the signed-in account. Async — callback receives
    // nullopt on transport failure / not logged on. Default timeout
    // 30s. The result drives the libsteamclient.so pushed
    // cloud_quota_{total,available} state via the SteamService bridge
    // that calls pushQuotaToLibSteamClient on success.
    using CloudUserQuotaCallback = std::function<void(
        std::optional<pb::CCloud_GetUserQuota_Response>)>;
    void cloud_get_user_quota(CloudUserQuotaCallback cb,
                              std::chrono::seconds timeout = std::chrono::seconds{30});

    // Cloud file changelist: Cloud.GetAppFileChangelist#1. Lists the app's
    // remote cloud files relative to synced_change_number; pass 0 for the
    // full list (the restore path). The response's path_prefixes index lets
    // the caller resolve each file to its local OS path. nullopt on failure.
    using CloudFileChangelistCallback = std::function<void(
        std::optional<pb::CCloud_GetAppFileChangelist_Response>)>;
    void cloud_get_app_file_changelist(uint32_t app_id,
                                       uint64_t synced_change_number,
                                       CloudFileChangelistCallback cb,
                                       std::chrono::seconds timeout = std::chrono::seconds{30});

    // Item-definition digest for steam_settings/items.json.
    using ItemDefMetaCallback = std::function<void(
        std::optional<pb::CInventory_GetItemDefMeta_Response>)>;
    void inventory_get_item_def_meta(uint32_t app_id,
                                     ItemDefMetaCallback cb,
                                     std::chrono::seconds timeout = std::chrono::seconds{30});

    // One page of subscriptions; caller paginates using `total`.
    using PublishedFileUserFilesCallback = std::function<void(
        std::optional<pb::CPublishedFile_GetUserFiles_Response>)>;
    void published_file_get_subscribed(uint32_t app_id, uint32_t page,
                                       uint32_t num_per_page,
                                       PublishedFileUserFilesCallback cb,
                                       std::chrono::seconds timeout = std::chrono::seconds{30});

    // Resolves the HTTP URL and headers for a cloud file body.
    using CloudFileDownloadCallback = std::function<void(
        std::optional<pb::CCloud_ClientFileDownload_Response>)>;
    void cloud_get_file_download_info(uint32_t app_id, std::string filename,
                                      CloudFileDownloadCallback cb,
                                      std::chrono::seconds timeout = std::chrono::seconds{30});

    // Cloud upload is caller-driven: open batch, upload files, commit, complete.
    using CloudBeginBatchCallback = std::function<void(
        std::optional<pb::CCloud_BeginAppUploadBatch_Response>)>;
    void cloud_begin_app_upload_batch(uint32_t app_id, std::string machine_name,
                                      std::vector<std::string> files_to_upload,
                                      std::vector<std::string> files_to_delete,
                                      uint64_t client_id,
                                      CloudBeginBatchCallback cb,
                                      std::chrono::seconds timeout = std::chrono::seconds{30});

    using CloudBeginFileUploadCallback = std::function<void(
        std::optional<pb::CCloud_ClientBeginFileUpload_Response>)>;
    void cloud_begin_file_upload(uint32_t app_id, std::string filename,
                                 uint32_t file_size, uint32_t raw_file_size,
                                 std::vector<uint8_t> file_sha, uint64_t time_stamp,
                                 uint64_t upload_batch_id,
                                 CloudBeginFileUploadCallback cb,
                                 std::chrono::seconds timeout = std::chrono::seconds{30});

    using CloudCommitFileUploadCallback = std::function<void(
        std::optional<pb::CCloud_ClientCommitFileUpload_Response>)>;
    void cloud_commit_file_upload(bool transfer_succeeded, uint32_t app_id,
                                  std::vector<uint8_t> file_sha, std::string filename,
                                  CloudCommitFileUploadCallback cb,
                                  std::chrono::seconds timeout = std::chrono::seconds{30});

    using CloudCompleteBatchCallback = std::function<void(bool ok)>;
    void cloud_complete_app_upload_batch(uint32_t app_id, uint64_t batch_id,
                                         uint32_t batch_eresult,
                                         CloudCompleteBatchCallback cb,
                                         std::chrono::seconds timeout = std::chrono::seconds{30});

    // Empty pending-op list means clear to launch.
    using CloudAppLaunchIntentCallback = std::function<void(
        std::optional<pb::CCloud_AppLaunchIntent_Response>)>;
    void cloud_signal_app_launch_intent(uint32_t app_id, uint64_t client_id,
                                        std::string machine_name,
                                        bool ignore_pending_operations,
                                        int32_t os_type,
                                        CloudAppLaunchIntentCallback cb,
                                        std::chrono::seconds timeout = std::chrono::seconds{30});

    void cloud_signal_app_exit_sync_done(uint32_t app_id, uint64_t client_id,
                                         bool uploads_completed,
                                         bool uploads_required);

private:
    void on_channel_connected();
    void on_channel_disconnected(ChannelDisconnectReason r, const std::string& detail);
    void on_channel_message(std::span<const uint8_t> bytes);

    void set_state_locked_(ClientState s);
    void route_inbound_(EMsg emsg, const CMsgProtoBufHeader& header,
                        std::span<const uint8_t> body);

    void library_populate_step_();

    std::unique_ptr<EncryptedChannel> channel_;
    JobManager                        jobs_;
    Heartbeat                         heartbeat_;

    std::atomic<ClientState>          state_{ClientState::Disconnected};
    std::atomic<uint64_t>             steam_id_{0};
    std::atomic<int32_t>              session_id_{0};
    std::atomic<uint64_t>             family_group_id_{0};
    std::atomic<bool>                 auto_populate_library_{true};
    std::atomic<bool>                 playing_blocked_{false};

    mutable std::mutex                cb_mu_;
    StateCallback                     on_state_;
    ClientMessageCallback             on_client_message_;

    // ISteamMatchmaking server-push observer. Set once at session init
    // (cm_bridge wires libsteamclient.so's mirror); fired from
    // route_inbound_'s ClientMMSLobbyData arm.
    LobbyDataObserver                 lobby_data_observer_;
    LobbyChatMsgObserver              lobby_chat_msg_observer_;
    LobbyMembershipObserver           lobby_membership_observer_;

    // PICS product-info accumulator. Keyed by jobid_source/_target. When a
    // response carries response_pending=true, the partial result is merged
    // into the entry and the callback is NOT yet fired. The final part (with
    // the flag absent or false) merges and fires.
    struct PicsAggregate {
        pb::CMsgClientPICSProductInfoResponse acc;
        PicsProductInfoCallback               cb;
    };
    mutable std::mutex                                pics_mu_;
    std::unordered_map<uint64_t, PicsAggregate>       pics_pending_;

    WnLibraryStore                                    library_;
    WnTicketCache                                     tickets_;
    WineBridge                                        wine_bridge_;

    mutable std::mutex                                persona_mu_;
    std::optional<pb::PersonaStateFriend>             self_persona_;

    mutable std::mutex                                license_mu_;
    std::vector<pb::License>                          license_list_;

    // CMsgClientFriendsList friends (relationship == Friend only). Full
    // snapshot from the initial push; merged in place for incremental
    // updates. Keyed as a set for O(1) merge.
    mutable std::mutex                                friends_mu_;
    std::unordered_map<uint64_t, uint32_t>            friends_;  // sid → relationship

    // CMsgClientPersonaState friend personas (everyone except the local
    // user; self lands in self_persona_). Keyed by SteamID64.
    mutable std::mutex                                friend_personas_mu_;
    std::unordered_map<uint64_t, FriendPersonaSnapshot> friend_personas_;
};

}  // namespace wn_steam
