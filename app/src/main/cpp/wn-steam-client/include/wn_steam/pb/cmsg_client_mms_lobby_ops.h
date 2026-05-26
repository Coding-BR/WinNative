#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>


namespace wn_steam::pb {

struct CMsgClientMMSCreateLobby {
    uint32_t                 app_id      = 0;
    int32_t                  max_members = 0;
    int32_t                  lobby_type  = 0;
    int32_t                  lobby_flags = 0;
    std::vector<uint8_t>     metadata;
    std::string              persona_name_owner;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSCreateLobbyResponse {
    uint32_t app_id          = 0;
    uint64_t steam_id_lobby  = 0;
    int32_t  eresult         = 2;   // proto2 default = Fail

    [[nodiscard]] static std::optional<CMsgClientMMSCreateLobbyResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

struct CMsgClientMMSJoinLobby {
    uint32_t    app_id         = 0;
    uint64_t    steam_id_lobby = 0;
    std::string persona_name;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSJoinLobbyResponseMember {
    uint64_t            steam_id = 0;
    std::string         persona_name;
    std::vector<uint8_t> metadata;
};

struct CMsgClientMMSJoinLobbyResponse {
    uint32_t                                          app_id                   = 0;
    uint64_t                                          steam_id_lobby           = 0;
    int32_t                                           chat_room_enter_response = 2; // Error
    int32_t                                           max_members              = 0;
    int32_t                                           lobby_type               = 0;
    int32_t                                           lobby_flags              = 0;
    uint64_t                                          steam_id_owner           = 0;
    std::vector<uint8_t>                              metadata;
    std::vector<CMsgClientMMSJoinLobbyResponseMember> members;

    [[nodiscard]] static std::optional<CMsgClientMMSJoinLobbyResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

struct CMsgClientMMSLeaveLobby {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSSetLobbyData {
    uint32_t                 app_id           = 0;
    uint64_t                 steam_id_lobby   = 0;
    uint64_t                 steam_id_member  = 0;
    int32_t                  max_members      = 0;
    int32_t                  lobby_type       = 0;
    int32_t                  lobby_flags      = 0;
    std::vector<uint8_t>     metadata;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSSetLobbyDataResponse {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;
    int32_t  eresult        = 2;

    [[nodiscard]] static std::optional<CMsgClientMMSSetLobbyDataResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

struct CMsgClientMMSSendLobbyChatMsg {
    uint32_t             app_id         = 0;
    uint64_t             steam_id_lobby = 0;
    std::vector<uint8_t> lobby_message;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSLobbyChatMsg {
    uint32_t             app_id          = 0;
    uint64_t             steam_id_lobby  = 0;
    uint64_t             steam_id_sender = 0;
    std::vector<uint8_t> lobby_message;

    [[nodiscard]] static std::optional<CMsgClientMMSLobbyChatMsg>
    deserialize(std::span<const uint8_t> body) noexcept;
};

struct CMsgClientMMSUserJoinedOrLeftLobby {
    uint32_t    app_id         = 0;
    uint64_t    steam_id_lobby = 0;
    uint64_t    steam_id_user  = 0;
    std::string persona_name;

    [[nodiscard]] static std::optional<CMsgClientMMSUserJoinedOrLeftLobby>
    deserialize(std::span<const uint8_t> body) noexcept;
};

struct CMsgClientMMSInviteToLobby {
    uint32_t app_id                = 0;
    uint64_t steam_id_lobby        = 0;
    uint64_t steam_id_user_invited = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSSetLobbyOwner {
    uint32_t app_id              = 0;
    uint64_t steam_id_lobby      = 0;
    uint64_t steam_id_new_owner  = 0;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
};

struct CMsgClientMMSSetLobbyOwnerResponse {
    uint32_t app_id         = 0;
    uint64_t steam_id_lobby = 0;
    int32_t  eresult        = 2;

    [[nodiscard]] static std::optional<CMsgClientMMSSetLobbyOwnerResponse>
    deserialize(std::span<const uint8_t> body) noexcept;
};

}  // namespace wn_steam::pb
