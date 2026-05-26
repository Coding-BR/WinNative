#include "wn_steam/pb/cmsg_client_mms_lobby_ops.h"

#include "wn_steam/proto_wire.h"

namespace wn_steam::pb {


std::vector<uint8_t> CMsgClientMMSCreateLobby::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.int32_field(2, max_members);
    w.int32_field(3, lobby_type);
    w.int32_field(4, lobby_flags);
    if (!metadata.empty()) w.bytes_field(7, metadata);
    if (!persona_name_owner.empty()) w.string_field(8, persona_name_owner);
    return out;
}


std::optional<CMsgClientMMSCreateLobbyResponse>
CMsgClientMMSCreateLobbyResponse::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSCreateLobbyResponse m;
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
                    m.eresult = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}


std::vector<uint8_t> CMsgClientMMSJoinLobby::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    if (!persona_name.empty()) w.string_field(3, persona_name);
    return out;
}


static std::optional<CMsgClientMMSJoinLobbyResponseMember>
parse_join_member(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSJoinLobbyResponseMember mb;
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

std::optional<CMsgClientMMSJoinLobbyResponse>
CMsgClientMMSJoinLobbyResponse::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSJoinLobbyResponse m;
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
                    m.chat_room_enter_response = static_cast<int32_t>(static_cast<uint32_t>(*v));
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
                auto mb = parse_join_member(*b);
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


std::vector<uint8_t> CMsgClientMMSLeaveLobby::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    return out;
}


std::vector<uint8_t> CMsgClientMMSSetLobbyData::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    if (steam_id_member != 0) w.fixed64_field(3, steam_id_member);
    w.int32_field(4, max_members);
    w.int32_field(5, lobby_type);
    w.int32_field(6, lobby_flags);
    if (!metadata.empty()) w.bytes_field(7, metadata);
    return out;
}

std::optional<CMsgClientMMSSetLobbyDataResponse>
CMsgClientMMSSetLobbyDataResponse::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSSetLobbyDataResponse m;
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
                    m.eresult = static_cast<int32_t>(static_cast<uint32_t>(*v));
                else return std::nullopt;
                break;
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}


std::vector<uint8_t> CMsgClientMMSSendLobbyChatMsg::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    if (!lobby_message.empty()) w.bytes_field(4, lobby_message);
    return out;
}


std::optional<CMsgClientMMSLobbyChatMsg>
CMsgClientMMSLobbyChatMsg::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSLobbyChatMsg m;
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
                if (auto v = r.fixed64(); v) m.steam_id_sender = *v;
                else return std::nullopt;
                break;
            case 4: {
                auto b = r.bytes();
                if (!b) return std::nullopt;
                m.lobby_message.assign(b->begin(), b->end());
                break;
            }
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}


std::optional<CMsgClientMMSUserJoinedOrLeftLobby>
CMsgClientMMSUserJoinedOrLeftLobby::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSUserJoinedOrLeftLobby m;
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
                if (auto v = r.fixed64(); v) m.steam_id_user = *v;
                else return std::nullopt;
                break;
            case 4: {
                auto s = r.string();
                if (!s) return std::nullopt;
                m.persona_name = std::move(*s);
                break;
            }
            default:
                if (!r.skip(t->wire_type)) return std::nullopt;
                break;
        }
    }
    return m;
}


std::vector<uint8_t> CMsgClientMMSInviteToLobby::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    w.fixed64_field(3, steam_id_user_invited);
    return out;
}


std::vector<uint8_t> CMsgClientMMSSetLobbyOwner::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    w.uint32_field(1, app_id);
    w.fixed64_field(2, steam_id_lobby);
    w.fixed64_field(3, steam_id_new_owner);
    return out;
}

std::optional<CMsgClientMMSSetLobbyOwnerResponse>
CMsgClientMMSSetLobbyOwnerResponse::deserialize(std::span<const uint8_t> body) noexcept {
    proto::Reader r(body);
    CMsgClientMMSSetLobbyOwnerResponse m;
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
                if (auto v = r.i32(); v) m.eresult = *v;
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
