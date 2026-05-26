#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>


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
