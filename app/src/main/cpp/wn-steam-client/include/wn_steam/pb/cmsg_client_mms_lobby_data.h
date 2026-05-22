#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// CMsgClientMMSLobbyData (EMsg 6612) — SERVER PUSH, decode-only.
// Steam fires this whenever a lobby the client has subscribed to changes:
// metadata edit, member join/leave (also covered by separate
// UserJoinedLobby / UserLeftLobby messages), owner change, etc.
//
// Verified against SteamKit2's
//   Resources/Protobufs/steam/steammessages_clientserver_mms.proto:
//
//   message CMsgClientMMSLobbyData {
//     message Member {
//       optional fixed64 steam_id     = 1;
//       optional string  persona_name = 2;
//       optional bytes   metadata     = 3;
//       optional string  ping_data    = 4;
//     }
//     optional uint32  app_id                       = 1;
//     optional fixed64 steam_id_lobby               = 2;
//     optional int32   num_members                  = 3;
//     optional int32   max_members                  = 4;
//     optional int32   lobby_type                   = 5;
//     optional int32   lobby_flags                  = 6;
//     optional fixed64 steam_id_owner               = 7;
//     optional bytes   metadata                     = 8;
//     repeated Member  members                      = 9;
//     optional uint32  lobby_cellid                 = 10;
//     optional bool    owner_should_accept_changes  = 11;
//   }

namespace wn_steam::pb {

struct MMSLobbyDataMember {
    uint64_t            steam_id = 0;
    std::string         persona_name;
    std::vector<uint8_t> metadata;
};

struct CMsgClientMMSLobbyData {
    uint32_t                          app_id          = 0;
    uint64_t                          steam_id_lobby  = 0;
    int32_t                           num_members     = 0;
    int32_t                           max_members     = 0;
    int32_t                           lobby_type      = 0;
    int32_t                           lobby_flags     = 0;
    uint64_t                          steam_id_owner  = 0;
    std::vector<uint8_t>              metadata;
    std::vector<MMSLobbyDataMember>   members;

    [[nodiscard]] static std::optional<CMsgClientMMSLobbyData>
    deserialize(std::span<const uint8_t> body) noexcept;
};

}  // namespace wn_steam::pb
