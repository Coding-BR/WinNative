// ISteamClient (SteamClient020) — the top-level interface factory.
// `CreateInterface(name, &err)` returns a pointer to this object for
// any version string in the SteamClient006..023 family; the object's
// vtable then has GetISteam* methods that fan out to per-feature
// interfaces (provided as stubs in isteam_stubs.cpp).
//
// Method order MUST mirror Valve's public SDK (isteamclient.h):
//   0  CreateSteamPipe()
//   1  BReleaseSteamPipe(pipe)
//   2  ConnectToGlobalUser(pipe)
//   3  CreateLocalUser(&pipe, type)
//   4  ReleaseUser(pipe, user)
//   5  GetISteamUser(hUser, hPipe, ver)
//   6  GetISteamGameServer(hUser, hPipe, ver)
//   7  SetLocalIPBinding(ip, port)
//   8  GetISteamFriends(hUser, hPipe, ver)
//   9  GetISteamUtils(hPipe, ver)
//  10  GetISteamMatchmaking(hUser, hPipe, ver)
//  11  GetISteamMatchmakingServers(hUser, hPipe, ver)
//  12  GetISteamGenericInterface(hUser, hPipe, ver)
//  13  GetISteamUserStats(hUser, hPipe, ver)
//  14  GetISteamApps(hUser, hPipe, ver)
//  15  GetISteamNetworking(hUser, hPipe, ver)
//  16  GetISteamRemoteStorage(hUser, hPipe, ver)
//  17  GetISteamScreenshots(hUser, hPipe, ver)
//  18  GetISteamUGC(hUser, hPipe, ver)
//  19  GetISteamAppList(hUser, hPipe, ver)
//  20  GetISteamMusic(hUser, hPipe, ver)
//  ... 21+ Music remote / HTML surface / inventory / video / parental
//
// Consumers' slot dispatch (e.g. our bootstrap's stage-2 self-test
// hitting `sc_vt[14]` for GetISteamApps) only works if this order
// matches Valve's exactly.

#include "wn_libsteamclient/runtime_state.h"

#include <android/log.h>
#include <cstdint>
#include <cstring>

namespace wn_libsteamclient {

extern "C" void* wn_get_isteam_utils();
extern "C" void* wn_get_isteam_user();
extern "C" void* wn_get_isteam_apps();
extern "C" void* wn_get_isteam_friends();
extern "C" void* wn_get_isteam_remote_storage();
extern "C" void* wn_get_isteam_user_stats();
extern "C" void* wn_get_isteam_inventory();
extern "C" void* wn_get_isteam_screenshots();
extern "C" void* wn_get_isteam_music();
extern "C" void* wn_get_isteam_app_list();
extern "C" void* wn_get_isteam_video();
extern "C" void* wn_get_isteam_parental();
extern "C" void* wn_get_isteam_matchmaking_servers();
extern "C" void* wn_get_isteam_matchmaking();
extern "C" void* wn_get_isteam_networking();
extern "C" void* wn_get_isteam_ugc();
extern "C" void* wn_get_isteam_game_server();
extern "C" void* wn_get_isteam_music_remote();
extern "C" void* wn_get_isteam_html_surface();
extern "C" void* wn_get_isteam_input();
extern "C" void* wn_get_isteam_parties();
extern "C" void* wn_get_isteam_remote_play();
extern "C" void* wn_get_isteam_networking_sockets();
extern "C" void* wn_get_isteam_networking_utils();
extern "C" void* wn_get_isteam_networking_messages();
extern "C" void* wn_get_iclient_engine();

// Forward declaration so ISteamClientImpl::GetISteamGenericInterface
// (slot 12) can delegate to the factory below. extern "C" matches
// the public-facing definition's linkage; the forward decl lives
// inside our namespace so the in-class call resolves without
// qualification.
extern "C" void* CreateInterface(const char* version_name, int* return_code);

class ISteamClientImpl {
public:
    // 0
    virtual int  CreateSteamPipe()                           {
        int pipe = alloc_pipe();
        if (pipe == 0) pipe = state().pipe.load();
        return pipe;
    }
    // 1
    virtual bool BReleaseSteamPipe(int pipe)                 { return release_pipe(pipe); }
    // 2
    virtual int  ConnectToGlobalUser(int pipe)               { return alloc_global_user(pipe); }
    // 3
    virtual int  CreateLocalUser(int* pipe_inout, int /*type*/) {
        if (!pipe_inout) return 0;
        int p = alloc_pipe(); if (p == 0) p = state().pipe.load();
        int u = alloc_global_user(p);
        *pipe_inout = p;
        return u;
    }
    // 4
    virtual void ReleaseUser(int pipe, int user)             { release_user(pipe, user); }
    // 5
    virtual void* GetISteamUser(int /*u*/, int /*p*/, const char* /*v*/)              { return wn_get_isteam_user(); }
    // 6
    virtual void* GetISteamGameServer(int, int, const char*)                          { return wn_get_isteam_game_server(); }
    // 7
    virtual void  SetLocalIPBinding(uint32_t, uint16_t)                               {}
    // 8
    virtual void* GetISteamFriends(int /*u*/, int /*p*/, const char* /*v*/)           { return wn_get_isteam_friends(); }
    // 9
    virtual void* GetISteamUtils(int /*p*/, const char* /*v*/)                        { return wn_get_isteam_utils(); }
    // 10
    virtual void* GetISteamMatchmaking(int, int, const char*)                         { return wn_get_isteam_matchmaking(); }
    // 11
    virtual void* GetISteamMatchmakingServers(int, int, const char*)                  { return wn_get_isteam_matchmaking_servers(); }
    // 12
    // 12 — GetISteamGenericInterface(hUser, hPipe, version). Legacy
    //   name-based dispatch for any interface the dedicated GetISteam*
    //   slots don't cover. Modern games call CreateInterface() instead;
    //   the few legacy probes that still hit this slot expect a
    //   prefix-match against the registered version-string family.
    //
    //   We delegate to CreateInterface (which already has all of our
    //   registered surfaces) — the legacy path then resolves through
    //   the same code games would use directly, so adding new
    //   interfaces never needs a corresponding slot-12 wire.
    virtual void* GetISteamGenericInterface(int, int, const char* version) {
        int err = 0;
        return CreateInterface(version, &err);
    }
    // 13
    virtual void* GetISteamUserStats(int /*u*/, int /*p*/, const char* /*v*/)         { return wn_get_isteam_user_stats(); }
    // 14
    virtual void* GetISteamApps(int /*u*/, int /*p*/, const char* /*v*/)              { return wn_get_isteam_apps(); }
    // 15
    virtual void* GetISteamNetworking(int, int, const char*)                          { return wn_get_isteam_networking(); }
    // 16
    virtual void* GetISteamRemoteStorage(int /*u*/, int /*p*/, const char* /*v*/)     { return wn_get_isteam_remote_storage(); }
    // 17
    virtual void* GetISteamScreenshots(int, int, const char*)                         { return wn_get_isteam_screenshots(); }
    // 18
    virtual void* GetISteamUGC(int, int, const char*)                                 { return wn_get_isteam_ugc(); }
    // 19
    virtual void* GetISteamAppList(int, int, const char*)                             { return wn_get_isteam_app_list(); }
    // 20
    virtual void* GetISteamMusic(int, int, const char*)                               { return wn_get_isteam_music(); }
    // 21
    virtual void* GetISteamMusicRemote(int, int, const char*)                         { return wn_get_isteam_music_remote(); }
    // 22
    virtual void* GetISteamHTMLSurface(int, int, const char*)                         { return wn_get_isteam_html_surface(); }
    // 23
    virtual void  Set_SteamAPI_CPostAPIResultInProcess(void*)                         {}
    // 24
    virtual void  Remove_SteamAPI_CPostAPIResultInProcess(void*)                      {}
    // 25
    virtual void  Set_SteamAPI_CCheckCallbackRegisteredInProcess(void*)               {}
    // 26
    virtual void* GetISteamInventory(int, int, const char*)                           { return wn_get_isteam_inventory(); }
    // 27
    virtual void* GetISteamVideo(int, int, const char*)                               { return wn_get_isteam_video(); }
    // 28
    virtual void* GetISteamParentalSettings(int, int, const char*)                    { return wn_get_isteam_parental(); }
    // 29
    virtual void* GetISteamInput(int, int, const char*)                               { return wn_get_isteam_input(); }
    // 30
    virtual void* GetISteamParties(int, int, const char*)                             { return wn_get_isteam_parties(); }
    // 31
    virtual void* GetISteamRemotePlay(int, int, const char*)                          { return wn_get_isteam_remote_play(); }
};

// Process-singleton. Pointers handed out by CreateInterface are this.
static ISteamClientImpl g_steam_client;

}  // namespace wn_libsteamclient

// ---------------------------------------------------------------------------
// CreateInterface — top-level interface factory. The bootstrap calls
// this with "SteamClient020" (or any other version); a real Valve
// build maintains a registry of named factories. For our initial
// drop-in we return the same ISteamClientImpl pointer for ANY
// `SteamClientNNN` version, since our vtable is wire-compatible with
// the public SDK's stable method order across 006..023.
//
// Returns nullptr (and writes -1 to *err) for unknown names — that's
// the Valve convention for "interface not found".
// ---------------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
void* CreateInterface(const char* version_name, int* return_code) {
    if (!version_name) {
        if (return_code) *return_code = -1;
        return nullptr;
    }
    // Trace every probe so the launch logcat lists exactly which
    // interface names the game asks for (incl. ones we successfully
    // dispatch). Useful when diagnosing why a game silently skips
    // a feature — the absence of a probe in the log tells us the
    // game never tried that path.
    __android_log_print(ANDROID_LOG_INFO, "WnLibSteamClient",
        "CreateInterface(%s)", version_name);
    // Accept the entire SteamClient006..023 family on the same vtable.
    if (std::strncmp(version_name, "SteamClient", 11) == 0) {
        if (return_code) *return_code = 0;
        return &wn_libsteamclient::g_steam_client;
    }
    // ISteamNetworkingSockets / Utils / Messages — modern (post-2019) P2P
    // and Steam-Datagram-Relay API. Games that probe these via
    // CreateInterface() previously hit nullptr (NOT-EXPORTED), then
    // either crashed or silently fell back to legacy ISteamNetworking.
    // The stubs return invalid handles / k_EResultNoConnection on every
    // call — real P2P traffic still doesn't flow, but games stop
    // crashing on the lookup. Accept the entire version family
    // (SteamNetworkingSockets006..014, SteamNetworkingUtils001..004,
    // SteamNetworkingMessages001..002) on the same vtable since our
    // stub layout matches the stable method order across versions.
    if (std::strncmp(version_name, "SteamNetworkingSockets", 22) == 0) {
        if (return_code) *return_code = 0;
        return wn_libsteamclient::wn_get_isteam_networking_sockets();
    }
    if (std::strncmp(version_name, "SteamNetworkingUtils", 20) == 0) {
        if (return_code) *return_code = 0;
        return wn_libsteamclient::wn_get_isteam_networking_utils();
    }
    if (std::strncmp(version_name, "SteamNetworkingMessages", 23) == 0) {
        if (return_code) *return_code = 0;
        return wn_libsteamclient::wn_get_isteam_networking_messages();
    }
    // ISteam* per-interface CreateInterface entry points. The wine PE
    // bridge (lsteamclient.dll/.so) dlsyms our CreateInterface and
    // calls it with version-specific names like "SteamMatchMaking009",
    // "SteamUser021", "SteamFriends017", etc. — separate from the
    // ISteamClient.GetISteamX vtable path. Without these returns, the
    // bridge dispatches via gbe_fork on the C++ inline path and crashes
    // when the wrapper attempts the equivalent vtable call against our
    // libsteamclient.so's CreateInterface-returned nullptr.
    //
    // All version variants share the same stub singleton because the
    // stable method order (per SteamWorks SDK conventions) is preserved
    // across our supported versions.
    auto dispatch_iface = [&](const char* prefix, int prefix_len,
                              void* (*getter)()) -> void* {
        if (std::strncmp(version_name, prefix, prefix_len) != 0) return nullptr;
        if (return_code) *return_code = 0;
        return getter();
    };
    if (void* p = dispatch_iface("SteamMatchMaking",  16, wn_libsteamclient::wn_get_isteam_matchmaking))         return p;
    if (void* p = dispatch_iface("SteamMatchMakingServers", 23, wn_libsteamclient::wn_get_isteam_matchmaking_servers)) return p;
    if (void* p = dispatch_iface("SteamUser",         9,  wn_libsteamclient::wn_get_isteam_user))                return p;
    if (void* p = dispatch_iface("SteamFriends",      12, wn_libsteamclient::wn_get_isteam_friends))             return p;
    if (void* p = dispatch_iface("SteamUtils",        10, wn_libsteamclient::wn_get_isteam_utils))               return p;
    if (void* p = dispatch_iface("STEAMAPPS_INTERFACE_VERSION", 26, wn_libsteamclient::wn_get_isteam_apps))      return p;
    if (void* p = dispatch_iface("STEAMUSERSTATS_INTERFACE_VERSION", 31, wn_libsteamclient::wn_get_isteam_user_stats)) return p;
    if (void* p = dispatch_iface("STEAMREMOTESTORAGE_INTERFACE_VERSION", 35, wn_libsteamclient::wn_get_isteam_remote_storage)) return p;
    if (void* p = dispatch_iface("STEAMSCREENSHOTS_INTERFACE_VERSION", 33, wn_libsteamclient::wn_get_isteam_screenshots)) return p;
    if (void* p = dispatch_iface("STEAMINVENTORY_INTERFACE_V", 26, wn_libsteamclient::wn_get_isteam_inventory))  return p;
    if (void* p = dispatch_iface("STEAMVIDEO_INTERFACE_V",     22, wn_libsteamclient::wn_get_isteam_video))      return p;
    if (void* p = dispatch_iface("STEAMMUSIC_INTERFACE_VERSION", 28, wn_libsteamclient::wn_get_isteam_music))    return p;
    if (void* p = dispatch_iface("STEAMMUSICREMOTE_INTERFACE_VERSION", 33, wn_libsteamclient::wn_get_isteam_music_remote)) return p;
    if (void* p = dispatch_iface("STEAMHTMLSURFACE_INTERFACE_",27, wn_libsteamclient::wn_get_isteam_html_surface)) return p;
    if (void* p = dispatch_iface("STEAMUGC_INTERFACE_VERSION", 26, wn_libsteamclient::wn_get_isteam_ugc))        return p;
    if (void* p = dispatch_iface("STEAMAPPLIST_INTERFACE_VERSION", 30, wn_libsteamclient::wn_get_isteam_app_list)) return p;
    if (void* p = dispatch_iface("STEAMPARENTALSETTINGS_INTERFACE_VERSION", 38, wn_libsteamclient::wn_get_isteam_parental)) return p;
    if (void* p = dispatch_iface("SteamGameServer",   15, wn_libsteamclient::wn_get_isteam_game_server))         return p;
    if (void* p = dispatch_iface("SteamNetworking",   15, wn_libsteamclient::wn_get_isteam_networking))          return p;
    if (void* p = dispatch_iface("SteamInput",        10, wn_libsteamclient::wn_get_isteam_input))               return p;
    if (void* p = dispatch_iface("SteamParties",      12, wn_libsteamclient::wn_get_isteam_parties))             return p;
    if (void* p = dispatch_iface("SteamRemotePlay",   15, wn_libsteamclient::wn_get_isteam_remote_play))         return p;
    // CLIENTENGINE_INTERFACE_VERSION005 returns the IClientEngine
    // singleton. Its vtable layout differs from ISteamClient (slot 8
    // is GetIClientUser, etc.); see iclient_engine.cpp. Returning the
    // proper IClientEngine impl makes the bootstrap's logon path
    // (IClientEngine.GetIClientUser → IClientUser.SetSteamID /
    // SetLoginInformation / LogonWithRefreshToken) call into OUR code.
    if (std::strncmp(version_name,
                     "CLIENTENGINE_INTERFACE_VERSION", 30) == 0) {
        if (return_code) *return_code = 0;
        return wn_libsteamclient::wn_get_iclient_engine();
    }
    if (return_code) *return_code = -1;
    __android_log_print(ANDROID_LOG_WARN, "WnLibSteamClient",
        "CreateInterface: unknown name='%s' — returning null", version_name);
    return nullptr;
}

// SteamInternal_* — the symbols the modern flat-C SDK macros expand
// into. `SteamUser()` for example expands to
//   SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(),
//                                            "SteamUser023")
// at link time. Without these exports games using the inline-defined
// accessor macros (most post-2014 titles) fail dlsym at SteamAPI_Init.
//
// The Wine-side lsteamclient.dll bridge also dlsyms these when
// translating flat-C calls — confirmed by inspecting the
// proton-experimental lsteamclient.dll.so reloc table.
//
// For our purposes the singleton interfaces are user-handle-agnostic:
// each interface struct is process-singleton, the HSteamUser arg is
// ignored. SteamInternal_CreateInterface is the no-user variant
// (server/utils path) — same dispatch as CreateInterface.
extern "C" __attribute__((visibility("default")))
void* SteamInternal_FindOrCreateUserInterface(int /*hSteamUser*/,
                                              const char* version_name) {
    int rc = 0;
    return CreateInterface(version_name, &rc);
}

extern "C" __attribute__((visibility("default")))
void* SteamInternal_FindOrCreateGameServerInterface(int /*hSteamUser*/,
                                                    const char* version_name) {
    // Game-server variant. ISteamGameServer*, ISteamGameServerStats,
    // ISteamMasterServerUpdater, ISteamGameServerNetworking{,Sockets}
    // all share the same singletons today — server vs client distinction
    // matters only for accessor behavior (e.g. GetSteamID returns the
    // server's SID instead of the user's). Wire it through CreateInter
    // face for now; promote to dedicated server vtable if a real
    // dedicated-server game lands.
    int rc = 0;
    return CreateInterface(version_name, &rc);
}

extern "C" __attribute__((visibility("default")))
void* SteamInternal_CreateInterface(const char* version_name) {
    int rc = 0;
    return CreateInterface(version_name, &rc);
}
