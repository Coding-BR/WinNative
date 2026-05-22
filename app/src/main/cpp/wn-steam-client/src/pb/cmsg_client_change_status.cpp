#include "wn_steam/pb/cmsg_client_change_status.h"

#include "wn_steam/proto_wire.h"

namespace wn_steam::pb {

std::vector<uint8_t> CMsgClientChangeStatus::serialize() const {
    std::vector<uint8_t> out;
    proto::Writer w(out);
    // Force-emit: persona_state 0 (Offline) is a meaningful value.
    w.uint32_field_force(1, persona_state);
    // Optional. Empty name == "don't broadcast a rename"; emit only
    // when the caller actually wants to change the displayed name.
    w.string_field(2, player_name);
    return out;
}

}  // namespace wn_steam::pb
