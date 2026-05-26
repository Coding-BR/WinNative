#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>


namespace wn_steam::pb {

struct ClientFriendsListEntry {
    uint64_t ulfriendid           = 0;
    uint32_t efriendrelationship  = 0;  // EFriendRelationship; 3 = Friend
};

struct CMsgClientFriendsList {
    bool                                bincremental        = false;
    std::vector<ClientFriendsListEntry> friends;
    uint32_t                            max_friend_count    = 0;
    uint32_t                            active_friend_count = 0;
    bool                                friends_limit_hit   = false;

    [[nodiscard]] static std::optional<CMsgClientFriendsList>
    deserialize(std::span<const uint8_t> body) noexcept;
};

}  // namespace wn_steam::pb
