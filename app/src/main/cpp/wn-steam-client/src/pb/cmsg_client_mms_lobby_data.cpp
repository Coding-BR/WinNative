#include "wn_steam/pb/cmsg_client_mms_lobby_data.h"

#include "wn_steam/proto_wire.h"

namespace wn_steam::pb {

static std::optional<MMSLobbyDataMember>
parse_member(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    MMSLobbyDataMember mb;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) { if (!r.ok()) return std::nullopt; break; }
        switch (t->field_number) {
            case 1:
                if (auto v = r.fixed64(); v) mb.steam_id = *v;
                else return std::nullopt;
                break;
            case 2: {
                auto s = r.string();
                if (!s) return std::nullopt;
                mb.persona_name = std::move(*s);
                break;
            }
            case 3: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                mb.metadata.assign(b->begin(), b->end());
                break;
            }
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return mb;
}

std::optional<CMsgClientMMSLobbyData>
CMsgClientMMSLobbyData::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSLobbyData m;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) { if (!r.ok()) return std::nullopt; break; }
        switch (t->field_number) {
            case 1:
                if (auto v = r.u32(); v) m.app_id = *v;
                else return std::nullopt;
                break;
            case 2:
                if (auto v = r.fixed64(); v) m.steam_id_lobby = *v;
                else return std::nullopt;
                break;
            case 3:
                if (auto v = r.u64(); v)
                    m.num_members = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 4:
                if (auto v = r.u64(); v)
                    m.max_members = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 5:
                if (auto v = r.u64(); v)
                    m.lobby_type = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 6:
                if (auto v = r.u64(); v)
                    m.lobby_flags = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 7:
                if (auto v = r.fixed64(); v) m.steam_id_owner = *v;
                else return std::nullopt;
                break;
            case 8: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                m.metadata.assign(b->begin(), b->end());
                break;
            }
            case 9: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                auto mb = parse_member(*b);
                if (!mb) return std::nullopt;
                m.members.push_back(std::move(*mb));
                break;
            }
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}

}  // namespace wn_steam::pb
