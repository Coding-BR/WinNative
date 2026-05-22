#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// CMsgClientMMSGetLobbyList (EMsg 6607) / Response (EMsg 6608).
//
// Verified against SteamKit2's
//   Resources/Protobufs/steam/steammessages_clientserver_mms.proto:
//
//   message CMsgClientMMSGetLobbyList {
//     message Filter {
//       optional string key            = 1;
//       optional string value          = 2;
//       optional int32  comparision    = 3;   // sic — proto typo upstream
//       optional int32  filter_type    = 4;
//     }
//     optional uint32 app_id                = 1;
//     optional int32  num_lobbies_requested = 3;
//     optional uint32 cell_id               = 4;
//     repeated Filter filters               = 6;
//     optional CMsgIPAddress public_ip      = 7;     // we leave unset
//     optional string network_ping_location = 8;     // we leave unset
//   }
//
//   message CMsgClientMMSGetLobbyListResponse {
//     message Lobby {
//       optional fixed64 steam_id    = 1;
//       optional int32   max_members = 2;
//       optional int32   lobby_type  = 3;
//       optional int32   lobby_flags = 4;
//       optional bytes   metadata    = 5;
//       optional int32   num_members = 6;
//       optional float   distance    = 7;
//       optional int64   weight      = 8;
//       optional int32   ping        = 9;
//       optional int32   missing_ping= 10;
//     }
//     optional uint32          app_id  = 1;
//     optional int32           eresult = 3 [default = 2];   // EResult.Fail
//     repeated Lobby           lobbies = 4;
//   }
//
// We model only the field set we actually exchange today; the wire
// parser is tolerant of unknown fields via skip().

namespace wn_steam::pb {

struct CMsgClientMMSGetLobbyListFilter {
    std::string key;
    std::string value;
    int32_t     comparision = 0;   // ELobbyComparison
    int32_t     filter_type = 0;   // ELobbyFilterType
};

struct CMsgClientMMSGetLobbyList {
    uint32_t app_id                = 0;
    int32_t  num_lobbies_requested = 50;
    uint32_t cell_id               = 0;
    std::vector<CMsgClientMMSGetLobbyListFilter> filters;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct MMSLobbyListEntry {
    uint64_t            steam_id    = 0;   // 1 fixed64
    int32_t             max_members = 0;   // 2 int32
    int32_t             lobby_type  = 0;   // 3 int32
    int32_t             lobby_flags = 0;   // 4 int32
    std::vector<uint8_t> metadata;         // 5 bytes
    int32_t             num_members = 0;   // 6 int32
    float               distance    = 0;   // 7 float
    int64_t             weight      = 0;   // 8 int64
    int32_t             ping        = 0;   // 9 int32
};

struct CMsgClientMMSGetLobbyListResponse {
    uint32_t                         app_id  = 0;   // 1 uint32
    int32_t                          eresult = 2;   // 3 int32 [default=2 Fail]
    std::vector<MMSLobbyListEntry>   lobbies;       // 4 repeated

    [[nodiscard]] static std::optional<CMsgClientMMSGetLobbyListResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

}  // namespace wn_steam::pb
