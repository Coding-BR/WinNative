#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CMsgClientChangeStatus (EMsg 716). Publishes the client's persona state
// (online/offline/away/…) and optionally a new player_name so Steam
// friends see the change. Fire-and-forget; no response. Serialize-only.
//
//   1 uint32 persona_state  (EPersonaState: 0 Offline, 1 Online, 2 Busy,
//                            3 Away, 4 Snooze, 5 LookingToTrade, 6 LookingToPlay)
//   2 string player_name    (optional; non-empty broadcasts the rename)
//
// Field numbers verified against canonical JavaSteam
//   steammessages_clientserver_friends.proto.

namespace wn_steam::pb {

struct CMsgClientChangeStatus {
    uint32_t    persona_state = 0;
    std::string player_name;   // empty = don't broadcast name change

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

}  // namespace wn_steam::pb
