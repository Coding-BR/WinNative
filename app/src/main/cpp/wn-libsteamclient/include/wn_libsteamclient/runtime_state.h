// Process-singleton state for the Steam pipe/user lifecycle. Mirrors
// Valve's flat-C ABI:
//
//   Steam_CreateSteamPipe()            → HSteamPipe (int)
//   Steam_BReleaseSteamPipe(pipe)      → bool
//   Steam_ConnectToGlobalUser(pipe)    → HSteamUser (int)
//   Steam_CreateLocalUser(&pipe, type) → HSteamUser
//   Steam_CreateGlobalUser(&pipe)      → HSteamUser (writes pipe slot)
//   Steam_ReleaseUser(pipe, user)      → void
//   Steam_LogOn / Steam_LogOff
//   Steam_BLoggedOn / Steam_BConnected
//   Steam_BGetCallback / Steam_FreeLastCallback
//
// Real Steam has an elaborate per-pipe/per-user object graph; for our
// initial drop-in we keep one global pipe + one global user (sufficient
// for the bootstrap's single-account use case and the typical Bionic
// game's SteamAPI_Init pattern). Multi-pipe support can land later.
//
// Thread safety: all entry points serialize on `state_mutex()`. The
// callback queue is its own mutex inside this header.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wn_libsteamclient {

// HSteam* handles are int per Valve's public types (Steamworks SDK).
// 0 = invalid. Real Steam uses small positive ints; we mirror that.
using HSteamPipe = int;
using HSteamUser = int;

struct CallbackMsg {
    int                   user;
    int                   id;
    std::vector<uint8_t>  body;
};

// Pushed-in state — populated by Kotlin via JNI setters
// (Java_…_WnLibSteamClient_*). The interface stubs read these for the
// values they return to callers, so a `wn_steam_set_steam_id(76561…)`
// call makes `ISteamUser.GetSteamID()` start returning that value
// without any vtable change. Thread-safe via the same state_mutex().
struct PushedState {
    std::atomic<uint64_t>   steam_id{0};
    std::atomic<uint32_t>   account_id{0};
    std::atomic<int>        persona_state{0};    // EPersonaState (0=Offline … 7=Invisible)
    std::atomic<uint32_t>   app_id{0};            // for ISteamUtils.GetAppID
    std::atomic<int>        ip_country_set{0};
    std::atomic<uint32_t>   server_realtime{0};       // unix epoch reported by CM at the anchor moment
    std::atomic<int64_t>    server_realtime_anchor_local_ms{0}; // local steady_clock::now() ms when server_realtime was captured
    std::string             persona_name;        // guarded by state_mutex()
    std::string             ip_country;          // guarded by state_mutex()
    std::string             ui_language;         // guarded by state_mutex()

    // App ownership + install state — populated from Kotlin (Room DB)
    // via the JNI setters. `owned_apps` powers ISteamApps.BIsSubscribedApp;
    // `installed_apps` powers BIsAppInstalled; `app_install_dirs` powers
    // GetAppInstallDir. All guarded by state_mutex().
    std::unordered_set<uint32_t>           owned_apps;
    std::unordered_set<uint32_t>           installed_apps;
    std::unordered_map<uint32_t,std::string> app_install_dirs;
    // Per-app active beta branch name. Empty / missing = public branch.
    // Populated by SteamService at game-launch time from WnLibrary's
    // selected_beta_name (mirrors Steam Library's "Properties → Betas").
    // Powers ISteamApps.GetCurrentBetaName (slot 15) — anti-cheat /
    // multiplayer matchmaking gate on this to segregate beta branches.
    std::unordered_map<uint32_t,std::string> app_current_beta;
    // Per-app active download progress. Present (and total > 0) only
    // while a download (full install / update / DLC fetch) is actively
    // in flight; cleared once the download terminates. Powers
    // ISteamApps.GetDlcDownloadProgress (slot 22). Updated by
    // SteamService's depot-downloader progress callbacks.
    struct DlProgress {
        uint64_t bytes_downloaded = 0;
        uint64_t bytes_total      = 0;
    };
    std::unordered_map<uint32_t,DlProgress> app_dl_progress;
    // Per-app local cloud remote directory. The "remote" subdirectory
    // under .../userdata/<acct>/<appId>/ where Steam mirrors cloud
    // files locally. Pushed by SteamService when a game launches
    // (resolved via PathManager.RemoteDir). Powers
    // ISteamRemoteStorage.FileWrite/FileRead/FileDelete which back-
    // load file bytes onto disk and the wn-session sync layer picks
    // them up on its next batch.
    std::unordered_map<uint32_t,std::string> app_cloud_remote_dirs;
    // Per-app boolean flags driven from PICS-extended fields or
    // wn-session library snapshots. Powers ISteamApps slot 1
    // (BIsLowViolence — apps with a low-violence content variant on
    // their region) and slot 3 (BIsVACBanned — user is VAC-banned for
    // this app's anti-cheat). Default not-set → false (matches SDK
    // contract).
    std::unordered_set<uint32_t> app_low_violence;
    std::unordered_set<uint32_t> app_vac_banned;
    // Account-level boolean flags driven from CMsgClientAccountInfo at
    // logon. Powers ISteamUser slots 26-29 (phone/2FA family) — the
    // store-overlay marketplace flow and family-share UI gate on these.
    // Atomic for lock-free reads since they're scalar.
    std::atomic<bool>   account_phone_verified{false};
    std::atomic<bool>   account_two_factor_enabled{false};
    std::atomic<bool>   account_phone_identifying{false};
    std::atomic<bool>   account_phone_requires_verification{false};
    // Apps the game has flagged for content revalidation via
    // ISteamApps.MarkContentCorrupt. The wn-session depot downloader
    // can poll this set and schedule a depot-integrity sweep — even
    // without that hook, simply tracking the request lets us return
    // true from slot 16 (matches SDK contract: "the request was
    // accepted") instead of silently dropping it.
    std::unordered_set<uint32_t>           apps_marked_corrupt;

    // Per-app subscribed-and-installed Workshop items. Keyed by appId
    // for fast game-launch swap; each entry is a map of
    // PublishedFileId → install info. Pushed by SteamService from the
    // existing WorkshopModsGenerator pipeline at game launch (the
    // staging dir lists ids the user subscribed to and the manifest
    // downloader has completed). Powers ISteamUGC slots 70-75
    // (GetNumSubscribedItems, GetSubscribedItems, GetItemState,
    // GetItemInstallInfo, GetItemDownloadInfo, DownloadItem) so mod-
    // supporting games (Skyrim, RimWorld, Cities Skylines, The Forest,
    // every Source-engine title) can enumerate installed mods at
    // boot rather than seeing an empty Workshop folder.
    struct WorkshopItemInfo {
        std::string install_dir;        // absolute Windows path or wine guest path
        uint64_t    size_bytes = 0;     // total disk footprint for ISteamUGC slot 73 bytes
        uint32_t    timestamp  = 0;     // unix32 last-update — slot 73 timestamp
        bool        installed  = true;  // currently we only ever push installed entries
    };
    std::unordered_map<uint32_t, std::unordered_map<uint64_t, WorkshopItemInfo>>
        subscribed_workshop_items;

    // ISteamInventory item-definition table (slot 20 LoadItemDefinitions,
    // 21 GetItemDefinitionIDs, 22 GetItemDefinitionProperty). Keyed by
    // appId so per-game swaps are atomic on launch — each entry is
    // (itemDefId → property map). The property map mirrors the items.json
    // schema gbe_fork understands: keys like "name", "type", "tradable",
    // "marketable", "icon_url", "price", etc. All values are strings,
    // matching the SDK contract that GetItemDefinitionProperty returns
    // strings (callers convert at the call site).
    //
    // Populated at game launch by SteamService reading the same JSON
    // archive `InventoryItemsGenerator` already produces for the
    // ColdClient `steam_settings/items.json`. Empty entry = no item
    // catalog (F2P games then show an empty store rather than crash on
    // a nullptr dereference).
    std::unordered_map<uint32_t,
        std::unordered_map<int32_t, std::unordered_map<std::string, std::string>>>
        inventory_item_defs;

    // ISteamMatchmaking lobby cache.
    //
    // active_lobbies holds the per-lobby state mirror that the
    // ClientMMSLobbyData server-push observer maintains. Powers
    // GetLobbyOwner, GetNumLobbyMembers, GetLobbyMemberLimit,
    // GetLobbyData, GetLobbyDataCount, GetLobbyDataByIndex, and the
    // join-side LobbyEnter_t population.
    //
    // lobby_match_list is the most-recent RequestLobbyList response,
    // indexed by GetLobbyByIndex.
    struct LobbyMember {
        std::string                                      persona_name;
        std::unordered_map<std::string, std::string>     data;
    };
    struct LobbyState {
        uint32_t                                         app_id        = 0;
        uint64_t                                         owner_sid     = 0;
        int32_t                                          max_members   = 0;
        int32_t                                          lobby_type    = 0;
        int32_t                                          lobby_flags   = 0;
        bool                                             joinable      = true;
        uint32_t                                         game_server_ip   = 0;
        uint16_t                                         game_server_port = 0;
        uint64_t                                         game_server_sid  = 0;
        std::unordered_map<std::string, std::string>     data;
        std::unordered_map<uint64_t, LobbyMember>        members;
    };
    std::unordered_map<uint64_t, LobbyState> active_lobbies;
    std::vector<uint64_t>                    lobby_match_list;

    // Per-lobby chat ring (bounded — drop oldest at 1024 entries).
    // Powers ISteamMatchmaking.GetLobbyChatEntry. m_iChatID values
    // handed to LobbyChatMsg_t callbacks are positions in this ring;
    // games call GetLobbyChatEntry(lobby, m_iChatID, ...) to retrieve
    // the body bytes.
    struct LobbyChatEntry {
        uint64_t                sender_sid;
        uint8_t                 chat_type;     // EChatEntryType (1=ChatMsg)
        std::vector<uint8_t>    body;
    };
    std::unordered_map<uint64_t, std::vector<LobbyChatEntry>>
        lobby_chat_buffer;

    // ISteamNetworking P2P session bookkeeping.
    //
    // active_p2p_sessions tracks the local side's view of each peer's
    // session: created when SendP2PPacket goes out for the first time,
    // when ReceiveP2PPacket lands one, or when AcceptP2PSessionWithUser
    // is called. SDK contract for GetP2PSessionState is to report
    // existence + last-known transport info even after a session has
    // gone idle, so we keep entries until CloseP2PSessionWithUser.
    //
    // p2p_inbound_queue is the FIFO for IsP2PPacketAvailable /
    // ReadP2PPacket. Filled by the transport layer (CM-relayed via
    // CMsgClientP2PConnectionInfo + CMsgClientP2PPacketRelayed once
    // we add it, or by a direct-UDP fallback). The queue is per-
    // channel so games with multiple traffic classes don't see them
    // interleaved.
    struct P2PSessionState {
        uint64_t  last_session_error   = 0;  // EP2PSessionError on close
        bool      connection_active    = false;
        bool      connecting           = false;
        uint32_t  bytes_queued_for_send = 0;
        uint32_t  remote_ip            = 0;  // little-endian / IPv4
        uint16_t  remote_port          = 0;
        bool      using_relay          = false;
    };
    std::unordered_map<uint64_t, P2PSessionState> active_p2p_sessions;

    struct P2PInboundPacket {
        uint64_t              sender_sid;
        int32_t               channel;
        std::vector<uint8_t>  body;
    };
    // Keyed by channel for fast per-channel polling; SDK semantics
    // require channel-aware peek (IsP2PPacketAvailable takes nChannel).
    std::unordered_map<int32_t, std::deque<P2PInboundPacket>> p2p_inbound_queue;
    // Atomic flag SDK contract: AllowP2PPacketRelay(true) lets Steam
    // fallback to relay through its servers when direct connection
    // fails. We support relay-only for now (no direct path), so the
    // flag is recorded but always-on effectively.
    std::atomic<bool> p2p_relay_allowed{true};

    // ISteamFriends overlay-activation requests (slot 28..33). The game
    // runs inside Wine + box64 — it can't fire an Android Intent itself
    // to open the system browser. ActivateGameOverlayTo{WebPage,Store,
    // User,InviteDialog} therefore enqueues a small JSON request here;
    // SteamService polls nativePollOverlayRequest from a Kotlin
    // background coroutine and converts each entry into Intent.ACTION
    // _VIEW with the matching URL. Bounded at 32 entries to avoid
    // games that spam Activate* in a loop filling memory; oldest drops.
    struct OverlayRequest {
        std::string kind;   // "webpage" | "store" | "user" | "invite" | "dialog"
        std::string arg1;   // URL / dialog name
        uint64_t    sid     = 0;   // user SID / lobby SID (depending on kind)
        uint32_t    app_id  = 0;   // store appid
    };
    std::deque<OverlayRequest> overlay_request_queue;
    // Per-friend Steam profile XP level. Populated from
    // CSteamID-keyed responses to CPlayer_GetSteamLevel; default 0
    // means "not yet known" (matches SDK contract — slot 10 returns 0
    // for unknown / never-fetched).
    std::unordered_map<uint64_t,int32_t>   friend_steam_levels;
    // Per-SteamID user-overridden nickname. SDK contract for slot 11
    // GetPlayerNickname: returns the local user's custom name override
    // for that account (or null when none). Pushed via local UI flows
    // (Steam Community → Edit Nickname) — we don't read this from CM
    // yet, so the map will be empty unless a game has explicitly set
    // one via cm_bridge.set_player_nickname (not yet implemented).
    std::unordered_map<uint64_t,std::string> player_nicknames;
    // Self Steam profile XP level — pushed from CPlayer.GetSteamLevel
    // response keyed on the signed-in steam_id. Powers ISteamUser.
    // GetPlayerSteamLevel (slot 24). 0 = not yet queried.
    std::atomic<int32_t>                   self_player_level{0};
    // Self per-game badge levels — game id → badge tier (0=none,
    // 1=foil basic, 5=foil master). Populated by the Steam Community
    // badge-overview JSON ingest. Powers ISteamUser.GetGameBadgeLevel.
    std::unordered_map<int32_t,int32_t>    self_game_badges;

    // Per-package license metadata pushed from CMClient's
    // CMsgClientLicenseList ingest via the reactive license-list bridge
    // observer. Powers ISteamApps.BIsSubscribedFromFamilySharing (slot
    // 27) — owner_id != self_account_id means the package was shared
    // by a family member. Also the substrate for future per-app
    // earliest-purchase time queries (slot 8) once a package→app
    // resolver is wired (today driven through SteamService's library
    // snapshot collector, not yet mirrored here).
    struct LicenseEntry {
        uint32_t package_id    = 0;
        uint32_t owner_id      = 0;       // AccountID; != self_account → family-shared
        uint32_t time_created  = 0;       // unix32 of purchase
        uint32_t license_type  = 0;       // ELicenseType
        uint32_t flags         = 0;       // ELicenseFlags bitfield
        int32_t  change_number = 0;       // PICS change_number on this package
        int32_t  minute_limit  = 0;       // 0 = unlimited; >0 = timed-trial cap (minutes)
        int32_t  minutes_used  = 0;       // current playtime against minute_limit
    };
    std::unordered_map<uint32_t,LicenseEntry> licenses;

    // Per-app source-package list — which packages grant access to this
    // app. Populated by SteamService from WnLibrary snapshot's PICS
    // resolution (each app entry carries source_packages from
    // CMsgClientLicenseList × PICS package→apps mapping). Used to
    // resolve app→license metadata: ISteamApps.GetEarliestPurchaseUnix
    // Time (slot 8) → min(time_created) across source packages;
    // BIsSubscribedFromFreeWeekend (slot 9) → any package with
    // license_type==FreeWeekend (ELicenseType=11).
    std::unordered_map<uint32_t,std::vector<uint32_t>> app_source_packages;
    // Per-app DLC entries. Powers ISteamApps.GetDLCCount (slot 10) +
    // BGetDLCDataByIndex (slot 11). Populated at game-launch time from
    // wn-steam-client's library snapshot (extended.listofdlc PICS field).
    // name may be empty for DLC whose PICS metadata hasn't been
    // resolved yet — slot 11 still surfaces an empty buffer with
    // pcbAvailable=true so the DLC counts toward iteration.
    struct DlcEntry {
        uint32_t    app_id;
        std::string name;
        bool        available = true;  // DLC currently purchasable
    };
    std::unordered_map<uint32_t,std::vector<DlcEntry>> app_dlcs;
    // Per-app installed depot ids. Powers ISteamApps.GetInstalledDepots
    // (slot 17) — games that verify install integrity (Source-engine
    // sv_pure check, Anti-Cheat depot-list audits) iterate this.
    std::unordered_map<uint32_t,std::vector<uint32_t>> app_installed_depots;
    // Per-app human-readable name. Populated from the Room app cache
    // (and wn-session's PICS library snapshot once Workshop / library
    // browsing wires it in). Powers ISteamAppList.GetAppName (slot 2).
    std::unordered_map<uint32_t,std::string>          app_names;
    // Per-app PICS public-branch buildid. Powers ISteamApps.GetAppBuildId
    // (slot 23). Sourced from wn-session's library snapshot
    // (depots.branches.public.buildid). 0 when the app has no public
    // branch (e.g. DLC apps that don't ship their own depots) or PICS
    // hasn't resolved it yet.
    std::unordered_map<uint32_t,uint32_t>             app_build_ids;
    // Friend list — list of CSteamID64 the user is friends with.
    // Powers ISteamFriends.GetFriendCount / GetFriendByIndex.
    std::vector<uint64_t>                  friends;
    std::unordered_map<uint64_t,std::string> friend_persona_names;
    // Per-friend EPersonaState (0=Offline 1=Online 2=Busy 3=Away
    // 4=Snooze 5=LookingToTrade 6=LookingToPlay 7=Invisible). Populated
    // alongside friend_persona_names from the CMsgClientPersonaState
    // observer; ISteamFriends.GetFriendPersonaState (slot 6) reads it.
    std::unordered_map<uint64_t,uint32_t>  friend_persona_states;
    // Per-friend game_played_app_id (0 when not in-game). Powers
    // ISteamFriends.GetFriendGamePlayed (slot 8) so games can show
    // "Friend X playing Y" overlays.
    std::unordered_map<uint64_t,uint32_t>  friend_game_played_app;

    // Rich-presence map: CSteamID64 → ordered (key,value) list.
    //
    // Keyed by CSteamID so the same map serves both the local user
    // (game writes via SetRichPresence) and friends (pushed by
    // wn-session from CMsgClientPersonaState). The inner type is
    // vector<pair> rather than unordered_map because
    // GetFriendRichPresenceKeyByIndex (slot 47) needs stable
    // iteration order matching insertion. Rich presence is small
    // (~10 keys/peer max per Steamworks guidance) so linear lookup
    // for SetRichPresence (key replacement) is fine.
    //
    // SetRichPresence semantics:
    //   • key == nullptr/empty → no-op
    //   • value == nullptr/empty → remove key
    //   • else → insert or update (linear search)
    //
    // ClearRichPresence: erase entry for self steam_id.
    using RichPresenceMap = std::vector<std::pair<std::string,std::string>>;
    std::unordered_map<uint64_t, RichPresenceMap> rich_presence;

    // Avatar pipeline.
    //
    // Image handles are monotonically allocated positive ints; 0 means
    // "no avatar registered" (which ISteamFriends.Get*FriendAvatar
    // returns to indicate "request fired, not yet ready"). We don't
    // reuse handles — once allocated, a handle is permanent for the
    // process lifetime, so existing callers can keep ints stale
    // without reading garbage.
    //
    // ImageEntry holds w/h plus the raw RGBA8 bytes (4 bytes per
    // pixel — Steam's GetImageRGBA contract). Lookup is keyed by
    // handle through image_registry.
    //
    // friend_avatars maps CSteamID64 to its three tier handles
    // (small=32x32, medium=64x64, large=184x184 per Steam's docs;
    // any size is accepted — games read the actual dimensions via
    // GetImageSize).
    struct ImageEntry {
        int32_t              width  = 0;
        int32_t              height = 0;
        std::vector<uint8_t> rgba;  // size = width*height*4
    };
    std::unordered_map<int32_t, ImageEntry>  image_registry;
    // Per-friend handles by size tier. 0 = not loaded.
    struct FriendAvatarHandles {
        int32_t small  = 0;
        int32_t medium = 0;
        int32_t large  = 0;
    };
    std::unordered_map<uint64_t, FriendAvatarHandles> friend_avatars;
    // Monotonic next-handle counter; advanced inside state_mutex().
    int32_t next_image_handle = 1;

    // Raw avatar-hash bytes (20 bytes SHA-1 per Steam's avatar-CDN
    // scheme) keyed by CSteamID. Populated by wn-session from
    // CMsgClientPersonaState. A downstream fetch (HTTPS to
    // avatars.akamai.steamstatic.com/<hex>_{small,medium,large}.jpg)
    // then decodes into the image_registry above and emits
    // AvatarImageLoaded_t. Stored as raw bytes so identity comparisons
    // are O(20) memcmp; the hex form is only synthesized at JNI
    // accessors. Empty bytes = no hash known yet.
    std::unordered_map<uint64_t, std::vector<uint8_t>> friend_avatar_hashes;

    // Cloud / RemoteStorage — populated from wn-session's cloud APIs
    // (Cloud.EnumerateUserFiles) and PrefManager. Powers
    // ISteamRemoteStorage.{IsCloudEnabledFor{Account,App},
    // GetFileCount, GetFileNameAndSize, GetQuota, FileExists, GetFileSize}.
    // Scalars are atomic for lock-free reads; the file list is guarded
    // by state_mutex().
    // Account-level Cloud defaults to true — that's Steam's documented
    // setting (off only when the user has explicitly disabled Cloud in
    // the Steam Client UI, which we don't currently surface). Prevents
    // the early-boot race where games querying IsCloudEnabledFor
    // Account before the Kotlin push lands saw `false` and disabled
    // their cloud-save subsystem permanently (some games cache this
    // on first call in their own SDK wrapper).
    std::atomic<bool>     cloud_enabled_account{true};
    // App-level Cloud STAYS at `false` until bindAppCloudState
    // succeeds. Games without cloud config (free demos, dedicated
    // servers, library tools) must not see IsCloudEnabledForApp=true
    // — they'd try cloud writes against zero quota and either error
    // out or spam retries. The Kotlin push (SteamService.bindAppCloud
    // State → setCloudEnabledForApp(true)) flips this when wn-session
    // confirms the bound app actually has cloud config.
    std::atomic<bool>     cloud_enabled_app{false};
    std::atomic<uint64_t> cloud_quota_total{0};
    std::atomic<uint64_t> cloud_quota_available{0};
    struct CloudFileEntry {
        std::string name;
        int32_t     size      = 0;   // ISteamRemoteStorage uses int32 here
        int64_t     timestamp = 0;   // unix seconds
    };
    std::vector<CloudFileEntry>            cloud_files;

    // ISteamUserStats — achievement schema + progress + stats values for
    // the currently-bound app. Populated from wn-session's
    // CMsgClientGetUserStatsResponse cache via JNI setters. Powers:
    //   slot 14 GetNumAchievements
    //   slot 15 GetAchievementName(idx)
    //   slot  6 GetAchievement(name)
    //   slot  9 GetAchievementAndUnlockTime(name)
    //   slot 11 GetAchievementIcon(name) — returns icon-handle int
    //   slot 12 GetAchievementDisplayAttribute(name, "name"/"desc"/"hidden")
    //   slot  1 GetStatInt(name)
    //   slot  2 GetStatFloat(name)
    // All maps + vector guarded by state_mutex().
    struct AchievementEntry {
        std::string  api_name;       // internal name ("ACH_FIRST_BLOOD")
        // Per-locale display strings. Steam's UserGameStatsSchema VDF
        // stores a `{english,spanish,...}` map; we preserve it so
        // GetAchievementDisplayAttribute can honor the runtime UI
        // language with sane fallback chain (ui_language → english →
        // any). Empty maps == legacy "schema not localized" path —
        // the single-locale push at nativeSetAchievementSchema time
        // populates the "english" key for backward compat.
        std::unordered_map<std::string, std::string> display_names;
        std::unordered_map<std::string, std::string> descriptions;
        std::string  icon;           // icon URL or empty
        bool         hidden        = false;
        bool         achieved      = false;
        uint32_t     unlock_time   = 0;   // unix seconds
        int32_t      icon_handle   = 0;   // synthetic id for slot-11
        // True if this achievement was unlocked since the last
        // StoreStats() commit. StoreStats() iterates the dirty set
        // and emits one UserAchievementStored_t per entry, then
        // clears the flag. SetAchievement only flips this on a true
        // false→true transition; setting an already-unlocked
        // achievement does NOT re-emit (matching SDK behavior).
        bool         pending_store = false;
        // CMsgClientStoreUserStats2 bit-pack mapping. Each achievement
        // is a single bit (`bit_index`) within the int32 stat
        // identified by `block_id`. Populated from the
        // UserGameStatsSchema VDF `stats[].bits[]` block on the
        // Kotlin side (StatsAchievementsGenerator emits
        // ProcessingResult.nameToBlockBit). block_id == -1 means
        // \"not bit-mapped\" — the schema didn't include this
        // achievement in any stat's bits block (legacy data, or a
        // schema we haven't fully ingested). StoreStats skips these
        // for the CM upload but still emits the local callbacks.
        int32_t      block_id      = -1;
        int32_t      bit_index     = 0;
    };
    std::vector<AchievementEntry>          achievements;
    // Index by api_name → vector index for O(1) lookup. Rebuilt on
    // every schema push.
    std::unordered_map<std::string,size_t> achievement_index;
    std::unordered_map<std::string,int32_t> stats_int;
    std::unordered_map<std::string,float>   stats_float;
    // Schema-side name→numeric-id map for stats. Same role as the
    // per-achievement block_id mapping but for direct stat writes.
    // CMsgClientStoreUserStats2's Stat entry is (uint32 stat_id,
    // uint32 stat_value); without this map, SetStatInt/Float can't
    // construct a valid upload from the api-name. Populated from
    // ProcessingResult.stats[i].id on the Kotlin side via
    // nativeSetStatIds.
    std::unordered_map<std::string,uint32_t> stat_name_to_id;
    // Dirty sets for SetStatInt / SetStatFloat. Modified-since-last-
    // StoreStats() entries land here; StoreStats walks them, resolves
    // each to a stat_id via stat_name_to_id, packs the values into
    // CMsgClientStoreUserStats2 alongside bit-pack achievement
    // updates, then clears. Cleared on schema re-push (new app
    // context starts fresh).
    std::unordered_set<std::string>         dirty_stats_int;
    std::unordered_set<std::string>         dirty_stats_float;

    // Per-stat accumulator state for ISteamUserStats.UpdateAvgRateStat
    // (slot 5). The visible stats_float[name] is the running average
    // (total_count / total_time); these hidden accumulators track the
    // cumulative inputs so successive UpdateAvgRateStat calls produce
    // the correct rolling rate. Stored as doubles to avoid float-add
    // drift over many session updates.
    struct AvgRateAccum {
        double total_count = 0.0;
        double total_time  = 0.0;
    };
    std::unordered_map<std::string,AvgRateAccum> stats_avg_rate;
    // True once a stats-response for the bound app has been ingested —
    // ISteamUserStats.RequestCurrentStats reports this so callers
    // know data is queryable.
    std::atomic<bool>                      stats_ready{false};

    // Latest Steam-overlay active state pushed via Kotlin's
    // setGameOverlayActive(). Used for transition de-duplication
    // (GameOverlayActivated_t emits once per false↔true transition,
    // not per redundant set). Emission-only — the SDK doesn't expose
    // a public getter for this in modern headers.
    std::atomic<bool>                      overlay_active{false};

    // Auth-session ticket cache. Modern Steam games call
    // ISteamUser.GetAuthSessionTicket to obtain a per-session token
    // they hand to the multiplayer server, which validates it via
    // BeginAuthSession. We track outstanding tickets so EndAuthSession
    // / CancelAuthTicket can clean up, and so a future real-ticket
    // backend (wn-steam-client's CMsgClientAuthList round-trip) can
    // populate the bytes from the CM. For now the bytes are a small
    // synthetic header — enough to look non-empty to the SDK shim.
    struct AuthTicket {
        uint32_t               h_ticket;       // returned to caller
        uint32_t               app_id;
        std::vector<uint8_t>   body;
    };
    std::atomic<uint32_t>                  next_auth_ticket_handle{1};
    std::unordered_map<uint32_t, AuthTicket> auth_tickets;

    // ISteamApps surface — values games query through the bound app
    // context. launch_command_line is the game's argv-style string
    // (guarded by state_mutex via the same convention as ui_language);
    // app_is_family_shared signals "this app is owned by another
    // member of the user's Steam Family Share group" to slot 27.
    std::string                            launch_command_line;
    std::atomic<bool>                      app_is_family_shared{false};

    // Encrypted App Ticket cache. Steam returns per-app encrypted
    // tickets via RequestEncryptedAppTicket → CMsgClientRequestEncryptedApp
    // Ticket round-trip; the bytes get cached here keyed by app_id so a
    // subsequent GetEncryptedAppTicket call serves them back without a
    // second CM round-trip. encrypted_app_ticket_eresult records the
    // most recent fetch's outcome (1 OK, 2 Fail, etc.) — read by
    // ISteamUser.GetEncryptedAppTicket to decide whether to copy bytes.
    // Populated by either the in-process synthetic path (offline) or
    // the wn-session real-CM bridge (online).
    std::unordered_map<uint32_t, std::vector<uint8_t>> encrypted_app_tickets;
    std::atomic<int32_t>                   encrypted_app_ticket_eresult{0};
};

PushedState& pushed();

// CCallResult body — async-op response keyed by SteamAPICall_t. The
// SDK consumer either polls via Steam_GetAPICallResult (which copies
// the body bytes into a caller buffer) or registers a CCallResult and
// receives a dispatched Run(payload, bIOFailure, hCall) on the next
// SteamAPI_RunCallbacks pump. Definition lives here (before `State`)
// so the State::call_results_pending map can name it.
struct CallResultMsg {
    uint64_t              h_call;
    int                   callback_id;
    bool                  io_failure;
    std::vector<uint8_t>  body;
};

// One global pipe + one global user. Both 0 when unallocated.
struct State {
    std::atomic<HSteamPipe> pipe{0};
    std::atomic<HSteamUser> user{0};

    // logged_on is set true after a successful Steam_LogOn; cleared on
    // Steam_LogOff. Stage-1 of this module exposes it as a flag the
    // backend (when wired) can flip; stage-0 leaves it permanently
    // false so consumers see an unauthenticated session.
    std::atomic<bool>       logged_on{false};
    std::atomic<bool>       connected{false};

    std::mutex              callback_mu;
    std::deque<CallbackMsg> callback_queue;
    // Pinned payload of the last dequeued callback — Steam's API
    // contract has the param pointer returned by BGetCallback stay
    // valid until FreeLastCallback is called.
    std::vector<uint8_t>    last_param;

    // CallResults — outstanding async-op responses keyed by hCall.
    // call_results_mu guards both call_results_pending and
    // next_api_call_handle. push_call_result inserts; RunCallbacks
    // (dispatch) + Steam_GetAPICallResult (poll) read; both consume
    // on success.
    std::mutex                                    call_results_mu;
    std::unordered_map<uint64_t, CallResultMsg>   call_results_pending;
    uint64_t                                      next_api_call_handle = 1;
};

State& state();
std::mutex& state_mutex();

// Allocate / release the singleton handles. Returns the chosen handle
// (currently always 1 for both pipe and user) or 0 if already alive.
HSteamPipe alloc_pipe();
bool       release_pipe(HSteamPipe pipe);
HSteamUser alloc_global_user(HSteamPipe pipe);
void       release_user(HSteamPipe pipe, HSteamUser user);

// Push a callback message; consumed by Steam_BGetCallback in FIFO order.
void push_callback(int user, int id, const void* data, size_t n);

// Push an async result. The internal helper future async ops (e.g.
// async ServiceMethod responses) call to surface their answer. Stores
// in state().call_results_pending keyed by hCall; dispatched on the
// next SteamAPI_RunCallbacks pump, and/or consumed by
// Steam_GetAPICallResult / Steam_IsAPICallCompleted.
void push_call_result(uint64_t h_call, int callback_id,
                      const void* data, size_t n, bool io_failure);

// Allocate a fresh SteamAPICall_t handle. Starts at 1 and increments
// monotonically; never returns 0 (the SDK treats 0 as "not in flight").
uint64_t alloc_api_call_handle();

// Transition the global logged-on / connected flags and emit the
// matching SteamServersConnected_t (101) or SteamServersDisconnected_t
// (103) callback IF the value actually changes. Idempotent — calling
// with the current value is a no-op so concurrent code paths (the
// bootstrap's LogonWithRefreshToken AND SteamService.onWnLoggedOn
// → nativeSetLoggedOn) can't double-fire. EResult passed only for
// the disconnect side (defaults to 6 = NoConnection).
void set_logged_on(bool logged_on, int eresult_on_disconnect = 6);

}  // namespace wn_libsteamclient
