#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// CreateLobby / JoinLobby / LeaveLobby — the three lobby-host-control
// messages for ISteamMatchmaking.{CreateLobby,JoinLobby,LeaveLobby}.
//
// Verified against SteamKit2's
//   Resources/Protobufs/steam/steammessages_clientserver_mms.proto.

namespace wn_steam::pb {

// EMsg 6601 — CMsgClientMMSCreateLobby
//
//   message CMsgClientMMSCreateLobby {
//     optional uint32 app_id              = 1;
//     optional int32  max_members         = 2;
//     optional int32  lobby_type          = 3;   // ELobbyType
//     optional int32  lobby_flags         = 4;
//     optional uint32 cell_id             = 5;
//     optional bytes  metadata            = 7;
//     optional string persona_name_owner  = 8;
//   }
struct CMsgClientMMSCreateLobby {
    uint32_t                 app_id      = 0;
    int32_t                  max_members = 0;
    int32_t                  lobby_type  = 0;
    int32_t                  lobby_flags = 0;
    std::vector<uint8_t>     metadata;
    std::string              persona_name_owner;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6602 — CMsgClientMMSCreateLobbyResponse
//
//   message CMsgClientMMSCreateLobbyResponse {
//     optional uint32  app_id         = 1;
//     optional fixed64 steam_id_lobby = 2;
//     optional int32   eresult        = 3 [default = 2];
//   }
struct CMsgClientMMSCreateLobbyResponse {
    uint32_t app_id          = 0;
    uint64_t steam_id_lobby  = 0;
    int32_t  eresult         = 2;   // proto2 default = Fail

    [[nodiscard]] static std::optional<CMsgClientMMSCreateLobbyResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

// EMsg 6603 — CMsgClientMMSJoinLobby
//
//   message CMsgClientMMSJoinLobby {
//     optional uint32  app_id          = 1;
//     optional fixed64 steam_id_lobby  = 2;
//     optional string  persona_name    = 3;
//   }
struct CMsgClientMMSJoinLobby {
    uint32_t    app_id         = 0;
    uint64_t    steam_id_lobby = 0;
    std::string persona_name;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6604 — CMsgClientMMSJoinLobbyResponse
//
//   message CMsgClientMMSJoinLobbyResponse {
//     message Member { optional fixed64 steam_id = 1;
//                       optional string  persona_name = 2;
//                       optional bytes   metadata = 3; }
//     optional uint32  app_id                   = 1;
//     optional fixed64 steam_id_lobby           = 2;
//     optional int32   chat_room_enter_response = 3;   // EChatRoomEnterResponse
//     optional int32   max_members              = 4;
//     optional int32   lobby_type               = 5;
//     optional int32   lobby_flags              = 6;
//     optional fixed64 steam_id_owner           = 7;
//     optional bytes   metadata                 = 8;
//     repeated Member  members                  = 9;
//   }
struct CMsgClientMMSJoinLobbyResponseMember {
    uint64_t            steam_id = 0;
    std::string         persona_name;
    std::vector<uint8_t> metadata;
};

struct CMsgClientMMSJoinLobbyResponse {
    uint32_t                                          app_id                   = 0;
    uint64_t                                          steam_id_lobby           = 0;
    int32_t                                           chat_room_enter_response = 2; // Error
    int32_t                                           max_members              = 0;
    int32_t                                           lobby_type               = 0;
    int32_t                                           lobby_flags              = 0;
    uint64_t                                          steam_id_owner           = 0;
    std::vector<uint8_t>                              metadata;
    std::vector<CMsgClientMMSJoinLobbyResponseMember> members;

    [[nodiscard]] static std::optional<CMsgClientMMSJoinLobbyResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

// EMsg 6605 — CMsgClientMMSLeaveLobby (fire-and-forget; Steam fires
// 6606 Response but we don't gate the SDK callback on it).
//
//   message CMsgClientMMSLeaveLobby {
//     optional uint32  app_id         = 1;
//     optional fixed64 steam_id_lobby = 2;
//   }
struct CMsgClientMMSLeaveLobby {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6609 — CMsgClientMMSSetLobbyData
//
//   message CMsgClientMMSSetLobbyData {
//     optional uint32  app_id          = 1;
//     optional fixed64 steam_id_lobby  = 2;
//     optional fixed64 steam_id_member = 3;   // 0 = lobby-level
//     optional int32   max_members     = 4;
//     optional int32   lobby_type      = 5;
//     optional int32   lobby_flags     = 6;
//     optional bytes   metadata        = 7;   // serialized KV
//   }
//
// Steam echoes via 6610 SetLobbyDataResponse + a 6612 LobbyData push
// to every member. We use the existing JobManager path for the
// response (already routed in cm_client.cpp's single-shot arm).
struct CMsgClientMMSSetLobbyData {
    uint32_t                 app_id           = 0;
    uint64_t                 steam_id_lobby   = 0;
    uint64_t                 steam_id_member  = 0;
    int32_t                  max_members      = 0;
    int32_t                  lobby_type       = 0;
    int32_t                  lobby_flags      = 0;
    std::vector<uint8_t>     metadata;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6610 — CMsgClientMMSSetLobbyDataResponse
//
//   message CMsgClientMMSSetLobbyDataResponse {
//     optional uint32  app_id         = 1;
//     optional fixed64 steam_id_lobby = 2;
//     optional int32   eresult        = 3 [default = 2];
//   }
struct CMsgClientMMSSetLobbyDataResponse {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;
    int32_t  eresult        = 2;

    [[nodiscard]] static std::optional<CMsgClientMMSSetLobbyDataResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

// EMsg 6613 — CMsgClientMMSSendLobbyChatMsg (fire-and-forget)
//
//   message CMsgClientMMSSendLobbyChatMsg {
//     optional uint32  app_id           = 1;
//     optional fixed64 steam_id_lobby   = 2;
//     optional bytes   lobby_message    = 4;
//   }
//
// Steam relays to all lobby members as 6614 ClientMMSLobbyChatMsg
// push. We don't track a response for the send itself.
struct CMsgClientMMSSendLobbyChatMsg {
    uint32_t             app_id         = 0;
    uint64_t             steam_id_lobby = 0;
    std::vector<uint8_t> lobby_message;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6614 — CMsgClientMMSLobbyChatMsg (SERVER PUSH, decode-only)
//
//   message CMsgClientMMSLobbyChatMsg {
//     optional uint32  app_id           = 1;
//     optional fixed64 steam_id_lobby   = 2;
//     optional fixed64 steam_id_sender  = 3;
//     optional bytes   lobby_message    = 4;
//   }
struct CMsgClientMMSLobbyChatMsg {
    uint32_t             app_id          = 0;
    uint64_t             steam_id_lobby  = 0;
    uint64_t             steam_id_sender = 0;
    std::vector<uint8_t> lobby_message;

    [[nodiscard]] static std::optional<CMsgClientMMSLobbyChatMsg>
    deserialize(std::span<const uint8_t> body) noexcept;
};

// EMsg 6619/6620 — CMsgClientMMSUserJoinedLobby / UserLeftLobby
// (SERVER PUSH, decode-only). Shape is the same for both.
//
//   message CMsgClientMMSUserJoinedLobby / UserLeftLobby {
//     optional uint32  app_id          = 1;
//     optional fixed64 steam_id_lobby  = 2;
//     optional fixed64 steam_id_user   = 3;
//     optional string  persona_name    = 4;
//   }
struct CMsgClientMMSUserJoinedOrLeftLobby {
    uint32_t    app_id         = 0;
    uint64_t    steam_id_lobby = 0;
    uint64_t    steam_id_user  = 0;
    std::string persona_name;

    [[nodiscard]] static std::optional<CMsgClientMMSUserJoinedOrLeftLobby>
    deserialize(std::span<const uint8_t> body) noexcept;
};

// EMsg 6621 — CMsgClientMMSInviteToLobby
//
//   message CMsgClientMMSInviteToLobby {
//     optional uint32  app_id                  = 1;
//     optional fixed64 steam_id_lobby          = 2;
//     optional fixed64 steam_id_user_invited   = 3;
//   }
//
// Fire-and-forget. Steam relays a notification to the invitee's
// Steam client which surfaces it as a popup / in-game overlay
// notification. No success/fail response — the SDK
// InviteUserToLobby return is "did we successfully ASK Steam to
// invite", not "did the invitee accept".
struct CMsgClientMMSInviteToLobby {
    uint32_t app_id                = 0;
    uint64_t steam_id_lobby        = 0;
    uint64_t steam_id_user_invited = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6615 — CMsgClientMMSSetLobbyOwner
//
//   message CMsgClientMMSSetLobbyOwner {
//     optional uint32  app_id              = 1;
//     optional fixed64 steam_id_lobby      = 2;
//     optional fixed64 steam_id_new_owner  = 3;
//   }
//
// Host-only transfer; Steam echoes 6612 LobbyData push with the
// updated owner_sid so all members re-resolve GetLobbyOwner.
struct CMsgClientMMSSetLobbyOwner {
    uint32_t app_id              = 0;
    uint64_t steam_id_lobby      = 0;
    uint64_t steam_id_new_owner  = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

// EMsg 6616 — CMsgClientMMSSetLobbyOwnerResponse
//
//   message CMsgClientMMSSetLobbyOwnerResponse {
//     optional uint32  app_id          = 1;
//     optional fixed64 steam_id_lobby  = 2;
//     optional int32   eresult         = 3 [default = 2];
//   }
struct CMsgClientMMSSetLobbyOwnerResponse {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;
    int32_t  eresult        = 2;

    [[nodiscard]] static std::optional<CMsgClientMMSSetLobbyOwnerResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

}  // namespace wn_steam::pb
