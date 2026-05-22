#pragma once

// Steamworks-SDK callback layouts. We replicate the exact structs games
// consume via Steam_BGetCallback so a byte-for-byte memcpy out of our
// queue matches whatever STEAM_CALLBACK declared on the game side.
//
// All structs use `#pragma pack(push,8)` semantics — that's how Valve's
// public headers (isteam*.h) emit callback structs. uint64_t naturally
// aligns to 8 under this; int32 stays at 4-byte alignment. Mismatches
// here surface as garbled callback bodies inside the game, so each
// struct gets static_assert size + offset guards.

#include <cstdint>
#include <cstddef>

namespace wn_libsteamclient::callbacks {

// k_iSteamUserCallbacks = 100
constexpr int kSteamServersConnected    = 101;
constexpr int kSteamServerConnectFailure = 102;
constexpr int kSteamServersDisconnected = 103;
constexpr int kIPCFailure               = 117;  // k_iSteamUserCallbacks + 17 — single-byte failure type
constexpr int kValidateAuthTicketResponse = 143; // k_iSteamUserCallbacks + 43
constexpr int kEncryptedAppTicketResponse = 154; // k_iSteamUserCallbacks + 54
constexpr int kGetAuthSessionTicketResponse = 163; // k_iSteamUserCallbacks + 63
constexpr int kGetTicketForWebApiResponse   = 168; // k_iSteamUserCallbacks + 68
constexpr int kStoreAuthURLResponse         = 165; // k_iSteamUserCallbacks + 65
constexpr int kMarketEligibilityResponse    = 166; // k_iSteamUserCallbacks + 66
constexpr int kDurationControl              = 167; // k_iSteamUserCallbacks + 67

// k_iSteamUtilsCallbacks = 700
constexpr int kSteamShutdown            = 704;  // k_iSteamUtilsCallbacks + 4 — empty marker
constexpr int kSteamAPICallCompleted    = 703;  // k_iSteamUtilsCallbacks + 3
constexpr int kCheckFileSignature       = 705;  // k_iSteamUtilsCallbacks + 5
// k_iSteamUserStatsCallbacks = 1100
constexpr int kLeaderboardFindResult         = 1104; // base + 4
constexpr int kLeaderboardScoresDownloaded   = 1105; // base + 5
constexpr int kLeaderboardScoreUploaded      = 1106; // base + 6
constexpr int kNumberOfCurrentPlayers        = 1107; // base + 7
constexpr int kGlobalAchievementPercentages  = 1110; // base + 10
constexpr int kLeaderboardUGCSet             = 1111; // base + 11
constexpr int kGlobalStatsReceived           = 1112; // base + 12
// k_iSteamFriendsCallbacks = 1300 (overlap with RemoteStorage range —
// SDK uses the same numbers; partition by struct semantics on the
// game side).
constexpr int kClanOfficerListResponse      = 1335; // base + 35
constexpr int kDownloadClanActivityCountsResult = 1341; // base + 41
constexpr int kJoinClanChatRoomCompletion   = 1342; // base + 42
constexpr int kFriendsGetFollowerCount      = 1344;
constexpr int kFriendsIsFollowing           = 1345;
constexpr int kFriendsEnumerateFollowingList = 1346;
constexpr int kEquippedProfileItems         = 1351; // base + 51
// k_iSteamRemoteStorageCallbacks = 1300
constexpr int kRemoteStorageSubscribePublishedFile   = 1313; // base + 13
constexpr int kRemoteStorageUnsubscribePublishedFile = 1315; // base + 15
constexpr int kRemoteStorageDownloadUGC              = 1317; // base + 17
// k_iClientUGCCallbacks = 3400
constexpr int kSteamUGCQueryCompleted        = 3401; // base + 1
constexpr int kSteamUGCRequestUGCDetails     = 3402; // base + 2
// k_iClientInventoryCallbacks = 4700
constexpr int kSteamInventoryEligiblePromoItemDefIDs = 4703;
constexpr int kSteamInventoryStartPurchaseResult     = 4704;
constexpr int kSteamInventoryRequestPricesResult     = 4705;
// k_iSteamMatchmakingCallbacks = 500
constexpr int kLobbyEnter                    = 504; // base + 4
constexpr int kLobbyMatchList                = 510; // base + 10
constexpr int kLobbyCreated                  = 513; // base + 13
constexpr int kGameOverlayActivated     = 731;  // k_iSteamUtilsCallbacks + 31 — bool m_bActive

// k_iSteamUserStatsCallbacks = 1100
constexpr int kUserStatsReceived = 1101;
constexpr int kUserStatsStored   = 1102;
constexpr int kUserAchievementStored = 1103;

// k_iSteamFriendsCallbacks = 1300
constexpr int kPersonaStateChange  = 1304;
constexpr int kSetPersonaNameResponse = 1332;
constexpr int kAvatarImageLoaded        = 1334;
constexpr int kFriendRichPresenceUpdate = 1336;

// k_iSteamRemoteStorageCallbacks = 1300 too (yes, same base; the
// callback id space is partitioned by struct semantics, not range).
// Differentiating by id alone is what the SDK does — callers register
// for one specific id via STEAM_CALLBACK and switch on that.
constexpr int kRemoteStorageAppSyncedClient = 1301;
constexpr int kRemoteStorageAppSyncedServer = 1302;
constexpr int kRemoteStorageFileWriteAsyncComplete = 1331; // base+31
constexpr int kRemoteStorageFileReadAsyncComplete  = 1332; // base+32
constexpr int kRemoteStorageFileShareResult        = 1307; // base+7
// FileDetailsResult lives under k_iSteamAppsCallbacks (1040), NOT
// RemoteStorage — it's ISteamApps.GetFileDetails for binary-integrity.
constexpr int kFileDetailsResult                   = 1063; // 1040 + 23

// EPersonaChange bitmask used by PersonaStateChange_t.m_nChangeFlags.
// Game-side overlays/friends-lists react to specific flags — name +
// status are the two we synthesize from local pushed-state events.
constexpr int kPersonaChangeName            = 0x0001;
constexpr int kPersonaChangeStatus          = 0x0002;
constexpr int kPersonaChangeComeOnline      = 0x0004;
constexpr int kPersonaChangeGoneOffline     = 0x0008;
constexpr int kPersonaChangeGamePlayed      = 0x0010;
constexpr int kPersonaChangeAvatar          = 0x0040;
constexpr int kPersonaChangeNameFirstSet    = 0x0400;
constexpr int kPersonaChangeNickname        = 0x1000;

// UserStatsReceived_t — emitted when a CMsgClientGetUserStatsResponse
// arrives (or, in our setter-driven model, when the schema is pushed
// into libsteamclient.so).
struct UserStatsReceived {
    uint64_t m_nGameID;
    int32_t  m_eResult;      // EResult: 1 = OK, 2 = Fail
    uint32_t _pad;           // pack=8 → uint64 at offset 16
    uint64_t m_steamIDUser;
};
static_assert(sizeof(UserStatsReceived) == 24, "UserStatsReceived size");
static_assert(offsetof(UserStatsReceived, m_nGameID)     == 0,  "off m_nGameID");
static_assert(offsetof(UserStatsReceived, m_eResult)     == 8,  "off m_eResult");
static_assert(offsetof(UserStatsReceived, m_steamIDUser) == 16, "off m_steamIDUser");

// UserStatsStored_t — emitted after ISteamUserStats.StoreStats writes
// back to Steam (in our case, when the game calls StoreStats; the
// actual CMsgClientStoreUserStats upload still owes a wn-session-side
// hook).
struct UserStatsStored {
    uint64_t m_nGameID;
    int32_t  m_eResult;
    uint32_t _pad;           // pack=8 trailing pad
};
static_assert(sizeof(UserStatsStored) == 16, "UserStatsStored size");
static_assert(offsetof(UserStatsStored, m_nGameID) == 0, "off m_nGameID");
static_assert(offsetof(UserStatsStored, m_eResult) == 8, "off m_eResult");

// SteamServersConnected_t — emitted when the local client transitions
// to a logged-on state with the Steam back-end. From isteamuser.h this
// struct is empty (a pure marker callback) — games gate online-only
// features on its receipt (multiplayer auth tickets, server browser,
// leaderboards). We push it with cubParam=0 / pubParam=nullptr.
struct SteamServersConnected {
    // Intentionally empty. C++ minimum sizeof = 1; consumers must not
    // read past Steam_BGetCallback's cubParam (which we emit as 0).
    char _placeholder;
};

// SteamServerConnectFailure_t — emitted on logon failure with the
// associated EResult. Size 8 (int + bool + trailing pack=8 pad).
struct SteamServerConnectFailure {
    int32_t m_eResult;
    bool    m_bStillRetrying;
    uint8_t _pad[3];
};
static_assert(sizeof(SteamServerConnectFailure) == 8, "SteamServerConnectFailure size");
static_assert(offsetof(SteamServerConnectFailure, m_eResult)        == 0, "off m_eResult");
static_assert(offsetof(SteamServerConnectFailure, m_bStillRetrying) == 4, "off m_bStillRetrying");

// IPCFailure_t — emitted when the IPC channel to the Steam client
// goes down unrecoverably. Games that handle it write state to disk
// and prompt the user to restart, instead of hanging forever waiting
// for a response that'll never come. Layout from isteamuser.h:
//   uint8 m_eFailureType
// (followed by 7 bytes of pack=8 trailing pad → struct size 8.)
struct IPCFailure {
    uint8_t m_eFailureType;
    uint8_t _pad[7];
};
static_assert(sizeof(IPCFailure) == 8, "IPCFailure size");
static_assert(offsetof(IPCFailure, m_eFailureType) == 0, "off m_eFailureType");
// EFailureType values (from isteamuser.h):
constexpr uint8_t kFailureFlushedCallbackQueue = 0;
constexpr uint8_t kFailurePipeFail             = 1;

// SteamShutdown_t — empty marker callback. The Steam client (our .so)
// is shutting down; games gate orderly cleanup on receipt.
struct SteamShutdown {
    char _placeholder;
};

// GameOverlayActivated_t — single-bool active state. The overlay is
// hosted out-of-process; we don't currently activate one, but the
// struct/id pair is here for completeness when a future overlay hook
// fires (or when a Wine-side overlay-bridge needs to forward the signal).
struct GameOverlayActivated {
    bool   m_bActive;
    uint8_t _pad[7];
};
static_assert(sizeof(GameOverlayActivated) == 8, "GameOverlayActivated size");
static_assert(offsetof(GameOverlayActivated, m_bActive) == 0, "off m_bActive");

// EncryptedAppTicketResponse_t — emitted async after Steam answers a
// RequestEncryptedAppTicket call. Layout from isteamuser.h:
//   EResult m_eResult @ 0  (int32)
// size = 4 (no trailing pad under pack=8 for an int-only struct).
struct EncryptedAppTicketResponse {
    int32_t m_eResult;
};
static_assert(sizeof(EncryptedAppTicketResponse) == 4, "EncryptedAppTicketResponse size");
static_assert(offsetof(EncryptedAppTicketResponse, m_eResult) == 0, "off m_eResult");

// GetAuthSessionTicketResponse_t — emitted asynchronously after a
// successful GetAuthSessionTicket call so games waiting on the
// "ticket ready to send" signal proceed. Layout from isteamuser.h:
//   HAuthTicket m_hAuthTicket   @ 0  (uint32)
//   EResult     m_eResult       @ 4  (int32)
// size = 8.
struct GetAuthSessionTicketResponse {
    uint32_t m_hAuthTicket;
    int32_t  m_eResult;
};
static_assert(sizeof(GetAuthSessionTicketResponse) == 8, "GetAuthSessionTicketResponse size");
static_assert(offsetof(GetAuthSessionTicketResponse, m_hAuthTicket) == 0, "off m_hAuthTicket");
static_assert(offsetof(GetAuthSessionTicketResponse, m_eResult)     == 4, "off m_eResult");

// GetTicketForWebApiResponse_t — emitted async after GetAuthTicketFor
// WebApi. Same generation pipeline as the session ticket but the
// bytes are returned inline in the callback (not via a separate
// GetAuthSessionTicket-style buffer). Layout from isteamuser.h:
//   HAuthTicket m_hAuthTicket  @ 0    uint32
//   EResult     m_eResult      @ 4    int32
//   int         m_cubTicket    @ 8    int32
//   uint8_t     m_rgubTicket[2560] @ 12
// size = 2572. Caller must consume via Steam_BGetCallback; the bytes
// are valid until FreeLastCallback advances past this entry.
struct GetTicketForWebApiResponse {
    uint32_t m_hAuthTicket;
    int32_t  m_eResult;
    int32_t  m_cubTicket;
    uint8_t  m_rgubTicket[2560];
};
static_assert(sizeof(GetTicketForWebApiResponse) == 2572,
              "GetTicketForWebApiResponse size");
static_assert(offsetof(GetTicketForWebApiResponse, m_hAuthTicket) == 0,  "off m_hAuthTicket");
static_assert(offsetof(GetTicketForWebApiResponse, m_eResult)     == 4,  "off m_eResult");
static_assert(offsetof(GetTicketForWebApiResponse, m_cubTicket)   == 8,  "off m_cubTicket");
static_assert(offsetof(GetTicketForWebApiResponse, m_rgubTicket)  == 12, "off m_rgubTicket");

// LeaderboardFindResult_t — k_iCallback=1104. Async result of
// FindLeaderboard / FindOrCreateLeaderboard. Layout from
// isteamuserstats.h:
//   SteamLeaderboard_t m_hSteamLeaderboard @ 0 (uint64)
//   uint8  m_bLeaderboardFound             @ 8
// size = 16 (pack=8 trailing pad).
struct LeaderboardFindResult {
    uint64_t m_hSteamLeaderboard;
    uint8_t  m_bLeaderboardFound;
    uint8_t  _pad[7];
};
static_assert(sizeof(LeaderboardFindResult) == 16, "LeaderboardFindResult size");
static_assert(offsetof(LeaderboardFindResult, m_hSteamLeaderboard) == 0, "off m_hSteamLeaderboard");
static_assert(offsetof(LeaderboardFindResult, m_bLeaderboardFound) == 8, "off m_bLeaderboardFound");

// LeaderboardScoresDownloaded_t — k_iCallback=1105. Async result of
// DownloadLeaderboardEntries / ForUsers. Layout:
//   SteamLeaderboard_t       m_hSteamLeaderboard         @ 0  uint64
//   SteamLeaderboardEntries_t m_hSteamLeaderboardEntries @ 8  uint64
//   int                       m_cEntryCount              @ 16 int32
// size = 24 (4B trailing pad after int32).
struct LeaderboardScoresDownloaded {
    uint64_t m_hSteamLeaderboard;
    uint64_t m_hSteamLeaderboardEntries;
    int32_t  m_cEntryCount;
    uint32_t _pad;
};
static_assert(sizeof(LeaderboardScoresDownloaded) == 24, "LeaderboardScoresDownloaded size");
static_assert(offsetof(LeaderboardScoresDownloaded, m_hSteamLeaderboard)        == 0,  "off m_hSteamLeaderboard");
static_assert(offsetof(LeaderboardScoresDownloaded, m_hSteamLeaderboardEntries) == 8,  "off m_hSteamLeaderboardEntries");
static_assert(offsetof(LeaderboardScoresDownloaded, m_cEntryCount)              == 16, "off m_cEntryCount");

// LeaderboardScoreUploaded_t — k_iCallback=1106. Async result of
// UploadLeaderboardScore. Layout:
//   uint8              m_bSuccess           @ 0
//   SteamLeaderboard_t m_hSteamLeaderboard  @ 8  (8-byte aligned)
//   int32              m_nScore             @ 16
//   uint8              m_bScoreChanged      @ 20
//   int                m_nGlobalRankNew     @ 24
//   int                m_nGlobalRankPrevious @ 28
// size = 32 (uint8 + 7B pad + uint64 + int32 + uint8 + 3B pad + int32 + int32).
struct LeaderboardScoreUploaded {
    uint8_t  m_bSuccess;
    uint8_t  _pad0[7];
    uint64_t m_hSteamLeaderboard;
    int32_t  m_nScore;
    uint8_t  m_bScoreChanged;
    uint8_t  _pad1[3];
    int32_t  m_nGlobalRankNew;
    int32_t  m_nGlobalRankPrevious;
};
static_assert(sizeof(LeaderboardScoreUploaded) == 32, "LeaderboardScoreUploaded size");
static_assert(offsetof(LeaderboardScoreUploaded, m_bSuccess)            == 0,  "off m_bSuccess");
static_assert(offsetof(LeaderboardScoreUploaded, m_hSteamLeaderboard)   == 8,  "off m_hSteamLeaderboard");
static_assert(offsetof(LeaderboardScoreUploaded, m_nScore)              == 16, "off m_nScore");
static_assert(offsetof(LeaderboardScoreUploaded, m_bScoreChanged)       == 20, "off m_bScoreChanged");
static_assert(offsetof(LeaderboardScoreUploaded, m_nGlobalRankNew)      == 24, "off m_nGlobalRankNew");
static_assert(offsetof(LeaderboardScoreUploaded, m_nGlobalRankPrevious) == 28, "off m_nGlobalRankPrevious");

// NumberOfCurrentPlayers_t — k_iCallback=1107. Async result of
// GetNumberOfCurrentPlayers. Layout:
//   uint8 m_bSuccess   @ 0
//   int32 m_cPlayers   @ 4
// size = 8.
struct NumberOfCurrentPlayers {
    uint8_t m_bSuccess;
    uint8_t _pad0[3];
    int32_t m_cPlayers;
};
static_assert(sizeof(NumberOfCurrentPlayers) == 8, "NumberOfCurrentPlayers size");
static_assert(offsetof(NumberOfCurrentPlayers, m_bSuccess) == 0, "off m_bSuccess");
static_assert(offsetof(NumberOfCurrentPlayers, m_cPlayers) == 4, "off m_cPlayers");

// GlobalAchievementPercentagesReady_t — k_iCallback=1110. Async result.
// Layout: { uint64 m_nGameID; EResult m_eResult } — size 16 (4B pad).
struct GlobalAchievementPercentagesReady {
    uint64_t m_nGameID;
    int32_t  m_eResult;
    uint32_t _pad;
};
static_assert(sizeof(GlobalAchievementPercentagesReady) == 16, "GlobalAchievementPercentagesReady size");
static_assert(offsetof(GlobalAchievementPercentagesReady, m_nGameID) == 0, "off m_nGameID");
static_assert(offsetof(GlobalAchievementPercentagesReady, m_eResult) == 8, "off m_eResult");

// LeaderboardUGCSet_t — k_iCallback=1111. Async result of
// AttachLeaderboardUGC. Layout: { EResult; SteamLeaderboard_t }.
// size = 16 (4B pad after EResult to 8-align uint64).
struct LeaderboardUGCSet {
    int32_t  m_eResult;
    uint32_t _pad;
    uint64_t m_hSteamLeaderboard;
};
static_assert(sizeof(LeaderboardUGCSet) == 16, "LeaderboardUGCSet size");
static_assert(offsetof(LeaderboardUGCSet, m_eResult)          == 0, "off m_eResult");
static_assert(offsetof(LeaderboardUGCSet, m_hSteamLeaderboard) == 8, "off m_hSteamLeaderboard");

// GlobalStatsReceived_t — k_iCallback=1112. Layout same as 1110.
struct GlobalStatsReceived {
    uint64_t m_nGameID;
    int32_t  m_eResult;
    uint32_t _pad;
};
static_assert(sizeof(GlobalStatsReceived) == 16, "GlobalStatsReceived size");
static_assert(offsetof(GlobalStatsReceived, m_nGameID) == 0, "off m_nGameID");
static_assert(offsetof(GlobalStatsReceived, m_eResult) == 8, "off m_eResult");

// LobbyMatchList_t — k_iCallback=510. Async result of RequestLobbyList.
// Layout: { uint32 m_nLobbiesMatching }. size = 4.
struct LobbyMatchList {
    uint32_t m_nLobbiesMatching;
};
static_assert(sizeof(LobbyMatchList) == 4, "LobbyMatchList size");
static_assert(offsetof(LobbyMatchList, m_nLobbiesMatching) == 0, "off m_nLobbiesMatching");

// LobbyCreated_t — k_iCallback=513. Async result of CreateLobby.
// Layout:
//   EResult m_eResult       @ 0   int32
//   uint64  m_ulSteamIDLobby @ 8
// size = 16.
struct LobbyCreated {
    int32_t  m_eResult;
    uint32_t _pad;
    uint64_t m_ulSteamIDLobby;
};
static_assert(sizeof(LobbyCreated) == 16, "LobbyCreated size");
static_assert(offsetof(LobbyCreated, m_eResult)        == 0, "off m_eResult");
static_assert(offsetof(LobbyCreated, m_ulSteamIDLobby) == 8, "off m_ulSteamIDLobby");

// LobbyEnter_t — k_iCallback=504. Async result of JoinLobby. Layout:
//   uint64 m_ulSteamIDLobby      @ 0
//   uint32 m_rgfChatPermissions  @ 8
//   uint8  m_bLocked             @ 12
//   uint32 m_EChatRoomEnterResponse @ 16 (4-aligned)
// size = 20, pack=8 trails pad to 24.
struct LobbyEnter {
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    uint8_t  m_bLocked;
    uint8_t  _pad[3];
    uint32_t m_EChatRoomEnterResponse;
    uint32_t _trail;
};
static_assert(sizeof(LobbyEnter) == 24, "LobbyEnter size");
static_assert(offsetof(LobbyEnter, m_ulSteamIDLobby)         == 0,  "off m_ulSteamIDLobby");
static_assert(offsetof(LobbyEnter, m_rgfChatPermissions)     == 8,  "off m_rgfChatPermissions");
static_assert(offsetof(LobbyEnter, m_bLocked)                == 12, "off m_bLocked");
static_assert(offsetof(LobbyEnter, m_EChatRoomEnterResponse) == 16, "off m_EChatRoomEnterResponse");

// SteamInventoryEligiblePromoItemDefIDs_t — k_iCallback=4703. Async
// result of RequestEligiblePromoItemDefinitionsIDs. Layout:
//   EResult m_result @ 0 (+4 pad)
//   CSteamID m_steamID @ 8
//   int m_numEligiblePromoItemDefs @ 16
//   bool m_bCachedData @ 20 (+3 trail pad)
// size = 24.
struct SteamInventoryEligiblePromoItemDefIDs {
    int32_t  m_result;
    uint32_t _pad0;
    uint64_t m_steamID;
    int32_t  m_numEligiblePromoItemDefs;
    uint8_t  m_bCachedData;
    uint8_t  _pad1[3];
};
static_assert(sizeof(SteamInventoryEligiblePromoItemDefIDs) == 24,
              "SteamInventoryEligiblePromoItemDefIDs size");
static_assert(offsetof(SteamInventoryEligiblePromoItemDefIDs, m_result)                  == 0,  "off m_result");
static_assert(offsetof(SteamInventoryEligiblePromoItemDefIDs, m_steamID)                 == 8,  "off m_steamID");
static_assert(offsetof(SteamInventoryEligiblePromoItemDefIDs, m_numEligiblePromoItemDefs) == 16, "off m_numEligiblePromoItemDefs");
static_assert(offsetof(SteamInventoryEligiblePromoItemDefIDs, m_bCachedData)             == 20, "off m_bCachedData");

// SteamInventoryStartPurchaseResult_t — k_iCallback=4704. Layout:
//   EResult m_result @ 0 (+4 pad)
//   uint64  m_ulOrderID @ 8
//   uint64  m_ulTransID @ 16
// size = 24.
struct SteamInventoryStartPurchaseResult {
    int32_t  m_result;
    uint32_t _pad;
    uint64_t m_ulOrderID;
    uint64_t m_ulTransID;
};
static_assert(sizeof(SteamInventoryStartPurchaseResult) == 24,
              "SteamInventoryStartPurchaseResult size");
static_assert(offsetof(SteamInventoryStartPurchaseResult, m_result)    == 0,  "off m_result");
static_assert(offsetof(SteamInventoryStartPurchaseResult, m_ulOrderID) == 8,  "off m_ulOrderID");
static_assert(offsetof(SteamInventoryStartPurchaseResult, m_ulTransID) == 16, "off m_ulTransID");

// SteamInventoryRequestPricesResult_t — k_iCallback=4705. Layout:
//   EResult m_result @ 0
//   char    m_rgchCurrency[4] @ 4
// size = 8.
struct SteamInventoryRequestPricesResult {
    int32_t m_result;
    char    m_rgchCurrency[4];
};
static_assert(sizeof(SteamInventoryRequestPricesResult) == 8,
              "SteamInventoryRequestPricesResult size");
static_assert(offsetof(SteamInventoryRequestPricesResult, m_result)       == 0, "off m_result");
static_assert(offsetof(SteamInventoryRequestPricesResult, m_rgchCurrency) == 4, "off m_rgchCurrency");

// ClanOfficerListResponse_t — k_iCallback=1335. Async result of
// RequestClanOfficerList. Layout:
//   CSteamID m_steamIDClan @ 0 (uint64)
//   int      m_cOfficers   @ 8
//   uint8    m_bSuccess    @ 12 (+3 trail pad)
// size = 16.
struct ClanOfficerListResponse {
    uint64_t m_steamIDClan;
    int32_t  m_cOfficers;
    uint8_t  m_bSuccess;
    uint8_t  _pad[3];
};
static_assert(sizeof(ClanOfficerListResponse) == 16,
              "ClanOfficerListResponse size");
static_assert(offsetof(ClanOfficerListResponse, m_steamIDClan) == 0, "off m_steamIDClan");
static_assert(offsetof(ClanOfficerListResponse, m_cOfficers)   == 8, "off m_cOfficers");
static_assert(offsetof(ClanOfficerListResponse, m_bSuccess)    == 12, "off m_bSuccess");

// DownloadClanActivityCountsResult_t — k_iCallback=1341. Layout:
//   bool m_bSuccess. size=1 (no trail pad in pack=8 on single-bool struct).
struct DownloadClanActivityCountsResult {
    uint8_t m_bSuccess;
};
static_assert(sizeof(DownloadClanActivityCountsResult) == 1,
              "DownloadClanActivityCountsResult size");

// JoinClanChatRoomCompletionResult_t — k_iCallback=1342. Layout:
//   CSteamID m_steamIDClanChat @ 0
//   int      m_eChatRoomEnterResponse @ 8 (+4 trail pad)
// size = 16.
struct JoinClanChatRoomCompletionResult {
    uint64_t m_steamIDClanChat;
    int32_t  m_eChatRoomEnterResponse;
    uint32_t _pad;
};
static_assert(sizeof(JoinClanChatRoomCompletionResult) == 16,
              "JoinClanChatRoomCompletionResult size");
static_assert(offsetof(JoinClanChatRoomCompletionResult, m_steamIDClanChat)       == 0, "off m_steamIDClanChat");
static_assert(offsetof(JoinClanChatRoomCompletionResult, m_eChatRoomEnterResponse) == 8, "off m_eChatRoomEnterResponse");

// EquippedProfileItems_t — k_iCallback=1351. Async result of
// RequestEquippedProfileItems (Steam Overlay friend-profile popup
// gates on this). Layout:
//   EResult m_eResult @ 0 (+4 pad)
//   CSteamID m_steamID @ 8
//   bool m_bHasAnimatedAvatar @ 16
//   bool m_bHasAvatarFrame @ 17
//   bool m_bHasProfileModifier @ 18
//   bool m_bHasProfileBackground @ 19
//   bool m_bHasMiniProfileBackground @ 20
// pack=8 trailing pad → 24.
struct EquippedProfileItems {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_steamID;
    uint8_t  m_bHasAnimatedAvatar;
    uint8_t  m_bHasAvatarFrame;
    uint8_t  m_bHasProfileModifier;
    uint8_t  m_bHasProfileBackground;
    uint8_t  m_bHasMiniProfileBackground;
    uint8_t  _pad1[3];
};
static_assert(sizeof(EquippedProfileItems) == 24,
              "EquippedProfileItems size");
static_assert(offsetof(EquippedProfileItems, m_eResult)                    == 0,  "off m_eResult");
static_assert(offsetof(EquippedProfileItems, m_steamID)                    == 8,  "off m_steamID");
static_assert(offsetof(EquippedProfileItems, m_bHasAnimatedAvatar)         == 16, "off m_bHasAnimatedAvatar");
static_assert(offsetof(EquippedProfileItems, m_bHasAvatarFrame)            == 17, "off m_bHasAvatarFrame");
static_assert(offsetof(EquippedProfileItems, m_bHasProfileModifier)        == 18, "off m_bHasProfileModifier");
static_assert(offsetof(EquippedProfileItems, m_bHasProfileBackground)      == 19, "off m_bHasProfileBackground");
static_assert(offsetof(EquippedProfileItems, m_bHasMiniProfileBackground)  == 20, "off m_bHasMiniProfileBackground");

// FriendsGetFollowerCount_t — k_iCallback=1344. Layout:
//   EResult m_eResult @ 0 (+4 pad)
//   CSteamID m_steamID @ 8 (uint64)
//   int m_nCount @ 16 (+4 trail pad)
// size = 24.
struct FriendsGetFollowerCount {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_steamID;
    int32_t  m_nCount;
    uint32_t _pad1;
};
static_assert(sizeof(FriendsGetFollowerCount) == 24,
              "FriendsGetFollowerCount size");
static_assert(offsetof(FriendsGetFollowerCount, m_eResult) == 0, "off m_eResult");
static_assert(offsetof(FriendsGetFollowerCount, m_steamID) == 8, "off m_steamID");
static_assert(offsetof(FriendsGetFollowerCount, m_nCount)  == 16, "off m_nCount");

// FriendsIsFollowing_t — k_iCallback=1345. Layout same shape as
// FollowerCount but with bool m_bIsFollowing instead of int.
// size = 24.
struct FriendsIsFollowing {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_steamID;
    uint8_t  m_bIsFollowing;
    uint8_t  _pad1[7];
};
static_assert(sizeof(FriendsIsFollowing) == 24,
              "FriendsIsFollowing size");
static_assert(offsetof(FriendsIsFollowing, m_eResult)       == 0, "off m_eResult");
static_assert(offsetof(FriendsIsFollowing, m_steamID)       == 8, "off m_steamID");
static_assert(offsetof(FriendsIsFollowing, m_bIsFollowing)  == 16, "off m_bIsFollowing");

// FriendsEnumerateFollowingList_t — k_iCallback=1346. Layout:
//   EResult m_eResult                     @ 0  (+4 pad)
//   CSteamID m_rgSteamID[50]              @ 8  (400B)
//   int32 m_nResultsReturned              @ 408
//   int32 m_nTotalResultCount             @ 412
// size = 416.
struct FriendsEnumerateFollowingList {
    int32_t  m_eResult;
    uint32_t _pad;
    uint64_t m_rgSteamID[50];
    int32_t  m_nResultsReturned;
    int32_t  m_nTotalResultCount;
};
static_assert(sizeof(FriendsEnumerateFollowingList) == 416,
              "FriendsEnumerateFollowingList size");
static_assert(offsetof(FriendsEnumerateFollowingList, m_eResult)          == 0,   "off m_eResult");
static_assert(offsetof(FriendsEnumerateFollowingList, m_rgSteamID)        == 8,   "off m_rgSteamID");
static_assert(offsetof(FriendsEnumerateFollowingList, m_nResultsReturned) == 408, "off m_nResultsReturned");
static_assert(offsetof(FriendsEnumerateFollowingList, m_nTotalResultCount) == 412, "off m_nTotalResultCount");

// RemoteStorageDownloadUGCResult_t — k_iCallback=1317. Async result of
// ISteamRemoteStorage.UGCDownload. Layout from isteamremotestorage.h:
//   EResult m_eResult              @ 0   (+4 pad)
//   UGCHandle_t m_hFile            @ 8   (uint64)
//   AppId_t m_nAppID               @ 16  (uint32)
//   int32   m_nSizeInBytes         @ 20
//   char    m_pchFileName[260]     @ 24
//   uint64  m_ulSteamIDOwner       @ 288 (needs 8-align after 260+24=284 → pad 4)
// size = 296 (32-bit ALIGNED pad after string, then 8 bytes uint64 at 288).
struct RemoteStorageDownloadUGCResult {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_hFile;
    uint32_t m_nAppID;
    int32_t  m_nSizeInBytes;
    char     m_pchFileName[260];
    uint32_t _pad1;
    uint64_t m_ulSteamIDOwner;
};
static_assert(sizeof(RemoteStorageDownloadUGCResult) == 296,
              "RemoteStorageDownloadUGCResult size");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_eResult)        == 0,   "off m_eResult");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_hFile)          == 8,   "off m_hFile");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_nAppID)         == 16,  "off m_nAppID");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_nSizeInBytes)   == 20,  "off m_nSizeInBytes");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_pchFileName)    == 24,  "off m_pchFileName");
static_assert(offsetof(RemoteStorageDownloadUGCResult, m_ulSteamIDOwner) == 288, "off m_ulSteamIDOwner");

// RemoteStorageSubscribePublishedFileResult_t — k_iCallback=1313.
// Async result of ISteamUGC.SubscribeItem. Layout:
//   EResult m_eResult @ 0 (+ 4B pad to 8-align)
//   PublishedFileId_t m_nPublishedFileId @ 8 (uint64)
// size = 16.
struct RemoteStorageSubscribePublishedFileResult {
    int32_t  m_eResult;
    uint32_t _pad;
    uint64_t m_nPublishedFileId;
};
static_assert(sizeof(RemoteStorageSubscribePublishedFileResult) == 16,
              "RemoteStorageSubscribePublishedFileResult size");
static_assert(offsetof(RemoteStorageSubscribePublishedFileResult, m_eResult) == 0,
              "off m_eResult");
static_assert(offsetof(RemoteStorageSubscribePublishedFileResult, m_nPublishedFileId) == 8,
              "off m_nPublishedFileId");

// RemoteStorageUnsubscribePublishedFileResult_t — k_iCallback=1315.
// Identical layout to subscribe-result.
struct RemoteStorageUnsubscribePublishedFileResult {
    int32_t  m_eResult;
    uint32_t _pad;
    uint64_t m_nPublishedFileId;
};
static_assert(sizeof(RemoteStorageUnsubscribePublishedFileResult) == 16,
              "RemoteStorageUnsubscribePublishedFileResult size");
static_assert(offsetof(RemoteStorageUnsubscribePublishedFileResult, m_eResult) == 0,
              "off m_eResult");
static_assert(offsetof(RemoteStorageUnsubscribePublishedFileResult, m_nPublishedFileId) == 8,
              "off m_nPublishedFileId");

// SteamUGCQueryCompleted_t — k_iCallback=3401. Async result of
// ISteamUGC.SendQueryUGCRequest. Layout from isteamugc.h:
//   UGCQueryHandle_t m_handle              @ 0   uint64
//   EResult          m_eResult             @ 8
//   uint32           m_unNumResultsReturned @ 12
//   uint32           m_unTotalMatchingResults @ 16
//   bool             m_bCachedData          @ 20
//   char m_rgchNextCursor[k_cchPublishedFileURLMax (256)] @ 21
// pack=8: char[256] at offset 21 ends at 277; pad to 8 → 280.
struct SteamUGCQueryCompleted {
    uint64_t m_handle;
    int32_t  m_eResult;
    uint32_t m_unNumResultsReturned;
    uint32_t m_unTotalMatchingResults;
    uint8_t  m_bCachedData;
    char     m_rgchNextCursor[256];
    uint8_t  _pad[3];
};
static_assert(sizeof(SteamUGCQueryCompleted) == 280,
              "SteamUGCQueryCompleted size");
static_assert(offsetof(SteamUGCQueryCompleted, m_handle)                 == 0,  "off m_handle");
static_assert(offsetof(SteamUGCQueryCompleted, m_eResult)                == 8,  "off m_eResult");
static_assert(offsetof(SteamUGCQueryCompleted, m_unNumResultsReturned)   == 12, "off m_unNumResultsReturned");
static_assert(offsetof(SteamUGCQueryCompleted, m_unTotalMatchingResults) == 16, "off m_unTotalMatchingResults");
static_assert(offsetof(SteamUGCQueryCompleted, m_bCachedData)            == 20, "off m_bCachedData");
static_assert(offsetof(SteamUGCQueryCompleted, m_rgchNextCursor)         == 21, "off m_rgchNextCursor");

// SteamUGCRequestUGCDetailsResult_t — k_iCallback=3402. Async result of
// ISteamUGC.RequestUGCDetails. Layout: just the EResult + the long
// SteamUGCDetails_t struct (which is hairy; we send EResult=Fail and
// rely on the caller to ignore the rest on failure). Approximation:
// post just enough EResult bytes — most callers only read m_eResult.
// To be safe we allocate 8 bytes (EResult + pad) so memcpy doesn't
// overrun the receiver's tiny payload.
struct SteamUGCRequestUGCDetailsResultMinimal {
    int32_t  m_eResult;
    uint32_t _pad;
};
static_assert(sizeof(SteamUGCRequestUGCDetailsResultMinimal) == 8,
              "SteamUGCRequestUGCDetailsResultMinimal size");

// SteamAPICallCompleted_t — k_iCallback=703. SDK contract: every async
// hCall completion fires this generic event in ADDITION to the typed
// CCallResult dispatch. Games that hook completions via
// STEAM_CALLBACK(SteamAPICallCompleted_t, …) (rather than registering
// per-call CCallResults) listen on this for ALL their pending hCalls.
// Layout:
//   SteamAPICall_t m_hAsyncCall @ 0  (uint64)
//   int            m_iCallback  @ 8  (the typed-cb id this call posted)
//   uint32         m_cubParam   @ 12 (size of the typed-cb payload)
// size = 16.
struct SteamAPICallCompleted {
    uint64_t m_hAsyncCall;
    int32_t  m_iCallback;
    uint32_t m_cubParam;
};
static_assert(sizeof(SteamAPICallCompleted) == 16,
              "SteamAPICallCompleted size");
static_assert(offsetof(SteamAPICallCompleted, m_hAsyncCall) == 0, "off m_hAsyncCall");
static_assert(offsetof(SteamAPICallCompleted, m_iCallback)  == 8, "off m_iCallback");
static_assert(offsetof(SteamAPICallCompleted, m_cubParam)   == 12, "off m_cubParam");

// CheckFileSignature_t — async result of ISteamUtils.CheckFileSignature.
// Layout from isteamutils.h:
//   ECheckFileSignature m_eCheckFileSignature @ 0   int32
// size = 4. (Trailing pad to alignof(struct) — alignof(int32)=4, so no
// pad; sizeof=4.)
struct CheckFileSignature {
    int32_t m_eCheckFileSignature;
};
static_assert(sizeof(CheckFileSignature) == 4, "CheckFileSignature size");
static_assert(offsetof(CheckFileSignature, m_eCheckFileSignature) == 0,
              "off m_eCheckFileSignature");

// StoreAuthURLResponse_t — async result of RequestStoreAuthURL. Layout:
//   char m_szURL[512] @ 0
// SDK pads to 512 fixed; the URL is NUL-terminated.
struct StoreAuthURLResponse {
    char m_szURL[512];
};
static_assert(sizeof(StoreAuthURLResponse) == 512, "StoreAuthURLResponse size");
static_assert(offsetof(StoreAuthURLResponse, m_szURL) == 0, "off m_szURL");

// MarketEligibilityResponse_t — async result of GetMarketEligibility.
// Layout from isteamuser.h:
//   bool   m_bAllowed                       @ 0
//   int    m_eNotAllowedReason              @ 4   (EMarketNotAllowedReasons bitfield)
//   uint32 m_rtAllowedAtTime                @ 8   (unix32)
//   int    m_cdaySteamGuardRequiredDays     @ 12
//   int    m_cdayNewDeviceCooldown          @ 16
// size = 20 (4-byte aligned trailing).
struct MarketEligibilityResponse {
    bool     m_bAllowed;
    uint8_t  _pad0[3];
    int32_t  m_eNotAllowedReason;
    uint32_t m_rtAllowedAtTime;
    int32_t  m_cdaySteamGuardRequiredDays;
    int32_t  m_cdayNewDeviceCooldown;
};
static_assert(sizeof(MarketEligibilityResponse) == 20, "MarketEligibilityResponse size");
static_assert(offsetof(MarketEligibilityResponse, m_bAllowed)                   == 0,  "off m_bAllowed");
static_assert(offsetof(MarketEligibilityResponse, m_eNotAllowedReason)          == 4,  "off m_eNotAllowedReason");
static_assert(offsetof(MarketEligibilityResponse, m_rtAllowedAtTime)            == 8,  "off m_rtAllowedAtTime");
static_assert(offsetof(MarketEligibilityResponse, m_cdaySteamGuardRequiredDays) == 12, "off m_cdaySteamGuardRequiredDays");
static_assert(offsetof(MarketEligibilityResponse, m_cdayNewDeviceCooldown)      == 16, "off m_cdayNewDeviceCooldown");

// DurationControl_t — async result of GetDurationControl. Chinese
// duration-control telemetry. Layout from isteamuser.h:
//   EResult m_eResult                       @ 0   int32
//   AppId_t m_appid                         @ 4   uint32
//   bool    m_bApplicable                   @ 8
//   int32   m_csecsLast5h                   @ 12
//   int32   m_progress                      @ 16  EDurationControlProgress
//   int32   m_notification                  @ 20  EDurationControlNotification
//   int32   m_csecsToday                    @ 24
//   int32   m_csecsRemaining                @ 28
// size = 32.
struct DurationControl {
    int32_t  m_eResult;
    uint32_t m_appid;
    bool     m_bApplicable;
    uint8_t  _pad0[3];
    int32_t  m_csecsLast5h;
    int32_t  m_progress;
    int32_t  m_notification;
    int32_t  m_csecsToday;
    int32_t  m_csecsRemaining;
};
static_assert(sizeof(DurationControl) == 32, "DurationControl size");
static_assert(offsetof(DurationControl, m_eResult)        == 0,  "off m_eResult");
static_assert(offsetof(DurationControl, m_appid)          == 4,  "off m_appid");
static_assert(offsetof(DurationControl, m_bApplicable)    == 8,  "off m_bApplicable");
static_assert(offsetof(DurationControl, m_csecsLast5h)    == 12, "off m_csecsLast5h");
static_assert(offsetof(DurationControl, m_progress)       == 16, "off m_progress");
static_assert(offsetof(DurationControl, m_notification)   == 20, "off m_notification");
static_assert(offsetof(DurationControl, m_csecsToday)     == 24, "off m_csecsToday");
static_assert(offsetof(DurationControl, m_csecsRemaining) == 28, "off m_csecsRemaining");

// ValidateAuthTicketResponse_t — server-side counterpart, emitted
// when BeginAuthSession completes. Layout:
//   CSteamID         m_SteamID            @ 0  (uint64)
//   EAuthSessionResponse m_eAuthSessionResponse @ 8  (int32)
//   CSteamID         m_OwnerSteamID       @ 16 (uint64) — for VAC banned users
// size = 24 (pack=8).
struct ValidateAuthTicketResponse {
    uint64_t m_SteamID;
    int32_t  m_eAuthSessionResponse;
    uint32_t _pad;
    uint64_t m_OwnerSteamID;
};
static_assert(sizeof(ValidateAuthTicketResponse) == 24, "ValidateAuthTicketResponse size");
static_assert(offsetof(ValidateAuthTicketResponse, m_SteamID)              == 0,  "off m_SteamID");
static_assert(offsetof(ValidateAuthTicketResponse, m_eAuthSessionResponse) == 8,  "off m_eAuthSessionResponse");
static_assert(offsetof(ValidateAuthTicketResponse, m_OwnerSteamID)         == 16, "off m_OwnerSteamID");

// SteamServersDisconnected_t — emitted when the .so transitions off
// logged-on. EResult is k_EResultOK on a clean logout, otherwise the
// reason (NoConnection / LoggedInElsewhere / etc.).
struct SteamServersDisconnected {
    int32_t m_eResult;
};
static_assert(sizeof(SteamServersDisconnected) == 4, "SteamServersDisconnected size");
static_assert(offsetof(SteamServersDisconnected, m_eResult) == 0, "off m_eResult");

// SetPersonaNameResponse_t — emitted async after SetPersonaName.
// Layout from isteamfriends.h:
//   bool    m_bSuccess       @ 0
//   bool    m_bLocalSuccess  @ 1
//   EResult m_result         @ 4  (2 bytes of pad between bool[2] + int)
// size = 8 with pack=8 trailing alignment to int.
struct SetPersonaNameResponse {
    bool     m_bSuccess;
    bool     m_bLocalSuccess;
    uint8_t  _pad[2];
    int32_t  m_result;
};
static_assert(sizeof(SetPersonaNameResponse) == 8, "SetPersonaNameResponse size");
static_assert(offsetof(SetPersonaNameResponse, m_bSuccess)      == 0, "off m_bSuccess");
static_assert(offsetof(SetPersonaNameResponse, m_bLocalSuccess) == 1, "off m_bLocalSuccess");
static_assert(offsetof(SetPersonaNameResponse, m_result)        == 4, "off m_result");

// RemoteStorageAppSyncedClient_t — emitted when Steam Cloud finishes
// syncing server-side state down to the local client (e.g. fresh
// save-state arrives). Games gate "ready to load save" on this.
// Layout from isteamremotestorage.h (3 ints; pack=8 alignment is
// no-op for an int-only struct):
//   AppId_t m_nAppID           @ 0
//   EResult m_eResult          @ 4
//   int     m_unNumDownloads   @ 8
// sizeof = 12.
struct RemoteStorageAppSyncedClient {
    uint32_t m_nAppID;
    int32_t  m_eResult;
    int32_t  m_unNumDownloads;
};
static_assert(sizeof(RemoteStorageAppSyncedClient) == 12,
              "RemoteStorageAppSyncedClient size");
static_assert(offsetof(RemoteStorageAppSyncedClient, m_nAppID)         == 0, "off m_nAppID");
static_assert(offsetof(RemoteStorageAppSyncedClient, m_eResult)        == 4, "off m_eResult");
static_assert(offsetof(RemoteStorageAppSyncedClient, m_unNumDownloads) == 8, "off m_unNumDownloads");

// RemoteStorageFileWriteAsyncComplete_t — k_iCallback = 1331. Dispatched
// as a CallResult against the SteamAPICall_t returned from FileWriteAsync.
// Layout: single EResult @ 0.
struct RemoteStorageFileWriteAsyncComplete {
    int32_t m_eResult;
};
static_assert(sizeof(RemoteStorageFileWriteAsyncComplete) == 4,
              "RemoteStorageFileWriteAsyncComplete size");
static_assert(offsetof(RemoteStorageFileWriteAsyncComplete, m_eResult) == 0,
              "off m_eResult");

// RemoteStorageFileReadAsyncComplete_t — k_iCallback = 1332. Dispatched
// as a CallResult against the SteamAPICall_t returned from FileReadAsync.
// Layout from isteamremotestorage.h:
//   SteamAPICall_t m_hFileReadAsync @ 0  (matches the issuing hCall)
//   EResult        m_eResult        @ 8
//   uint32         m_nOffset        @ 12
//   uint32         m_cubRead        @ 16
// Trailing pad to 8-align next-element boundary → 24.
struct RemoteStorageFileReadAsyncComplete {
    uint64_t m_hFileReadAsync;
    int32_t  m_eResult;
    uint32_t m_nOffset;
    uint32_t m_cubRead;
    uint32_t _pad;
};
static_assert(sizeof(RemoteStorageFileReadAsyncComplete) == 24,
              "RemoteStorageFileReadAsyncComplete size");
static_assert(offsetof(RemoteStorageFileReadAsyncComplete, m_hFileReadAsync) == 0,  "off m_hFileReadAsync");
static_assert(offsetof(RemoteStorageFileReadAsyncComplete, m_eResult)        == 8,  "off m_eResult");
static_assert(offsetof(RemoteStorageFileReadAsyncComplete, m_nOffset)        == 12, "off m_nOffset");
static_assert(offsetof(RemoteStorageFileReadAsyncComplete, m_cubRead)        == 16, "off m_cubRead");

// RemoteStorageFileShareResult_t — k_iCallback=1307. Dispatched as a
// CallResult against the SteamAPICall_t returned from FileShare.
// Layout from isteamremotestorage.h (pack=8):
//   EResult     m_eResult                     @ 0  (4B + 4B pad)
//   UGCHandle_t m_hFile                       @ 8  (uint64)
//   char        m_rgchFilename[k_cchFilenameMax] @ 16  (260B)
//   trailing pad to 8-align → size = 280
struct RemoteStorageFileShareResult {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_hFile;
    char     m_rgchFilename[260];
    uint8_t  _pad1[4];
};
static_assert(sizeof(RemoteStorageFileShareResult) == 280,
              "RemoteStorageFileShareResult size");
static_assert(offsetof(RemoteStorageFileShareResult, m_eResult)      == 0,  "off m_eResult");
static_assert(offsetof(RemoteStorageFileShareResult, m_hFile)        == 8,  "off m_hFile");
static_assert(offsetof(RemoteStorageFileShareResult, m_rgchFilename) == 16, "off m_rgchFilename");

// FileDetailsResult_t — k_iCallback=1323. Dispatched as a CallResult
// against the SteamAPICall_t returned from GetFileDetails. Layout
// (pack=8):
//   EResult m_eResult     @ 0  (4B + 4B pad)
//   uint64  m_ulFileSize  @ 8
//   uint8   m_FileSHA[20] @ 16
//   uint32  m_unFlags     @ 36
//   size = 40
struct FileDetailsResult {
    int32_t  m_eResult;
    uint32_t _pad0;
    uint64_t m_ulFileSize;
    uint8_t  m_FileSHA[20];
    uint32_t m_unFlags;
};
static_assert(sizeof(FileDetailsResult) == 40,
              "FileDetailsResult size");
static_assert(offsetof(FileDetailsResult, m_eResult)    == 0,  "off m_eResult");
static_assert(offsetof(FileDetailsResult, m_ulFileSize) == 8,  "off m_ulFileSize");
static_assert(offsetof(FileDetailsResult, m_FileSHA)    == 16, "off m_FileSHA");
static_assert(offsetof(FileDetailsResult, m_unFlags)    == 36, "off m_unFlags");

// PersonaStateChange_t — emitted on every persona delta. Games drive
// their friends-list refresh off the m_nChangeFlags bitmask: a list
// member with kPersonaChangeName set wants its name re-fetched, etc.
// Layout from isteamfriends.h:
//   uint64 m_ulSteamID    @ 0
//   int    m_nChangeFlags @ 8  (EPersonaChange bitmask)
// Size with pack=8 trailing pad: 16.
struct PersonaStateChange {
    uint64_t m_ulSteamID;
    int32_t  m_nChangeFlags;
    uint32_t _pad;
};
static_assert(sizeof(PersonaStateChange) == 16, "PersonaStateChange size");
static_assert(offsetof(PersonaStateChange, m_ulSteamID)    == 0, "off m_ulSteamID");
static_assert(offsetof(PersonaStateChange, m_nChangeFlags) == 8, "off m_nChangeFlags");

// UserAchievementStored_t — per-achievement variant of UserStatsStored.
// SDK fires this once per achievement when ISteamUserStats::StoreStats
// commits unlocks, and once per IndicateAchievementProgress call (with
// curProgress < maxProgress). Overlay HUD popups + analytics gate on
// it. We emit synthesized copies on StoreStats (one per achievement
// that flipped true→achieved since the last commit) and on every
// IndicateAchievementProgress call. The real CM round-trip
// (CMsgClientStoreUserStats) is owned by wn-session; this is the
// client-visible callback.
//
// Layout matches SDK isteamuserstats.h k_cchStatNameMax=128:
//   uint64 m_nGameID                @ 0
//   bool   m_bGroupAchievement      @ 8
//   char   m_rgchAchievementName[128] @ 9
//   uint32 m_nCurProgress           @ 140 (3-byte pad before u32 align)
//   uint32 m_nMaxProgress           @ 144
//   trailing pad to 8-byte alignment → sizeof = 152.
//
// The pad between the bool at offset 8 and the char[] at offset 9 is
// _none_ — the array byte-aligned right after the bool. The pad
// before m_nCurProgress is at offset 137-139 (3 bytes) so it lands
// 4-aligned at 140.
// AvatarImageLoaded_t (callback id 1334 = k_iSteamFriendsCallbacks + 34).
// Fired when an avatar image's RGBA bytes become resident (either via
// async download or, in our case, an explicit push from wn-session /
// the diagnostic path). Games' friends-HUDs typically request avatars
// and gate the per-friend portrait render on this callback.
//
// SDK layout (pack=8):
//   uint64 m_steamID  @ 0   (CSteamID)
//   int    m_iImage   @ 8
//   int    m_iWide    @ 12
//   int    m_iTall    @ 16
//   trailing 4-byte pad (struct align = 8) → sizeof = 24.
struct AvatarImageLoaded {
    uint64_t m_steamID;
    int32_t  m_iImage;
    int32_t  m_iWide;
    int32_t  m_iTall;
};
static_assert(sizeof(AvatarImageLoaded) == 24, "AvatarImageLoaded size");
static_assert(offsetof(AvatarImageLoaded, m_steamID) == 0,  "off m_steamID");
static_assert(offsetof(AvatarImageLoaded, m_iImage)  == 8,  "off m_iImage");
static_assert(offsetof(AvatarImageLoaded, m_iWide)   == 12, "off m_iWide");
static_assert(offsetof(AvatarImageLoaded, m_iTall)   == 16, "off m_iTall");

// FriendRichPresenceUpdate_t (callback id 1336) — fired whenever
// rich-presence data changes for the local user (after SetRichPresence
// / ClearRichPresence) or asynchronously when a remote friend's RP
// arrives via CMsgClientPersonaState. Friend overlays + invite/
// connect UIs gate the "in lobby X" string on this callback +
// follow-up GetFriendRichPresence.
//
// Layout (SDK):
//   CSteamID m_steamIDFriend    @ 0  (uint64)
//   AppId_t  m_nAppID           @ 8  (uint32)
//   trailing 4-byte pad → sizeof = 16, 8-byte aligned.
struct FriendRichPresenceUpdate {
    uint64_t m_steamIDFriend;
    uint32_t m_nAppID;
};
static_assert(sizeof(FriendRichPresenceUpdate) == 16, "FriendRichPresenceUpdate size");
static_assert(offsetof(FriendRichPresenceUpdate, m_steamIDFriend) == 0, "off m_steamIDFriend");
static_assert(offsetof(FriendRichPresenceUpdate, m_nAppID)        == 8, "off m_nAppID");

constexpr size_t kAchievementNameMax = 128;
struct UserAchievementStored {
    uint64_t  m_nGameID;
    bool      m_bGroupAchievement;
    char      m_rgchAchievementName[kAchievementNameMax];
    uint32_t  m_nCurProgress;
    uint32_t  m_nMaxProgress;
};
static_assert(sizeof(UserAchievementStored) == 152, "UserAchievementStored size");
static_assert(offsetof(UserAchievementStored, m_nGameID)            == 0,   "off m_nGameID");
static_assert(offsetof(UserAchievementStored, m_bGroupAchievement)  == 8,   "off m_bGroupAchievement");
static_assert(offsetof(UserAchievementStored, m_rgchAchievementName)== 9,   "off m_rgchAchievementName");
static_assert(offsetof(UserAchievementStored, m_nCurProgress)       == 140, "off m_nCurProgress");
static_assert(offsetof(UserAchievementStored, m_nMaxProgress)       == 144, "off m_nMaxProgress");

}  // namespace wn_libsteamclient::callbacks
