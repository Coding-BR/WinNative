#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>


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
