#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// CMsgClientFriendsList (EMsg 767). Server-pushed at logon (full snapshot)
// and incrementally afterward (bincremental=true) when relationships
// change. We parse only the fields we need: the (friendid, relationship)
// tuples — relationship 3 = k_EFriendRelationshipFriend (mutual). Anything
// else is a request/pending/blocked state we don't surface yet.
//
// Field numbers verified against steammessages_clientserver_friends.proto.
//
//   message CMsgClientFriendsList {
//     optional bool   bincremental         = 1;
//     repeated Friend friends              = 2;
//     optional uint32 max_friend_count     = 3;
//     optional uint32 active_friend_count  = 4;
//     optional bool   friends_limit_hit    = 5;
//   }
//   message CMsgClientFriendsList.Friend {
//     optional fixed64 ulfriendid         = 1;
//     optional uint32  efriendrelationship = 2;
//   }

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
