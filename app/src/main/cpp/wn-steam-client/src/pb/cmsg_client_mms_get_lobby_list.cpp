#include "wn_steam/pb/cmsg_client_mms_get_lobby_list.h"

#include <cstring>

#include "wn_steam/proto_wire.h"

namespace wn_steam::pb {

std::vector<uint8_t> CMsgClientMMSGetLobbyList::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    if (num_lobbies_requested > 0) {
        w.int32_field(3, num_lobbies_requested);
    }
    if (cell_id != 0) {
        w.uint32_field(4, cell_id);
    }
    for (const auto& f : filters) {
        std::vector<uint8_t> sub;
        proto::Writer fw(sub);
        if (!f.key.empty())   fw.string_field(1, f.key);
        if (!f.value.empty()) fw.string_field(2, f.value);
        fw.int32_field(3, f.comparision);
        fw.int32_field(4, f.filter_type);
        w.submessage_field(6, sub);
    }
    return out;
}

static std::optional<MMSLobbyListEntry>
parse_lobby_entry(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    MMSLobbyListEntry e;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) { if (!r.ok()) return std::nullopt; break; }
        switch (t->field_number) {
            case 1:
                if (auto v = r.fixed64(); v) e.steam_id = *v;
                else return std::nullopt;
                break;
            case 2:
                if (auto v = r.u64(); v)
                    e.max_members = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 3:
                if (auto v = r.u64(); v)
                    e.lobby_type = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 4:
                if (auto v = r.u64(); v)
                    e.lobby_flags = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 5: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                e.metadata.assign(b->begin(), b->end());
                break;
            }
            case 6:
                if (auto v = r.u64(); v)
                    e.num_members = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 7: {
                auto raw = r.fixed32();
                if (!raw) return std::nullopt;
                std::memcpy(&e.distance, &*raw, sizeof(float));
                break;
            }
            case 8:
                if (auto v = r.u64(); v)
                    e.weight = static_cast<int64_t>(*v);
                else return std::nullopt;
                break;
            case 9:
                if (auto v = r.u64(); v)
                    e.ping = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return e;
}

std::optional<CMsgClientMMSGetLobbyListResponse>
CMsgClientMMSGetLobbyListResponse::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSGetLobbyListResponse m;
    while (!r.eof()) {
        auto t = r.next_tag();
        if (!t) { if (!r.ok()) return std::nullopt; break; }
        switch (t->field_number) {
            case 1:
                if (auto v = r.u32(); v) m.app_id = *v;
                else return std::nullopt;
                break;
            case 3:
                if (auto v = r.u64(); v)
                    m.eresult = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            case 4: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                auto e = parse_lobby_entry(*b);
                if (!e) return std::nullopt;
                m.lobbies.push_back(std::move(*e));
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
