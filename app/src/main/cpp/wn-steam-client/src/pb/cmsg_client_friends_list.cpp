#include "wn_steam/pb/cmsg_client_friends_list.h"

#include "wn_steam/proto_wire.h"

namespace wn_steam::pb {

namespace {
std::optional<ClientFriendsListEntry>
parse_entry(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    ClientFriendsListEntry e;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) {
            if (!r.ok()) return std::nullopt;
            break;
        }
        switch (t->field_number) {
            case 1:
                if (auto v = r.fixed64(); v) e.ulfriendid = *v;
                else return std::nullopt;
                break;
            case 2:
                if (auto v = r.u32(); v) e.efriendrelationship = *v;
                else return std::nullopt;
                break;
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return e;
}
}  // namespace

std::optional<CMsgClientFriendsList>
CMsgClientFriendsList::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientFriendsList m;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) {
            if (!r.ok()) return std::nullopt;
            break;
        }
        switch (t->field_number) {
            case 1:
                if (auto v = r.boolean(); v) m.bincremental = *v;
                else return std::nullopt;
                break;
            case 2: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                auto e = parse_entry(*b);
                if (!e) return std::nullopt;
                m.friends.push_back(*e);
                break;
            }
            case 3:
                if (auto v = r.u32(); v) m.max_friend_count = *v;
                else return std::nullopt;
                break;
            case 4:
                if (auto v = r.u32(); v) m.active_friend_count = *v;
                else return std::nullopt;
                break;
            case 5:
                if (auto v = r.boolean(); v) m.friends_limit_hit = *v;
                else return std::nullopt;
                break;
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}

}  // namespace wn_steam::pb
