/* Flat-C ISteamClient accessor overrides for matchmaking.
 *
 * Steamworks.NET (Unity P/Invoke) calls these flat-C exports directly:
 *
 *     SteamAPI_ISteamClient_GetISteamMatchmaking(instancePtr, hUser, hPipe, ver)
 *
 * If we leave them forwarded to gbe_fork, gbe dispatches on its own
 * matchmaking. Owning them here lets P/Invoke callers reach our
 * libsteamclient.so matchmaking pointer.
 *
 * History: the more invasive SteamClient vtable wrapper (this file's
 * earlier state) crashed Forest even with an Init-gate. Forest takes
 * the C++ inline `SteamMatchmaking()` path which goes through
 * SteamClient()->vtable[10] and never hits these flat-C accessors —
 * so on Forest's path our pointer never replaces gbe's, and the
 * crash is avoided. Future iterations can revisit the wrapper after
 * verifying our libsteamclient.so matchmaking matches the exact SDK
 * v009 vtable layout Forest expects.
 */

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define WN_STEAMAPI_EXPORT __declspec(dllexport)

extern void* get_our_matchmaking(void);
extern void* get_our_matchmaking_servers(void);

static int g_logged_matchmaking = 0;
static int g_logged_matchmaking_servers = 0;
static void wnb_marker(const char* msg) {
    FILE* f = fopen("C:\\wnb.log", "a");
    if (f) { fputs(msg, f); fputs("\n", f); fclose(f); }
}

WN_STEAMAPI_EXPORT void* SteamAPI_ISteamClient_GetISteamMatchmaking(
        void* instancePtr,
        int hSteamUser,
        int hSteamPipe,
        const char* pchVersion) {
    (void)instancePtr; (void)hSteamUser; (void)hSteamPipe; (void)pchVersion;
    if (!g_logged_matchmaking) {
        wnb_marker("SteamAPI_ISteamClient_GetISteamMatchmaking: flat-C hook -> libsteamclient.so");
        g_logged_matchmaking = 1;
    }
    return get_our_matchmaking();
}

WN_STEAMAPI_EXPORT void* SteamAPI_ISteamClient_GetISteamMatchmakingServers(
        void* instancePtr,
        int hSteamUser,
        int hSteamPipe,
        const char* pchVersion) {
    (void)instancePtr; (void)hSteamUser; (void)hSteamPipe; (void)pchVersion;
    if (!g_logged_matchmaking_servers) {
        wnb_marker("SteamAPI_ISteamClient_GetISteamMatchmakingServers: flat-C hook -> libsteamclient.so");
        g_logged_matchmaking_servers = 1;
    }
    return get_our_matchmaking_servers();
}

/* ── Steam Launcher: bare global accessor overrides ────────────────────────────
 *
 * The Steamworks SDK C++ header `isteammatchmaking.h` declares
 *   STEAM_DEFINE_USER_INTERFACE_ACCESSOR( ISteamMatchmaking *, SteamMatchmaking, ... )
 * which expands to an extern "C" `SteamMatchmaking()` symbol that games
 * call via inline `::SteamMatchmaking()->CreateLobby(...)` etc.
 *
 * Before Steam Launcher, the bridge's `.def` PE-forwarded `SteamMatchmaking` and
 * `SteamMatchmakingServers` to `original_steam_api64.dll` (gbe_fork).
 * gbe_fork's matchmaking is an in-process emulator with no real CM
 * connection — its lobby advertise is UDP broadcast on the local
 * subnet, which is exactly why Forest's lobbies show up under
 * "Dedicated (LAN)" but not "Dedicated (Internet)".
 *
 * Under Steam Launcher:
 *  - Wine's `lsteamclient.dll` bridge is deleted from system32 for the
 *    Steam Launcher process tree (XServerDisplayActivity Steam Launcher scrub).
 *  - `C:\Program Files (x86)\Steam\steamclient64.dll` is now Valve's
 *    REAL 25 MB Windows DLL (staged from `valve-steam-x86_64.tzst`),
 *    not the Proton lsteamclient forwarder.
 *  - `get_our_matchmaking()` in overrides.c calls
 *    LoadLibrary("steamclient64.dll") → CreateInterface("SteamClient020")
 *    → vtable[10] GetISteamMatchmaking, all hitting Valve's real DLL.
 *  - Routing the bare global accessors through `get_our_matchmaking()`
 *    therefore lands every C++ inline `SteamMatchmaking()` call on
 *    Valve's real ISteamMatchmaking — which dispatches over Valve's
 *    in-Wine-session named-object IPC to wn-steam-launcher.exe's
 *    in-process Valve client, which talks to Valve's CMs.
 *
 * The `.def` forwards at lines 1025-1026 + 1170-1171 are removed.
 */
WN_STEAMAPI_EXPORT void* SteamMatchmaking(void) {
    static int logged = 0;
    if (!logged) {
        wnb_marker("SteamMatchmaking(): bare global -> Steam Launcher Valve client");
        logged = 1;
    }
    return get_our_matchmaking();
}

WN_STEAMAPI_EXPORT void* SteamMatchmakingServers(void) {
    static int logged = 0;
    if (!logged) {
        wnb_marker("SteamMatchmakingServers(): bare global -> Steam Launcher Valve client");
        logged = 1;
    }
    return get_our_matchmaking_servers();
}

/* Versioned accessors used by some Steamworks SDK builds (the macro
 * STEAM_DEFINE_USER_INTERFACE_ACCESSOR emits both the unversioned
 * `SteamMatchmaking` and a `SteamAPI_SteamMatchmaking_v009` alias).
 * Same routing as above. */
WN_STEAMAPI_EXPORT void* SteamAPI_SteamMatchmaking_v009(void) {
    return get_our_matchmaking();
}

WN_STEAMAPI_EXPORT void* SteamAPI_SteamMatchmakingServers_v002(void) {
    return get_our_matchmaking_servers();
}
