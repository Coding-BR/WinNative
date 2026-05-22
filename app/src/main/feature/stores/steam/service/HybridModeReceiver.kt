package com.winlator.cmod.feature.stores.steam.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.winlator.cmod.feature.stores.steam.utils.PrefManager
import com.winlator.cmod.feature.stores.steam.wnsteam.WnSteamBootstrap

/**
 * Hybrid Steam mode adb-driven control receiver. Lets the cron-driven
 * test loop flip [PrefManager.wnHybridMode] and observe the takeover
 * state without UI navigation.
 *
 * Triggers:
 *   adb shell am broadcast -a com.winnative.cmod.action.HYBRID_MODE \
 *     --es op enable     # flip wnHybridMode on + trigger takeover
 *   adb shell am broadcast -a com.winnative.cmod.action.HYBRID_MODE \
 *     --es op disable    # flip wnHybridMode off + release hand-off
 *   adb shell am broadcast -a com.winnative.cmod.action.HYBRID_MODE \
 *     --es op toggle     # flip whichever direction current state isn't
 *   adb shell am broadcast -a com.winnative.cmod.action.HYBRID_MODE \
 *     --es op query      # one-line summary (no mutation)
 *   adb shell am broadcast -a com.winnative.cmod.action.HYBRID_MODE \
 *     --es op state      # query + verbose dump of every read-side
 *                        # accessor (user/apps/cloud/stats/friends)
 *
 * Enable / disable / toggle write [PrefManager.wnHybridMode] AND call
 * [SteamService.setHybridModeRuntime] so the takeover swap fires
 * immediately (when isLoggedIn). Query logs current state without
 * mutating anything. Every action emits a single Timber.i line tagged
 * "HybridModeReceiver" + a single Log.d so the cron loop's logcat grep
 * has a stable marker.
 *
 * Exported in AndroidManifest with no permission — purely a debug
 * hook. The behavior it exposes is reachable from the Settings UI
 * anyway (Settings > Debug > "WN Hybrid Steam mode"); this receiver
 * just makes it scriptable from the host shell.
 */
class HybridModeReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return
        val op = intent.getStringExtra("op")?.lowercase() ?: "query"
        val before = PrefManager.wnHybridMode
        val bsInitialized = WnSteamBootstrap.isLoggedOn() ||
            // isLoggedOn requires `initialized` so a true reading proves
            // both. For non-logged states fall back to the steamId
            // accessor which also requires init.
            WnSteamBootstrap.steamId() != 0L

        val after: Boolean = when (op) {
            "enable"  -> true
            "disable" -> false
            "toggle"  -> !before
            "query"   -> before
            "state"   -> before    // verbose query, no mutation
            "forceinit" -> before  // fires WnSteamBootstrap.prewarm directly,
                                   // bypassing the wn-session-logged-on
                                   // gate — diagnostic-only path for
                                   // exercising the JNI surface when
                                   // Steam-side auth is unavailable
            "seedteststats" -> before  // pushes a synthetic schema into
                                       // libsteamclient.so — proves the
                                       // ISteamUserStats wiring without
                                       // needing a wn-session logon for
                                       // a specific app
            "seedtestfriends" -> before  // pushes 2 synthetic friend
                                         // personas + a self-persona
                                         // change — proves the
                                         // PersonaStateChange_t emission
                                         // chain end-to-end
            "seedtestrichpresence" -> before  // exercises ISteamFriends RP
                                              // slots 43-46 + friend-side
                                              // push — proves the
                                              // FriendRichPresenceUpdate_t
                                              // emission chain end-to-end
            "seedtestoverlay" -> before  // cycles setGameOverlayActive
                                         // true→false→true — proves
                                         // GameOverlayActivated_t emission
                                         // + transition de-dup at the
                                         // JNI boundary
            "probebridge" -> before      // calls WnLibSteamClient.setPersonaState
                                         // to exercise the cm_bridge → live
                                         // CMClient dispatch — logs whether
                                         // the bridge resolved to an active
                                         // session or dropped the call
            "probebridgename" -> before  // calls ISteamFriends.SetPersonaName
                                         // through the stub vtable to exercise
                                         // the cm_bridge → CMsgClientChangeStatus
                                         // (player_name) path end-to-end
            "probebridgereq" -> before   // calls ISteamFriends.RequestUserInformation
                                         // (slot 37) for an UN-CACHED sid to drive
                                         // the cm_bridge → CMsgClientRequestFriendData
                                         // path; verifies bridge fires when stub
                                         // doesn't short-circuit on cache hit
            "probebridgereqbulk" -> before  // drives cm_bridge bulk variant for an
                                            // array of SteamID64s — proves the
                                            // wn-cm-bridge handles batches in one
                                            // CMsgClientRequestFriendData send
            "probebridgeticket" -> before   // probes cm_bridge's cached-
                                            // ownership-ticket lookup — until
                                            // wn-session pre-fetches, expect
                                            // cache miss (size=0)
            "probeauthwrap" -> before       // injects synthetic ownership bytes,
                                            // calls GetAuthSessionTicket vtable,
                                            // verifies 24-byte SteamKit2-style
                                            // header is prepended correctly
            "probebridgeplayed" -> before   // toggles setAppId 0→N→0 to exercise
                                            // cm_bridge.wn_cm_notify_games_played
                                            // (CMsgClientGamesPlayed broadcast for
                                            // "Playing X" friend presence)
            "probebridgerp" -> before       // calls ISteamFriends.RequestFriendRich
                                            // Presence vtable slot 48 — drives
                                            // cm_bridge with the RP flag bit set,
                                            // emits synthetic FriendRichPresence
                                            // Update_t immediately
            "probebridgeclearrp" -> before  // calls SetRichPresence then
                                            // ClearRichPresence (slots 43+44) —
                                            // verifies the empty-map broadcast
                                            // path through Player.SetRichPresence#1
            "probebridgelogon" -> before    // synthetically dispatches the cm_
                                            // bridge logon-state observer —
                                            // verifies SteamServersConnected_t /
                                            // Disconnected_t emit on transition
            "probebridgefriends" -> before  // synthetically dispatches the cm_
                                            // bridge friends-list observer with
                                            // a 3-SID array — verifies
                                            // pushed.friends mirror
            "probebridgelicenses" -> before // injects 3 licenses (1 self-owned,
                                            // 2 family-shared from different
                                            // accounts) and verifies pushed.
                                            // licenses owner_ids mirror
            "probepurchasetime" -> before   // pushes synthetic licenses +
                                            // app→packages map, calls slot 8
                                            // GetEarliestPurchaseUnixTime via
                                            // bootstrap, verifies min(time)
                                            // resolution across packages
            "probefamilyshare" -> before    // pushes self-owned + family-shared
                                            // license cases, verifies slot 27
                                            // dynamic resolution returns the
                                            // correct per-app answer
            "probeappowner" -> before       // verifies slot 20 GetAppOwner
                                            // returns the correct CSteamID64
                                            // for direct + family-shared cases
                                            // (composed from license owner_id
                                            // + standard individual bits)
            "probeavgrate" -> before        // calls UpdateAvgRateStat (slot 5)
                                            // multiple times, verifies running
                                            // average via GetStat(float) (slot 2)
            "probetimedtrial" -> before     // injects a trial license (90 min
                                            // allowed, 12 used), wires app→pkg,
                                            // verifies slot 28 returns correct
                                            // seconds_allowed/played
            "probedlcinstalled" -> before   // verifies slot 7 contract across
                                            // 4 cases (owned+installed direct,
                                            // owned via parent install, owned
                                            // but uninstalled, unowned)
            "probebetaname" -> before       // pushes a beta branch via
                                            // nativeSetAppCurrentBeta, binds
                                            // appId, verifies slot 15 returns
                                            // it; null branch → false
            "probedlprogress" -> before     // pushes (1234, 5000) via
                                            // nativeSetAppDownloadProgress,
                                            // verifies slot 22 returns true +
                                            // matching counters; cleared → false
            "probecloudio" -> before        // points the bound app's cloud
                                            // remote dir at /sdcard cache,
                                            // FileWrite + FileRead round-trips
                                            // bytes, FileDelete unlinks +
                                            // clears mirror
            "probecloudasync" -> before     // FileWriteAsync (slot 2) +
                                            // FileReadAsync (slot 3) +
                                            // FileReadAsyncComplete (slot 4):
                                            // verifies hCall non-zero, byte
                                            // round-trip, single-shot complete
            "probecloudstream" -> before    // FileWriteStream{Open,WriteChunk,
                                            // Close,Cancel} (slots 9-12):
                                            // multi-chunk write commits to
                                            // disk + mirror; cancel rolls
                                            // back; unknown handle rejected
            "probeappsbool" -> before       // ISteamApps slots 0/1/2/3 vs
                                            // synthetic owned/low-violence/
                                            // VAC state — verifies SDK
                                            // contract on each
            "probefilestate" -> before      // FileForget (5) + FilePersisted
                                            // (14) round-trip against the
                                            // cloud_files mirror
            "probeuserbool" -> before       // ISteamUser slots 26-29 (phone/
                                            // 2FA flags) + slot 32 duration-
                                            // control no-op
            "probeaccountinfo" -> before    // cm_bridge account-info observer
                                            // injection — verifies the
                                            // CMsgClientAccountInfo → pushed
                                            // -state path end-to-end
            "probeuserdata" -> before       // ISteamUser slot 6 GetUserData
                                            // Folder: strips trailing /remote
                                            // off the pushed cloud dir
            "probefriendrel" -> before      // ISteamFriends slots 5 + 17:
                                            // GetFriendRelationship returns
                                            // 3 for in-list, 0 for not;
                                            // HasFriend matches Immediate mask
            "probewebapiticket" -> before   // ISteamUser slot 14: synthetic
                                            // ticket allocated for the
                                            // "myWebApi" identity; verifies
                                            // non-zero handle
            "probelicense" -> before        // ISteamUser slot 18: HasLicense
                                            // for self+owned, NoAuth for
                                            // other users, DoesNotHaveLicense
                                            // for self+un-owned
            "probecorrupt" -> before        // ISteamApps slot 16: marks the
                                            // bound app corrupt, polls the
                                            // flag, clears it
            "probefriendlevel" -> before    // ISteamFriends slot 10:
                                            // GetFriendSteamLevel returns
                                            // pushed value or 0 if unknown
            "probeplayerlevel" -> before    // ISteamUser slots 23/24/25/30/31
                                            // — player level + game badge,
                                            // async store/market/duration
            "probecloudshare" -> before     // ISteamRemoteStorage slot 7
                                            // (FileShare) + 25 (GetFileDetails)
                                            // — write a file then share +
                                            // fetch details async
            "probenicksig" -> before        // ISteamFriends 11 GetPlayer
                                            // Nickname + ISteamUtils 19
                                            // CheckFileSignature
            "probebridgepersona" -> before  // synthetically dispatches the cm_
                                            // bridge persona observer with a
                                            // full payload — verifies pushed
                                            // state mirror + PSC + RP emits
            "probebridgepersonaself" -> before  // same as probebridgepersona but
                                                // sets self steamId first so the
                                                // observer takes the self path
                                                // (pushed.persona_name + state)
            "seedtestavatar" -> before   // pushes a synthetic 4×4 RGBA8
                                         // avatar for a friend — proves
                                         // ISteamFriends slot 34 →
                                         // ISteamUtils slots 5/6 round
                                         // trip + AvatarImageLoaded_t
                                         // emission end-to-end
            "seedtestavatarhash" -> before  // pushes a synthetic 20-byte
                                            // avatar hash + verifies the
                                            // change-emit + idempotent-set
                                            // contract (PersonaStateChange_t
                                            // with kPersonaChangeAvatar)
            "seedtestfetchavatar" -> before  // fetches a known-stable avatar
                                             // from the akamai CDN, decodes
                                             // via BitmapFactory, RGBA-
                                             // converts, pushes — proves the
                                             // end-to-end network → image
                                             // pipeline (needs internet)
            "seedtestselfavatar" -> before   // sets a synthetic self
                                             // steamId, fetches the known
                                             // default-avatar hash via the
                                             // AvatarFetcher under self sid
                                             // — proves the SteamService
                                             // self-persona wiring without
                                             // needing wn-session logon
            "seedtestprefswarmup" -> before  // seeds PrefManager with a
                                             // synthetic sid + name +
                                             // hash, calls
                                             // seedFromPrefManager, polls
                                             // for the avatar handle —
                                             // proves the cold-boot
                                             // restore path end-to-end
            "seedtestfriendsnapshot" -> before  // pushes a 2-friend JSON
                                                // via pushFriendPersonasJson
                                                // then replays from the
                                                // persisted snapshot —
                                                // proves the warmup path
                                                // restores friends list
            "seedtestschemacache" -> before     // writes a synthetic per-app
                                                // schema cache file, calls
                                                // warmAchievementSchemaFromCache
                                                // — proves the file-backed
                                                // per-app schema persistence
            "seedtestavatartiers" -> before     // calls enqueueAllTiers, polls
                                                // ISteamFriends slots 34/35/36
                                                // + ISteamUtils slot 5 to verify
                                                // all three avatar handles land
            "togglelogon"   -> before    // cycles setLoggedOn(false)→
                                         // setLoggedOn(true) — proves
                                         // SteamServersConnected_t /
                                         // SteamServersDisconnected_t
                                         // emit on transitions only
            "seedtestquota" -> before    // pushes a synthetic 5 GB/5 GB
                                         // quota — proves the
                                         // setCloudQuota → state-row
                                         // path independent of a
                                         // wn-session logon
            "seedtestcloud" -> before    // pushes 2 synthetic cloud
                                         // files for appId=440 — proves
                                         // RemoteStorageAppSyncedClient_t
                                         // emit on cloud-file push
            "probelocale"  -> before     // toggles pushed ui_language
                                         // between english and spanish,
                                         // reads ACH_TEST_FIRST's
                                         // display name back through
                                         // the .so vtable — proves
                                         // per-locale fallback
            "probeshutdown" -> before    // cycles the pipe (Create→
                                         // Release) to fire
                                         // SteamShutdown_t and
                                         // SteamServersDisconnected_t
                                         // through the queue
            "probelogonfail" -> before   // reports a synthetic
                                         // EResult=84 (RateLimit) logon
                                         // failure — fires
                                         // SteamServerConnectFailure_t
                                         // through the queue
            "proberundispatch" -> before // registers a probe cb on
                                         // UserStatsReceived_t (1101),
                                         // pushes the schema, drains
                                         // via SteamAPI_RunCallbacks
                                         // — verifies dispatch chain
            "probecallresult" -> before  // pushes a synthetic CCallResult
                                         // body via push_call_result +
                                         // RegisterCallResult, drains —
                                         // verifies vtable[1] dispatch
            "probeauthticket" -> before  // calls ISteamUser.GetAuthSessionTicket
                                         // via vtable[13], verifies the
                                         // ticket bytes + GetAuthSessionTicket
                                         // Response_t emission
            "probeencticket" -> before   // calls ISteamUser.RequestEncryptedApp
                                         // Ticket (slot 21) + GetEncryptedApp
                                         // Ticket (slot 22), verifies the
                                         // synthetic 'WNETKT' body + the
                                         // EncryptedAppTicketResponse_t
                                         // landed as a CallResult
            "probeutilsapicall" -> before // pushes a synthetic CallResult,
                                          // reads it back via ISteamUtils
                                          // slot 13 — verifies the
                                          // vtable-based async-result path
                                          // mirrors the flat-C Steam_*
                                          // delegation
            "probedlc"        -> before  // pushes 3 synthetic DLC entries
                                         // for appId=440, reads back via
                                         // ISteamApps.GetDLCCount /
                                         // BGetDLCDataByIndex (slots 10/11)
            "probedepots"     -> before  // pushes 4 synthetic depot ids
                                         // for appId=440, reads back via
                                         // ISteamApps.GetInstalledDepots
                                         // (slot 17)
            "probebuildid"    -> before  // pushes a synthetic buildId,
                                         // reads back via
                                         // ISteamApps.GetAppBuildId
                                         // (slot 23)
            "probepreparelaunch" -> before  // runs the full launch-time
                                            // chokepoint (encrypted-app-
                                            // ticket + ownership-ticket +
                                            // cloud-state push) for
                                            // --ei appid <N> (default
                                            // 242760, The Forest) — used
                                            // to verify the
                                            // libsteamclient mirror is
                                            // populated for ISteamRemote
                                            // Storage queries from the
                                            // launching game
            "probelogongate" -> before  // engages the Kotlin-side rate-
                                        // limit gate with a synthetic
                                        // EResult (--ei eresult <N>,
                                        // default 84) + reports current
                                        // gate state (logonGateUntilMs,
                                        // lastLogonFailureEresult,
                                        // consecutiveLogonFailures) —
                                        // verifies the back-off escalation
                                        // chain without firing a real
                                        // CMsgClientLogon
            "probelogongateclear" -> before  // clears the rate-limit
                                             // gate (recordLogonSuccess);
                                             // diagnostic-only escape
                                             // hatch when the gate is
                                             // open and a test needs to
                                             // proceed
            else      -> {
                Log.w(TAG, "Unknown op=$op; valid: enable/disable/toggle/query/state/forceInit/seedTestStats")
                return
            }
        }

        if (op == "forceinit") {
            try {
                WnSteamBootstrap.prewarm(context.applicationContext)
                Log.i(TAG, "forceInit: prewarm dispatched (will attempt logon w/ existing token)")
            } catch (t: Throwable) {
                Log.e(TAG, "forceInit: prewarm threw", t)
            }
        } else if (op == "probebuildid") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val bs = WnSteamBootstrap
                W.setAppId(440)
                W.setAppBuildId(440, 0x12345678)
                val got = bs.appBuildId()
                Log.i(TAG, "probeBuildId: appBuildId(440)=$got " +
                    "(expect 305419896 = 0x12345678)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBuildId threw", t)
            }
        } else if (op == "probelogongate") {
            try {
                val SS = com.winlator.cmod.feature.stores.steam.service.SteamService
                val eresult = intent.getIntExtra("eresult", 84)
                val beforeUntil = SS.logonGateUntilMs
                val beforeFails = SS.consecutiveLogonFailures
                SS.recordLogonFailure(eresult)
                val afterUntil = SS.logonGateUntilMs
                val afterFails = SS.consecutiveLogonFailures
                val nowMs = System.currentTimeMillis()
                val backoffS = ((afterUntil - nowMs).coerceAtLeast(0L) / 1000L)
                Log.i(TAG, "probeLogonGate: synth EResult=$eresult — " +
                    "gate before=$beforeUntil after=$afterUntil (in ${backoffS}s) " +
                    "consecutive=$beforeFails→$afterFails")
            } catch (t: Throwable) {
                Log.e(TAG, "probeLogonGate threw", t)
            }
        } else if (op == "probelogongateclear") {
            try {
                val SS = com.winlator.cmod.feature.stores.steam.service.SteamService
                val beforeUntil = SS.logonGateUntilMs
                val beforeFails = SS.consecutiveLogonFailures
                SS.recordLogonSuccess()
                Log.i(TAG, "probeLogonGateClear: gate ${beforeUntil}→${SS.logonGateUntilMs} " +
                    "consecutive=$beforeFails→${SS.consecutiveLogonFailures}")
            } catch (t: Throwable) {
                Log.e(TAG, "probeLogonGateClear threw", t)
            }
        } else if (op == "probepreparelaunch") {
            try {
                val appId = intent.getIntExtra("appid", 242760)
                val bs = WnSteamBootstrap
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                Log.i(TAG, "probePrepareLaunch: starting chokepoint for app=$appId")
                // Off main thread — prepareLibSteamClientForLaunchBlocking
                // does network round-trips that we never want on the
                // BroadcastReceiver dispatch thread.
                Thread({
                    try {
                        com.winlator.cmod.feature.stores.steam.service.SteamService
                            .prepareLibSteamClientForLaunchBlocking(appId)
                        // Bootstrap accessors are zero when dormant; read the
                        // pushed-state mirror in parallel so the dump shows
                        // what game-side queries actually see.
                        val q = bs.cloudQuota()
                        val total = if (q.isNotEmpty()) q[0] else 0L
                        val avail = if (q.size >= 2) q[1] else 0L
                        val etk = runCatching {
                            W.nativeGetPushedEncryptedAppTicketSize(appId)
                        }.getOrNull() ?: 0
                        val pAppId    = runCatching { W.nativeGetPushedAppId() }.getOrNull() ?: 0
                        val pCloudApp = runCatching { W.nativeGetPushedCloudEnabledApp() }.getOrNull() ?: false
                        val pCFiles   = runCatching { W.nativeGetPushedCloudFileCount() }.getOrNull() ?: 0
                        Log.i(TAG, "probePrepareLaunch: app=$appId DONE — " +
                            "boundAppId=${bs.currentAppId()} " +
                            "subscribed=${bs.isSubscribedApp(appId)} " +
                            "cloudApp=${bs.cloudEnabledForApp()} " +
                            "cloudFiles=${bs.cloudFileCount()} " +
                            "cloudQuota=$total/$avail " +
                            "encTicketBytes=$etk " +
                            "(pushed: appId=$pAppId cloudApp=$pCloudApp cloudFiles=$pCFiles)")
                    } catch (t: Throwable) {
                        Log.e(TAG, "probePrepareLaunch: chokepoint threw", t)
                    }
                }, "probePrepareLaunch").start()
            } catch (t: Throwable) {
                Log.e(TAG, "probePrepareLaunch threw", t)
            }
        } else if (op == "probedepots") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val bs = WnSteamBootstrap
                W.setAppInstalledDepots(440, intArrayOf(441, 442, 443, 444))
                val got = bs.installedDepots(440)
                Log.i(TAG, "probeDepots: installedDepots(440)=${got.toList()} " +
                    "(expect [441, 442, 443, 444])")
            } catch (t: Throwable) {
                Log.e(TAG, "probeDepots threw", t)
            }
        } else if (op == "probedlc") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val bs = WnSteamBootstrap
                // 3 synthetic DLC for TF2.
                W.setAppDlcs(
                    parentAppId = 440,
                    dlcAppIds   = intArrayOf(440001, 440002, 440003),
                    dlcNames    = arrayOf("Demo Pack", "Heavy Pack", "Engineer Pack"),
                    available   = booleanArrayOf(true, true, false),
                )
                val count = bs.dlcCount(440)
                Log.i(TAG, "probeDlc: GetDLCCount(440)=$count (expect 3)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeDlc threw", t)
            }
        } else if (op == "probeutilsapicall") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val expected = 0xCAFEBABE.toInt()
                val observed = try {
                    W.nativeDiagnosticUtilsGetAPICallResult(callbackId = 1101, eresult = expected)
                } catch (_: UnsatisfiedLinkError) { 0 }
                Log.i(TAG, "probeUtilsAPICall: observed=0x" +
                    Integer.toHexString(observed) +
                    " (expect 0x" + Integer.toHexString(expected) + ")")
            } catch (t: Throwable) {
                Log.e(TAG, "probeUtilsAPICall threw", t)
            }
        } else if (op == "probeencticket") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val bs = WnSteamBootstrap
                // Bind a synthetic appId so RequestEncryptedAppTicket
                // has a non-zero app to key the cache on.
                W.setAppId(440)
                val (hCall, body) = bs.diagnosticRequestEncryptedAppTicket()
                val magic = if (body.size >= 6) String(body, 0, 6, Charsets.US_ASCII) else "?"
                val embeddedApp = if (body.size >= 20)
                    (body[16].toInt() and 0xFF) or
                    ((body[17].toInt() and 0xFF) shl 8) or
                    ((body[18].toInt() and 0xFF) shl 16) or
                    ((body[19].toInt() and 0xFF) shl 24) else -1
                Log.i(TAG, "probeEncTicket: hCall=$hCall body=${body.size}B " +
                    "magic='$magic' embeddedApp=$embeddedApp " +
                    "(expect hCall>=1, magic='WNETKT', embeddedApp=440)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeEncTicket threw", t)
            }
        } else if (op == "probeauthticket") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val buf = ByteArray(64)
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                val handle = try { W.nativeDiagnosticGetAuthTicket(buf) }
                             catch (_: UnsatisfiedLinkError) { 0 }
                val post = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Decode the magic and embedded handle to verify the
                // bytes round-tripped correctly.
                val magic = String(buf, 0, 4, Charsets.US_ASCII)
                val handleInBuf =
                    (buf[4].toInt() and 0xFF) or
                    ((buf[5].toInt() and 0xFF) shl 8) or
                    ((buf[6].toInt() and 0xFF) shl 16) or
                    ((buf[7].toInt() and 0xFF) shl 24)
                Log.i(TAG, "probeAuthTicket: handle=$handle magic='$magic' " +
                    "embeddedHandle=$handleInBuf cbDepth pre=$pre post=$post " +
                    "(expect handle>=1 magic='WNAT' embeddedHandle==handle " +
                    "post=pre+1 [GetAuthSessionTicketResponse_t emitted])")
            } catch (t: Throwable) {
                Log.e(TAG, "probeAuthTicket threw", t)
            }
        } else if (op == "probecallresult") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Synthetic eresult value to round-trip through the body.
                val expectedEresult = 0xABCD
                val packed = try {
                    W.nativeDiagnosticPushAndDrainCallResult(
                        callbackId = 1101, eresult = expectedEresult)
                } catch (_: UnsatisfiedLinkError) { 0L }
                val runs   = (packed ushr 32).toInt()
                val observed = (packed and 0xFFFFFFFFL).toInt()
                Log.i(TAG, "probeCallResult: runs=$runs observedEresult=0x" +
                    Integer.toHexString(observed) +
                    " (expect runs=1 observedEresult=0x" +
                    Integer.toHexString(expectedEresult) + ")")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCallResult threw", t)
            }
        } else if (op == "proberundispatch") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Push the schema — emits UserStatsReceived_t (1101).
                W.setAchievementSchema(
                    apiNames     = arrayOf("ACH_TEST_FIRST"),
                    displayNames = arrayOf("First Blood"),
                    descriptions = arrayOf("Score one kill."),
                    icons        = arrayOf(""),
                    hidden       = booleanArrayOf(false),
                )
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Register the probe cb + drain the queue — vtable[0]
                // fires once per matching message.
                val runs = try { W.nativeDiagnosticRegisterAndDrain(1101) }
                           catch (_: UnsatisfiedLinkError) { -1 }
                val post = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeRunDispatch: cb depth pre=$pre runs=$runs post=$post " +
                    "(expect runs>=1 — UserStatsReceived_t emitted by schema push)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeRunDispatch threw", t)
            }
        } else if (op == "probelogonfail") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.reportLogonFailure(eresult = 84, stillRetrying = true)
                val post = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeLogonFail: cb depth pre=$pre post=$post " +
                    "(expect +1 SteamServerConnectFailure_t eresult=84/RateLimit)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeLogonFail threw", t)
            }
        } else if (op == "probeshutdown") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Bring logged_on=true so the Release path emits a
                // SteamServersDisconnected_t transition (alongside
                // SteamShutdown_t emitted unconditionally).
                W.setLoggedOn(true)
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.shutdownPipe()
                val post = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeShutdown: cb depth pre=$pre post=$post " +
                    "(expect +1 SteamShutdown_t, +1 SteamServersDisconnected_t)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeShutdown threw", t)
            }
        } else if (op == "probelocale") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val bs = WnSteamBootstrap
                // Read with ui_language="english" (default seeded from PrefManager).
                fun displayName(): String? =
                    bs.listAchievementsFull()
                        .firstOrNull { it.apiName == "ACH_TEST_FIRST" }
                        ?.displayName
                W.setUiLanguage("english")
                val en = displayName()
                // Flip to spanish and re-read.
                W.setUiLanguage("spanish")
                val es = displayName()
                // ISteamApps.GetCurrentGameLanguage (slot 4) should now
                // honor pushed ui_language too (after this fire's wiring).
                val gameLangEs = bs.currentGameLanguage()
                // Restore to whatever the user has configured.
                W.setUiLanguage(PrefManager.containerLanguage.ifBlank { "english" })
                val gameLangAfter = bs.currentGameLanguage()
                Log.i(TAG, "probeLocale: ACH_TEST_FIRST en='$en' es='$es' " +
                    "ISteamApps.lang(spanish)='$gameLangEs' " +
                    "ISteamApps.lang(restored)='$gameLangAfter' " +
                    "(expect en='First Blood' es='Primera Sangre' " +
                    "ISteamApps.lang(spanish)='spanish')")
            } catch (t: Throwable) {
                Log.e(TAG, "probeLocale threw", t)
            }
        } else if (op == "seedtestcloud") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Bind a synthetic app so the RemoteStorageAppSyncedClient_t
                // callback fires (appId=0 path is suppressed — see
                // jni_pushed_state.cpp comment).
                W.setAppId(440)  // TF2's appid as a stable test value
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setCloudFiles(
                    names      = arrayOf("save0.dat", "config.cfg"),
                    sizes      = intArrayOf(1024, 256),
                    timestamps = longArrayOf(1700000000L, 1700001000L),
                )
                val post = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestCloud: pushed 2 cloud files for app 440 " +
                    "(cb depth pre=$pre post=$post — expect +1 RemoteStorageAppSyncedClient_t)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestCloud threw", t)
            }
        } else if (op == "seedtestquota") {
            try {
                // 5 GB total, 5 GB available — matches Steam Cloud's
                // default per-account allocation as of 2026. Both values
                // are picked so the round-trip math (avail = total - used)
                // is obvious in the state row (quota=5368709120/5368709120).
                val totalBytes = 5L * 1024L * 1024L * 1024L
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .setCloudQuota(totalBytes, totalBytes)
                Log.i(TAG, "seedTestQuota: pushed 5 GiB / 5 GiB available to libsteamclient.so")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestQuota threw", t)
            }
        } else if (op == "togglelogon") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val pre = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Idempotent re-set: should NOT emit (transition-only).
                W.setLoggedOn(true)
                val sameValue = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Real transition off → on. Two emits expected.
                W.setLoggedOn(false)
                val afterOff = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setLoggedOn(true)
                val afterOn = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "toggleLogon: pre=$pre sameValue=$sameValue " +
                    "afterOff=$afterOff afterOn=$afterOn " +
                    "(expect pre==sameValue, +1 disconnected, +1 connected)")
            } catch (t: Throwable) {
                Log.e(TAG, "toggleLogon threw", t)
            }
        } else if (op == "seedtestfriends") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Self persona name change — triggers
                // PersonaStateChange_t(self, kPersonaChangeName).
                W.setPersonaName("Voyager-1986#2")
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Push two synthetic friends. Names per-friend trigger
                // one PersonaStateChange_t each.
                W.setFriendsList(longArrayOf(76561197960287930L, 76561197960265728L))
                W.setFriendPersonaName(76561197960287930L, "Alice (test)")
                W.setFriendPersonaState(76561197960287930L, 1)         // Online
                W.setFriendGamePlayed(76561197960287930L, 440)         // playing TF2
                W.setFriendPersonaName(76561197960265728L, "Bob (test)")
                W.setFriendPersonaState(76561197960265728L, 0)         // Offline
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Read back via direct libsteamclient JNI accessor — the
                // bootstrap-routed accessors return 0 when bootstrap is
                // dormant (cold boot, no sign-in yet), giving a false
                // negative even when our pushed state is correctly set.
                val aliceState = try {
                    W.nativeDiagnosticGetFriendPersonaState(76561197960287930L)
                } catch (_: UnsatisfiedLinkError) { -2 }
                val bobState = try {
                    W.nativeDiagnosticGetFriendPersonaState(76561197960265728L)
                } catch (_: UnsatisfiedLinkError) { -2 }
                Log.i(TAG, "seedTestFriends: pushed self-name + 2 friend personas " +
                    "(cb depth pre=$depth0 post=$depth1) " +
                    "aliceState=$aliceState bobState=$bobState " +
                    "(expect aliceState=1 bobState=0; cb depth Δ now includes " +
                    "the ComeOnline state-transition emit for Alice)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestFriends threw", t)
            }
        } else if (op == "seedtestrichpresence") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Ensure self steam_id is non-zero so SetRichPresence
                // doesn't bail. Use a synthetic CSteamID that won't
                // collide with the user's real one (test-realm 0x1).
                val selfId = 0x110000100C123456L
                W.setSteamId(selfId)
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Self write via vtable slot 43 → emits 1
                // FriendRichPresenceUpdate_t.
                val setOk = try {
                    W.nativeDiagnosticSetRichPresence("status", "In Lobby")
                } catch (_: UnsatisfiedLinkError) { false }
                val setOk2 = try {
                    W.nativeDiagnosticSetRichPresence("connect", "+lobby:42")
                } catch (_: UnsatisfiedLinkError) { false }
                val selfCount = try {
                    W.nativeDiagnosticRichPresenceKeyCount(selfId)
                } catch (_: UnsatisfiedLinkError) { -1 }
                // Friend-side push (wn-session simulating
                // CMsgClientPersonaState RP delivery) → emits 1
                // FriendRichPresenceUpdate_t per call.
                val aliceId = 76561197960287930L
                W.setFriendRichPresence(aliceId, "status", "Boss Battle")
                W.setFriendRichPresence(aliceId, "connect", "+lobby:7")
                val aliceCount = try {
                    W.nativeDiagnosticRichPresenceKeyCount(aliceId)
                } catch (_: UnsatisfiedLinkError) { -1 }
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestRichPresence: cb depth pre=$depth0 post=$depth1 " +
                    "(expect post-pre=4: 1 self-Set*2 + 2 friend-pushes), " +
                    "setOk=$setOk setOk2=$setOk2 selfKeyCount=$selfCount " +
                    "aliceKeyCount=$aliceCount (expect selfKeyCount=2 aliceKeyCount=2)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestRichPresence threw", t)
            }
        } else if (op == "seedtestavatartiers") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val F = com.winlator.cmod.feature.stores.steam.wnsteam.AvatarFetcher
                // Use a distinct sid so we don't collide with handles
                // allocated by earlier ops this boot.
                val aliceId = 76561197960287931L
                val testHash = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                F.enqueueAllTiers(aliceId, testHash)
                // Poll up to ~10s for all three tiers to land.
                var h0 = 0; var w0 = 0; var t0 = 0
                var h1 = 0; var w1 = 0; var t1 = 0
                var h2 = 0; var w2 = 0; var t2 = 0
                repeat(100) {
                    val p0 = try { W.nativeDiagnosticGetTieredAvatarSize(aliceId, 0) } catch (_: UnsatisfiedLinkError) { 0L }
                    val p1 = try { W.nativeDiagnosticGetTieredAvatarSize(aliceId, 1) } catch (_: UnsatisfiedLinkError) { 0L }
                    val p2 = try { W.nativeDiagnosticGetTieredAvatarSize(aliceId, 2) } catch (_: UnsatisfiedLinkError) { 0L }
                    h0 = (p0 shr 32).toInt(); w0 = ((p0 shr 16) and 0xFFFF).toInt(); t0 = (p0 and 0xFFFF).toInt()
                    h1 = (p1 shr 32).toInt(); w1 = ((p1 shr 16) and 0xFFFF).toInt(); t1 = (p1 and 0xFFFF).toInt()
                    h2 = (p2 shr 32).toInt(); w2 = ((p2 shr 16) and 0xFFFF).toInt(); t2 = (p2 and 0xFFFF).toInt()
                    if (h0 > 0 && h1 > 0 && h2 > 0) return@repeat
                    Thread.sleep(100)
                }
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestAvatarTiers: " +
                    "small(handle=$h0 size=${w0}x${t0}) " +
                    "medium(handle=$h1 size=${w1}x${t1}) " +
                    "large(handle=$h2 size=${w2}x${t2}) " +
                    "cb depth pre=$depth0 post=$depth1 " +
                    "(expect small=32x32 medium=64x64 large=184x184, post-pre=3)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestAvatarTiers threw", t)
            }
        } else if (op == "seedtestschemacache") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val testAppId = 99999  // unlikely to collide with a real app
                val achievements = listOf(
                    com.winlator.cmod.feature.stores.steam.statsgen.Achievement(
                        name        = "ACH_CACHE_TEST_A",
                        displayName = mapOf("english" to "Cached A"),
                        description = mapOf("english" to "First cached achievement"),
                        hidden      = 0,
                        unlocked    = true,
                        unlockTimestamp = 1700000000,
                    ),
                    com.winlator.cmod.feature.stores.steam.statsgen.Achievement(
                        name        = "ACH_CACHE_TEST_B",
                        displayName = mapOf("english" to "Cached B"),
                        description = mapOf("english" to "Second cached achievement"),
                        hidden      = 1,
                    ),
                )
                val stats = listOf(
                    com.winlator.cmod.feature.stores.steam.statsgen.Stat(
                        id = "kills", name = "kills", type = "int", default = "0"),
                )
                // 1. Write cache via internal helper.
                SteamService.cacheAchievementSchemaJson(testAppId, achievements, stats)
                // 2. Verify the file exists.
                val cacheFile = java.io.File(
                    context.applicationContext.filesDir,
                    "wn_lsteam_schemas/$testAppId.json",
                )
                val fileLen = if (cacheFile.exists()) cacheFile.length() else -1L
                // 3. Clear in-memory schema (push empty so we can tell
                //    the warm path actually restored data).
                try {
                    W.setAchievementSchema(emptyArray(), emptyArray(), emptyArray(),
                                            emptyArray(), BooleanArray(0))
                } catch (_: UnsatisfiedLinkError) {}
                val countBefore = try { W.nativeDiagnosticAchievementCount() }
                                  catch (_: UnsatisfiedLinkError) { -1 }
                // 4. Warm from cache.
                val warmed = SteamService.warmAchievementSchemaFromCache(testAppId)
                val countAfter = try { W.nativeDiagnosticAchievementCount() }
                                 catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestSchemaCache: testAppId=$testAppId cacheFileLen=$fileLen " +
                    "warmed=$warmed achCount before=$countBefore after=$countAfter " +
                    "(expect fileLen>0 warmed=true before=0 after=2)")
                // 5. Cleanup the test cache so it doesn't pollute next runs.
                try { cacheFile.delete() } catch (_: Exception) {}
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestSchemaCache threw", t)
            }
        } else if (op == "seedtestfriendsnapshot") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val P = com.winlator.cmod.feature.stores.steam.utils.PrefManager
                val prevSnap = P.friendsSnapshotJson
                try {
                    val json = """[
                        {"sid":76561197960287930,"name":"Alice Snapshot","state":1,"app":440,
                         "avatarHash":"fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"},
                        {"sid":76561197960265728,"name":"Bob Snapshot","state":0,"app":0,
                         "avatarHash":""}
                    ]"""
                    // 1. Push via the helper (persist=true → writes to prefs).
                    val pushed = W.pushFriendPersonasJson(json, persistSnapshot = true)
                    val persistedLen = P.friendsSnapshotJson.length
                    // 2. Read back via direct libsteamclient JNI accessor
                    //    (bypasses bootstrap, which requires g_state
                    //    .initialized — bootstrap may not be alive in
                    //    this diagnostic flow).
                    val aliceState = try {
                        W.nativeDiagnosticGetFriendPersonaState(76561197960287930L)
                    } catch (_: UnsatisfiedLinkError) { -2 }
                    val bobState = try {
                        W.nativeDiagnosticGetFriendPersonaState(76561197960265728L)
                    } catch (_: UnsatisfiedLinkError) { -2 }
                    // 3. Clear in-memory state and replay from snapshot.
                    //    Same JSON same content → C++ dedup means name
                    //    won't re-emit, but state setter is non-dedup
                    //    so it always ticks the persona-change count.
                    val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                    val replayed = W.pushFriendPersonasJson(P.friendsSnapshotJson, persistSnapshot = false)
                    val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                    Log.i(TAG, "seedTestFriendSnapshot: pushed=$pushed " +
                        "persistedJsonLen=$persistedLen aliceState=$aliceState bobState=$bobState " +
                        "replayed=$replayed cb depth pre=$depth0 post=$depth1 " +
                        "(expect pushed=2 persistedJsonLen>0 aliceState=1 bobState=0 " +
                        "replayed=2 — replay reuses same map, dedup-aware setters)")
                } finally {
                    P.friendsSnapshotJson = prevSnap
                }
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestFriendSnapshot threw", t)
            }
        } else if (op == "seedtestprefswarmup") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val P = com.winlator.cmod.feature.stores.steam.utils.PrefManager
                // Snapshot existing values so we don't trash a real
                // signed-in user's prefs.
                val prevSid    = P.steamUserSteamId64
                val prevName   = P.steamUserName
                val prevHash   = P.steamUserAvatarHash
                try {
                    val testSid  = 0x110000100DEADBE0L
                    val testName = "WarmupTester"
                    val testHash = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"
                    P.steamUserSteamId64   = testSid
                    P.steamUserName        = testName
                    P.steamUserAvatarHash  = testHash
                    val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                    W.seedFromPrefManager(context.applicationContext)
                    // Poll for the avatar handle to land via the
                    // background CDN fetch.
                    var handle = 0
                    var w = 0
                    var h = 0
                    repeat(60) {
                        val packed = try {
                            W.nativeDiagnosticGetSmallAvatarSize(testSid)
                        } catch (_: UnsatisfiedLinkError) { 0L }
                        handle = (packed shr 32).toInt()
                        if (handle > 0) {
                            w = ((packed shr 16) and 0xFFFF).toInt()
                            h = (packed and 0xFFFF).toInt()
                            return@repeat
                        }
                        Thread.sleep(100)
                    }
                    val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                    val storedHashHex = try {
                        W.nativeDiagnosticGetFriendAvatarHashHex(testSid)
                    } catch (_: UnsatisfiedLinkError) { "?" }
                    Log.i(TAG, "seedTestPrefsWarmup: sid=$testSid handle=$handle " +
                        "size=${w}x${h} storedHash='$storedHashHex' " +
                        "cb depth pre=$depth0 post=$depth1 " +
                        "(expect handle>0 size=32x32 storedHash=$testHash " +
                        "post-pre≥2 — 1 PersonaStateChange + 1 AvatarImageLoaded)")
                } finally {
                    P.steamUserSteamId64  = prevSid
                    P.steamUserName       = prevName
                    P.steamUserAvatarHash = prevHash
                }
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestPrefsWarmup threw", t)
            }
        } else if (op == "seedtestselfavatar") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val F = com.winlator.cmod.feature.stores.steam.wnsteam.AvatarFetcher
                // Set a synthetic self steamId — the same value the
                // seedtestrichpresence op uses, so it persists between
                // ops in a single boot.
                val selfId = 0x110000100C123456L
                W.setSteamId(selfId)
                val testHash = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Mirror the exact wn-session-side sequence:
                //   setFriendAvatarHash(self_sid, bytes) → PersonaStateChange_t
                //   AvatarFetcher.enqueue(self_sid, hex, 0)  → AvatarImageLoaded_t
                val bytes = ByteArray(20)
                for (k in bytes.indices) {
                    val hi = Character.digit(testHash[k * 2], 16)
                    val lo = Character.digit(testHash[k * 2 + 1], 16)
                    bytes[k] = ((hi shl 4) or lo).toByte()
                }
                W.setFriendAvatarHash(selfId, bytes)
                F.enqueue(selfId, testHash, tier = 0)
                var handle = 0
                var w = 0
                var h = 0
                repeat(60) {
                    val packed = try {
                        W.nativeDiagnosticGetSmallAvatarSize(selfId)
                    } catch (_: UnsatisfiedLinkError) { 0L }
                    handle = (packed shr 32).toInt()
                    if (handle > 0) {
                        w = ((packed shr 16) and 0xFFFF).toInt()
                        h = (packed and 0xFFFF).toInt()
                        return@repeat
                    }
                    Thread.sleep(100)
                }
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestSelfAvatar: selfId=$selfId handle=$handle " +
                    "size=${w}x${h} cb depth pre=$depth0 post=$depth1 " +
                    "(expect handle>0, size=32x32, post-pre=2 — 1 PersonaStateChange + " +
                    "1 AvatarImageLoaded — proves self uses same friend_avatars map)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestSelfAvatar threw", t)
            }
        } else if (op == "seedtestfetchavatar") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val F = com.winlator.cmod.feature.stores.steam.wnsteam.AvatarFetcher
                val aliceId = 76561197960287930L
                // Steam's well-known "default unknown user" avatar hash —
                // stable across years, served by every Steam CDN edge.
                // The image is a generic silhouette; perfect for a
                // network-path verification that doesn't depend on a
                // specific user's avatar staying unchanged.
                val testHash = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                F.enqueue(aliceId, testHash, tier = 0)
                // Poll for the push to land — fetch + decode runs on
                // IO dispatcher off this broadcast thread. Cap at ~6s.
                var handle = 0
                var w = 0
                var h = 0
                repeat(60) {
                    val packed = try {
                        W.nativeDiagnosticGetSmallAvatarSize(aliceId)
                    } catch (_: UnsatisfiedLinkError) { 0L }
                    handle = (packed shr 32).toInt()
                    if (handle > 0) {
                        w = ((packed shr 16) and 0xFFFF).toInt()
                        h = (packed and 0xFFFF).toInt()
                        return@repeat
                    }
                    Thread.sleep(100)
                }
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestFetchAvatar: hash=$testHash handle=$handle " +
                    "size=${w}x${h} cb depth pre=$depth0 post=$depth1 " +
                    "(expect handle>0, size=32x32 or similar, " +
                    "post-pre=1 AvatarImageLoaded — assumes network reachable)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestFetchAvatar threw", t)
            }
        } else if (op == "seedtestavatarhash") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val aliceId = 76561197960287930L
                // Synthesize a 20-byte SHA-1-shaped hash (gradient).
                val hashA = ByteArray(20) { (it * 13 + 7).toByte() }
                val hashB = ByteArray(20) { (it * 17 + 3).toByte() }
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setFriendAvatarHash(aliceId, hashA)        // change → +1
                W.setFriendAvatarHash(aliceId, hashA)        // dedup → +0
                val depthMid = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setFriendAvatarHash(aliceId, hashB)        // change → +1
                W.setFriendAvatarHash(aliceId, null)         // clear → +1
                W.setFriendAvatarHash(aliceId, null)         // dedup → +0
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Final stored value should be empty after the clear.
                val finalHex = try {
                    W.nativeDiagnosticGetFriendAvatarHashHex(aliceId)
                } catch (_: UnsatisfiedLinkError) { "?" }
                Log.i(TAG, "seedTestAvatarHash: cb depth pre=$depth0 mid=$depthMid post=$depth1 " +
                    "finalHex='$finalHex' " +
                    "(expect mid-pre=1 post-mid=2 finalHex='' — 3 transitions, 2 dedups)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestAvatarHash threw", t)
            }
        } else if (op == "seedtestavatar") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val aliceId = 76561197960287930L
                // 4×4 RGBA8 = 64 bytes. Fill with a recognizable
                // gradient (R=x*64, G=y*64, B=0xAA, A=0xFF) so the
                // round-trip can verify byte-exact transport.
                val w = 4
                val h = 4
                val rgba = ByteArray(w * h * 4)
                for (y in 0 until h) for (x in 0 until w) {
                    val i = (y * w + x) * 4
                    rgba[i + 0] = (x * 64).toByte()
                    rgba[i + 1] = (y * 64).toByte()
                    rgba[i + 2] = 0xAA.toByte()
                    rgba[i + 3] = 0xFF.toByte()
                }
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                val handle = try {
                    W.nativePushFriendAvatar(aliceId, tier = 0, width = w, height = h, rgba = rgba)
                } catch (_: UnsatisfiedLinkError) { 0 }
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                val packed = try {
                    W.nativeDiagnosticGetSmallAvatarSize(aliceId)
                } catch (_: UnsatisfiedLinkError) { 0L }
                val rdHandle = (packed shr 32).toInt()
                val rdW = ((packed shr 16) and 0xFFFF).toInt()
                val rdH = (packed and 0xFFFF).toInt()
                val readBuf = ByteArray(w * h * 4)
                val copied = try {
                    W.nativeDiagnosticGetImageRGBA(handle, readBuf)
                } catch (_: UnsatisfiedLinkError) { 0 }
                // Spot-check pixel (2, 1): R=128 G=64 B=0xAA A=0xFF
                val px2_1 = (2 + 1 * w) * 4
                val r = readBuf[px2_1 + 0].toInt() and 0xFF
                val g = readBuf[px2_1 + 1].toInt() and 0xFF
                val b = readBuf[px2_1 + 2].toInt() and 0xFF
                val a = readBuf[px2_1 + 3].toInt() and 0xFF
                Log.i(TAG, "seedTestAvatar: handle=$handle (cb depth pre=$depth0 post=$depth1) " +
                    "rdHandle=$rdHandle rdW=$rdW rdH=$rdH copied=$copied " +
                    "px(2,1) RGBA=($r,$g,$b,$a) " +
                    "(expect post-pre=1 AvatarImageLoaded, rdW=4 rdH=4, RGBA=(128,64,170,255))")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestAvatar threw", t)
            }
        } else if (op == "probebridgepersonaself") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val selfSid = 0x110000100C123498L
                W.setSteamId(selfSid)
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                try {
                    W.nativeDiagnosticInjectPersonaEvent(
                        steamId      = selfSid,         // matches setSteamId → observer takes self path
                        personaState = 3,                // Away
                        gameAppId    = 730,
                        name         = "SelfPathProbe",
                        avatarHash   = null,
                        rpKeys       = null,
                        rpValues     = null,
                    )
                } catch (_: UnsatisfiedLinkError) {}
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Read self values via bootstrap (or fall back: query
                // persona_name from cache directly through state op).
                // For verification we don't have a direct getter for
                // pushed.persona_name; rely on cb depth delta + check
                // that the observer's friend_persona_states[selfSid]
                // entry is NOT created (self path doesn't touch the
                // friend map).
                val friendStateAtSelfSid = try {
                    W.nativeDiagnosticGetFriendPersonaState(selfSid)
                } catch (_: UnsatisfiedLinkError) { -2 }
                Log.i(TAG, "probeBridgePersonaSelf: cb depth pre=$depth0 post=$depth1 " +
                    "friendStateAtSelfSid=$friendStateAtSelfSid " +
                    "(expect cb Δ=+1 self PSC; friendStateAtSelfSid=-1 " +
                    "[self path writes pushed.persona_state, NOT the friend map])")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgePersonaSelf threw", t)
            }
        } else if (op == "probebridgepersona") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Fresh sid so we don't collide with prior op state.
                val sid = 76561197960287936L
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Full payload: name + state + game + 20-byte avatar hash + 2 RP keys.
                // First-observation: all flags emit, RP callback fires.
                val hash = ByteArray(20) { (it * 11 + 5).toByte() }
                try {
                    W.nativeDiagnosticInjectPersonaEvent(
                        steamId      = sid,
                        personaState = 1,
                        gameAppId    = 440,
                        name         = "PersonaProbe",
                        avatarHash   = hash,
                        rpKeys       = arrayOf("status", "connect"),
                        rpValues     = arrayOf("In Lobby", "+lobby:42"),
                    )
                } catch (_: UnsatisfiedLinkError) {}
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Read back state via direct accessor.
                val st = try { W.nativeDiagnosticGetFriendPersonaState(sid) } catch (_: UnsatisfiedLinkError) { -2 }
                val hashHex = try { W.nativeDiagnosticGetFriendAvatarHashHex(sid) } catch (_: UnsatisfiedLinkError) { "?" }
                val rpCount = try { W.nativeDiagnosticRichPresenceKeyCount(sid) } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeBridgePersona: cb depth pre=$depth0 post=$depth1 " +
                    "state=$st hashLen=${hashHex.length / 2} rpCount=$rpCount " +
                    "(expect cb Δ=+2 PSC[name|state|game|avatar|come-online] + " +
                    "FriendRichPresenceUpdate_t; state=1, hashLen=20, rpCount=2)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgePersona threw", t)
            }
        } else if (op == "probenicksig") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val sid = 0x0110000100006666L
                val nickBefore = W.nativeDiagnosticGetPlayerNickname(sid)
                W.nativeSetPlayerNickname(sid, "Sniper42")
                val nickAfter = W.nativeDiagnosticGetPlayerNickname(sid)
                W.nativeSetPlayerNickname(sid, null)
                val nickCleared = W.nativeDiagnosticGetPlayerNickname(sid)
                val hSig1 = W.nativeDiagnosticCheckFileSignature("bin/game.exe")
                val hSig2 = W.nativeDiagnosticCheckFileSignature(null)
                Log.i(TAG, "probeNickSig: nickBefore=$nickBefore (null), " +
                    "nickAfter=$nickAfter (Sniper42), nickCleared=$nickCleared (null), " +
                    "hSig1=$hSig1 hSig2=$hSig2 (both non-zero+distinct: " +
                    "${hSig1 != 0L && hSig2 != 0L && hSig1 != hSig2})")
            } catch (t: Throwable) {
                Log.e(TAG, "probeNickSig threw", t)
            }
        } else if (op == "probecloudshare") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9900
                val dir = java.io.File(context.cacheDir, "probe-cloud-share-$appId")
                dir.mkdirs()
                W.nativeSetAppCloudRemoteDir(appId, dir.absolutePath)
                W.setAppId(appId)
                W.nativeDiagnosticCloudFileWrite("share.dat", "shareable".toByteArray())
                // FileShare twice on same file → same call handles, but
                // the underlying UGCHandle returned via cb is content-
                // stable (we can't easily verify the latter here without
                // a cb pump, but distinct call handles is fine).
                val hShare1 = W.nativeDiagnosticCloudFileShare("share.dat")
                val hShare2 = W.nativeDiagnosticCloudFileShare("share.dat")
                // FileShare missing → still gets hCall (cb resolves with
                // FileNotFound).
                val hShareMiss = W.nativeDiagnosticCloudFileShare("missing.dat")
                // GetFileDetails is in ISteamApps — needs setAppInstallDir
                // to resolve a base. Stash a real file there so the
                // happy-path can hash actual bytes.
                W.setAppInstallDir(appId, dir.absolutePath)
                java.io.File(dir, "binary.dat").writeBytes("xyz123".toByteArray())
                val hDetails = W.nativeDiagnosticAppsGetFileDetails("binary.dat")
                val hDetailsMiss = W.nativeDiagnosticAppsGetFileDetails("nope.dat")
                val hDetailsTraversal = W.nativeDiagnosticAppsGetFileDetails("../escape")
                W.nativeDiagnosticCloudFileDelete("share.dat")
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                Log.i(TAG, "probeCloudShare: " +
                    "share1=$hShare1 share2=$hShare2 (distinct hCalls), " +
                    "shareMissing=$hShareMiss, " +
                    "details=$hDetails detailsMissing=$hDetailsMiss " +
                    "detailsTraversal=$hDetailsTraversal " +
                    "(all non-zero: ${hShare1 != 0L && hShare2 != 0L && hShareMiss != 0L && hDetails != 0L && hDetailsMiss != 0L && hDetailsTraversal != 0L})")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCloudShare threw", t)
            }
        } else if (op == "probeplayerlevel") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 6500
                W.setAppId(appId)
                W.nativeSetSelfPlayerLevel(73)
                val plr = W.nativeDiagnosticGetPlayerSteamLevel()
                W.nativeSetSelfGameBadge(appId, 1, false, 5)
                W.nativeSetSelfGameBadge(appId, 1, true,  3)
                val badgeBasic = W.nativeDiagnosticGetGameBadgeLevel(1, false)
                val badgeFoil  = W.nativeDiagnosticGetGameBadgeLevel(1, true)
                val badgeOther = W.nativeDiagnosticGetGameBadgeLevel(2, false)
                val hStore   = W.nativeDiagnosticRequestStoreAuthURL("checkout/")
                val hMarket  = W.nativeDiagnosticGetMarketEligibility()
                val hDuration = W.nativeDiagnosticGetDurationControl()
                W.setAppId(0)
                W.nativeSetSelfPlayerLevel(-1)
                W.nativeSetSelfGameBadge(appId, 1, false, -1)
                W.nativeSetSelfGameBadge(appId, 1, true,  -1)
                Log.i(TAG, "probePlayerLevel: " +
                    "playerLevel=$plr (expect 73), " +
                    "badgeBasic=$badgeBasic (5), badgeFoil=$badgeFoil (3), " +
                    "badgeOther=$badgeOther (0), " +
                    "hStoreUrl=$hStore hMarket=$hMarket hDuration=$hDuration " +
                    "(all non-zero+distinct: ${hStore != 0L && hMarket != 0L && hDuration != 0L && hStore != hMarket && hMarket != hDuration})")
            } catch (t: Throwable) {
                Log.e(TAG, "probePlayerLevel threw", t)
            }
        } else if (op == "probelicense") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val selfSid = 0x0110000100009999L
                val otherSid = 0x0110000100008888L
                val ownedApp = 7700
                val unownedApp = 7800
                W.setSteamId(selfSid)
                W.setOwnedApps(intArrayOf(ownedApp))
                val r1 = W.nativeDiagnosticUserHasLicense(selfSid, ownedApp)
                val r2 = W.nativeDiagnosticUserHasLicense(selfSid, unownedApp)
                val r3 = W.nativeDiagnosticUserHasLicense(otherSid, ownedApp)
                val r4 = W.nativeDiagnosticUserHasLicense(0L, ownedApp)
                val r5 = W.nativeDiagnosticUserHasLicense(selfSid, 0)
                W.setOwnedApps(IntArray(0))
                W.setSteamId(0)
                Log.i(TAG, "probeLicense: self+owned=$r1 (expect 0/HasLicense), " +
                    "self+unowned=$r2 (expect 1/DoesNotHave), " +
                    "other+owned=$r3 (expect 2/NoAuth), " +
                    "nullSid+owned=$r4 (expect 2/NoAuth), " +
                    "self+nullApp=$r5 (expect 2/NoAuth)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeLicense threw", t)
            }
        } else if (op == "probecorrupt") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 7900
                W.setAppId(appId)
                val markedBefore = W.nativeIsAppMarkedCorrupt(appId)
                val markOk = W.nativeDiagnosticMarkContentCorrupt(false)
                val markedAfter = W.nativeIsAppMarkedCorrupt(appId)
                W.nativeClearAppCorruptFlag(appId)
                val markedCleared = W.nativeIsAppMarkedCorrupt(appId)
                W.setAppId(0)
                val markNoApp = W.nativeDiagnosticMarkContentCorrupt(true)
                Log.i(TAG, "probeCorrupt: before=$markedBefore (false), " +
                    "mark=$markOk (true), after=$markedAfter (true), " +
                    "cleared=$markedCleared (false), markNoApp=$markNoApp (false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCorrupt threw", t)
            }
        } else if (op == "probefriendlevel") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val sid = 0x0110000100007777L
                val before = W.nativeDiagnosticGetFriendSteamLevel(sid)
                W.nativeSetFriendSteamLevel(sid, 42)
                val after = W.nativeDiagnosticGetFriendSteamLevel(sid)
                W.nativeSetFriendSteamLevel(sid, -1)
                val cleared = W.nativeDiagnosticGetFriendSteamLevel(sid)
                Log.i(TAG, "probeFriendLevel: before=$before (0), " +
                    "after=$after (42), cleared=$cleared (0)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeFriendLevel threw", t)
            }
        } else if (op == "probewebapiticket") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val h1 = W.nativeDiagnosticGetAuthTicketForWebApi("myWebApi")
                val h2 = W.nativeDiagnosticGetAuthTicketForWebApi(null)
                val h3 = W.nativeDiagnosticGetAuthTicketForWebApi("anotherDest")
                Log.i(TAG, "probeWebApiTicket: h1=$h1 h2=$h2 h3=$h3 " +
                    "(all non-zero, distinct: ${h1 != 0L && h2 != 0L && h3 != 0L && h1 != h2 && h2 != h3})")
            } catch (t: Throwable) {
                Log.e(TAG, "probeWebApiTicket threw", t)
            }
        } else if (op == "probefriendrel") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val friend1 = 0x0110000100000001L
                val friend2 = 0x0110000100000002L
                val stranger = 0x0110000100000099L
                W.setFriendsList(longArrayOf(friend1, friend2))
                val rel1   = W.nativeDiagnosticGetFriendRelationship(friend1)
                val relStr = W.nativeDiagnosticGetFriendRelationship(stranger)
                val rel0   = W.nativeDiagnosticGetFriendRelationship(0L)
                val hasImmediate = W.nativeDiagnosticHasFriend(friend2, 0x10)
                val hasAll       = W.nativeDiagnosticHasFriend(friend2, 0xFFFF)
                val hasClanOnly  = W.nativeDiagnosticHasFriend(friend2, 0x20)
                val hasStranger  = W.nativeDiagnosticHasFriend(stranger, 0xFFFF)
                W.setFriendsList(LongArray(0))
                Log.i(TAG, "probeFriendRel: friend=$rel1 (expect 3), " +
                    "stranger=$relStr (expect 0), null=$rel0 (expect 0), " +
                    "hasImmediate=$hasImmediate (true), hasAll=$hasAll (true), " +
                    "hasClanOnly=$hasClanOnly (false), hasStranger=$hasStranger (false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeFriendRel threw", t)
            }
        } else if (op == "probeuserdata") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9800
                val remoteDir = "/data/cache/.../userdata/123/$appId/remote"
                W.nativeSetAppCloudRemoteDir(appId, remoteDir)
                W.setAppId(appId)
                val resolved = W.nativeDiagnosticGetUserDataFolder()
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                val expected = "/data/cache/.../userdata/123/$appId"
                Log.i(TAG, "probeUserData: resolved=\"$resolved\" expected=\"$expected\" " +
                    "match=${resolved == expected}")
            } catch (t: Throwable) {
                Log.e(TAG, "probeUserData threw", t)
            }
        } else if (op == "probeaccountinfo") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Clear, inject mixed, verify each slot reflects.
                W.nativeDiagnosticInjectAccountInfo(false, false, false, false)
                val cleared26 = W.nativeDiagnosticUserBool(26)
                val cleared27 = W.nativeDiagnosticUserBool(27)
                W.nativeDiagnosticInjectAccountInfo(true, false, true, false)
                val v26 = W.nativeDiagnosticUserBool(26)  // phone-verified=false
                val v27 = W.nativeDiagnosticUserBool(27)  // 2FA=true
                val v28 = W.nativeDiagnosticUserBool(28)  // phone-identifying=true
                val v29 = W.nativeDiagnosticUserBool(29)  // phone-needs-verify=false
                Log.i(TAG, "probeAccountInfo: cleared(26=$cleared26, 27=$cleared27) " +
                    "after-inject(26=$v26 expect false, 27=$v27 expect true, " +
                    "28=$v28 expect true, 29=$v29 expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeAccountInfo threw", t)
            }
        } else if (op == "probeuserbool") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Set all 4 account flags then verify each.
                for (k in 0..3) W.nativeSetAccountFlag(k, true)
                val v26 = W.nativeDiagnosticUserBool(26)  // phone-verified
                val v27 = W.nativeDiagnosticUserBool(27)  // 2FA
                val v28 = W.nativeDiagnosticUserBool(28)  // phone-identifying
                val v29 = W.nativeDiagnosticUserBool(29)  // phone-needs-verify
                // Clear all → all false.
                for (k in 0..3) W.nativeSetAccountFlag(k, false)
                val o26 = W.nativeDiagnosticUserBool(26)
                val o27 = W.nativeDiagnosticUserBool(27)
                val o28 = W.nativeDiagnosticUserBool(28)
                val o29 = W.nativeDiagnosticUserBool(29)
                val dur = W.nativeDiagnosticSetDurationControl(1)
                Log.i(TAG, "probeUserBool: " +
                    "set→phoneV=$v26 2FA=$v27 phoneIdent=$v28 phoneNeedsV=$v29 (all true); " +
                    "cleared→phoneV=$o26 2FA=$o27 phoneIdent=$o28 phoneNeedsV=$o29 (all false); " +
                    "durationControl=$dur (always true)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeUserBool threw", t)
            }
        } else if (op == "probeappsbool") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9600
                W.setOwnedApps(intArrayOf(appId))
                W.nativeSetAppFlag(0, appId, true)   // low-violence
                W.nativeSetAppFlag(1, appId, true)   // vac-banned
                W.setAppId(appId)
                val sub  = W.nativeDiagnosticAppsBool(0)
                val lowV = W.nativeDiagnosticAppsBool(1)
                val cyber = W.nativeDiagnosticAppsBool(2)
                val vac  = W.nativeDiagnosticAppsBool(3)
                // Clear flags + un-own → all false.
                W.setOwnedApps(IntArray(0))
                W.nativeSetAppFlag(0, appId, false)
                W.nativeSetAppFlag(1, appId, false)
                val subOff  = W.nativeDiagnosticAppsBool(0)
                val lowVOff = W.nativeDiagnosticAppsBool(1)
                val vacOff  = W.nativeDiagnosticAppsBool(3)
                val ctxOk = W.nativeDiagnosticSetDlcContext(appId)
                W.setAppId(0)
                Log.i(TAG, "probeAppsBool: " +
                    "subscribed=$sub (true), lowV=$lowV (true), " +
                    "cyber=$cyber (always false), vac=$vac (true); " +
                    "cleared → subOff=$subOff lowVOff=$lowVOff vacOff=$vacOff (all false), " +
                    "setDlcContext=$ctxOk (true)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeAppsBool threw", t)
            }
        } else if (op == "probefilestate") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9700
                val dir = java.io.File(context.cacheDir, "probe-filestate-$appId")
                dir.mkdirs()
                W.nativeSetAppCloudRemoteDir(appId, dir.absolutePath)
                W.setAppId(appId)
                W.nativeDiagnosticCloudFileWrite("note.dat", "x".toByteArray())
                val persistedBefore = W.nativeDiagnosticCloudFilePersisted("note.dat")
                val forgotten = W.nativeDiagnosticCloudFileForget("note.dat")
                val persistedAfter = W.nativeDiagnosticCloudFilePersisted("note.dat")
                val forgottenAgain = W.nativeDiagnosticCloudFileForget("note.dat")
                // Disk should still have the bytes.
                val onDisk = java.io.File(dir, "note.dat").exists()
                W.nativeDiagnosticCloudFileDelete("note.dat")
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                Log.i(TAG, "probeFileState: persistedBefore=$persistedBefore (true), " +
                    "forgotten=$forgotten (true), persistedAfter=$persistedAfter (false), " +
                    "forgottenAgain=$forgottenAgain (false), diskKept=$onDisk (true)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeFileState threw", t)
            }
        } else if (op == "probecloudstream") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9500
                val dir = java.io.File(context.cacheDir, "probe-cloud-stream-$appId")
                dir.mkdirs()
                W.nativeSetAppCloudRemoteDir(appId, dir.absolutePath)
                W.setAppId(appId)
                // Commit path: open → 3 chunks → close → read-back.
                val h1 = W.nativeDiagnosticCloudStreamOpen("stream.dat")
                val c1 = W.nativeDiagnosticCloudStreamWriteChunk(h1, "AAA".toByteArray())
                val c2 = W.nativeDiagnosticCloudStreamWriteChunk(h1, "BBBB".toByteArray())
                val c3 = W.nativeDiagnosticCloudStreamWriteChunk(h1, "CC".toByteArray())
                val closed = W.nativeDiagnosticCloudStreamClose(h1)
                val readBack = W.nativeDiagnosticCloudFileRead("stream.dat", 64)
                val match = readBack != null && readBack.contentEquals("AAABBBBCC".toByteArray())
                // Cancel path: open → write → cancel → read-back must miss.
                val h2 = W.nativeDiagnosticCloudStreamOpen("cancelled.dat")
                W.nativeDiagnosticCloudStreamWriteChunk(h2, "ZZZ".toByteArray())
                val cancelled = W.nativeDiagnosticCloudStreamCancel(h2)
                val missing = W.nativeDiagnosticCloudFileRead("cancelled.dat", 16)
                // Unknown handle: bogus close/cancel must return false.
                val bogusClose  = W.nativeDiagnosticCloudStreamClose(0x999999L)
                val bogusCancel = W.nativeDiagnosticCloudStreamCancel(0x999999L)
                W.nativeDiagnosticCloudFileDelete("stream.dat")
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                Log.i(TAG, "probeCloudStream: open1=$h1 c1=$c1 c2=$c2 c3=$c3 " +
                    "close=$closed match=$match (expect AAABBBBCC), " +
                    "cancel=$cancelled miss-after-cancel=${missing == null}, " +
                    "bogusClose=$bogusClose bogusCancel=$bogusCancel (both false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCloudStream threw", t)
            }
        } else if (op == "probecloudasync") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9400
                val dir = java.io.File(context.cacheDir, "probe-cloud-async-$appId")
                dir.mkdirs()
                W.nativeSetAppCloudRemoteDir(appId, dir.absolutePath)
                W.setAppId(appId)
                val payload = "async-roundtrip-${System.currentTimeMillis()}".toByteArray()
                val wH = W.nativeDiagnosticCloudFileWriteAsync("async.dat", payload)
                val rH = W.nativeDiagnosticCloudFileReadAsync("async.dat", 0, payload.size)
                val bytes = W.nativeDiagnosticCloudFileReadAsyncComplete(rH, payload.size)
                val match = bytes != null && bytes.contentEquals(payload)
                // Single-shot semantics: 2nd complete on same hCall must
                // return null.
                val bytes2 = W.nativeDiagnosticCloudFileReadAsyncComplete(rH, payload.size)
                W.nativeDiagnosticCloudFileDelete("async.dat")
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                Log.i(TAG, "probeCloudAsync: writeAsync-hCall=$wH (expect non-zero), " +
                    "readAsync-hCall=$rH (expect non-zero), " +
                    "roundtrip-match=$match (expect true), " +
                    "second-complete-null=${bytes2 == null} (expect true)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCloudAsync threw", t)
            }
        } else if (op == "probecloudio") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9300
                val dir = java.io.File(context.cacheDir, "probe-cloud-$appId")
                dir.mkdirs()
                W.nativeSetAppCloudRemoteDir(appId, dir.absolutePath)
                W.setAppId(appId)
                val payload = "hello-steam-cloud-${System.currentTimeMillis()}".toByteArray()
                val wOk  = W.nativeDiagnosticCloudFileWrite("save.dat", payload)
                val read = W.nativeDiagnosticCloudFileRead("save.dat", 256)
                val match = read != null && read.contentEquals(payload)
                // Path-traversal rejection: must return false / null.
                val rejectW = W.nativeDiagnosticCloudFileWrite("../escape", byteArrayOf(1))
                val rejectR = W.nativeDiagnosticCloudFileRead("../escape", 8)
                val dOk = W.nativeDiagnosticCloudFileDelete("save.dat")
                val readAfter = W.nativeDiagnosticCloudFileRead("save.dat", 256)
                W.setAppId(0)
                W.nativeSetAppCloudRemoteDir(appId, null)
                Log.i(TAG, "probeCloudIO: write=$wOk (expect true), " +
                    "read-roundtrip=$match (expect true), " +
                    "rejectTraversalW=$rejectW (expect false), " +
                    "rejectTraversalR=${rejectR != null} (expect false), " +
                    "delete=$dOk (expect true), " +
                    "read-after-delete=${readAfter != null} (expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeCloudIO threw", t)
            }
        } else if (op == "probedlprogress") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9200
                W.nativeSetAppDownloadProgress(appId, 1234L, 5000L)
                val active = W.nativeDiagnosticGetDlcDownloadProgress(appId)
                val dl = W.nativeDiagnosticGetDlcDownloadProgressBytes()
                val tot = W.nativeDiagnosticGetDlcDownloadProgressTotal()
                W.nativeSetAppDownloadProgress(appId, 0L, 0L)
                val cleared = W.nativeDiagnosticGetDlcDownloadProgress(appId)
                val unknown = W.nativeDiagnosticGetDlcDownloadProgress(99999)
                Log.i(TAG, "probeDlProgress: active=$active (expect true) " +
                    "dl=$dl (expect 1234) total=$tot (expect 5000), " +
                    "cleared=$cleared (expect false), " +
                    "unknown-app=$unknown (expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeDlProgress threw", t)
            }
        } else if (op == "probebetaname") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val appId = 9100
                W.nativeSetAppCurrentBeta(appId, "experimental_branch")
                W.setAppId(appId)
                val name = W.nativeDiagnosticGetCurrentBetaName()
                W.nativeSetAppCurrentBeta(appId, null)
                val cleared = W.nativeDiagnosticGetCurrentBetaName()
                W.setAppId(0)
                Log.i(TAG, "probeBetaName: bound→\"$name\" (expect experimental_branch), " +
                    "cleared→\"$cleared\" (expect null)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBetaName threw", t)
            }
        } else if (op == "probedlcinstalled") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Seed parent + DLC. DLC1 is owned + installed directly
                // (standalone-DLC shape). DLC2 is owned but installed only
                // via the parent (piggyback shape). DLC3 is owned but the
                // parent isn't installed. DLC4 is unowned.
                val parent = 8100
                val dlc1 = 8101; val dlc2 = 8102; val dlc3 = 8103; val dlc4 = 8104
                W.setOwnedApps(intArrayOf(parent, dlc1, dlc2, dlc3))
                W.setInstalledApps(intArrayOf(parent, dlc1))
                W.setAppDlcs(
                    parent,
                    intArrayOf(dlc1, dlc2, dlc3, dlc4),
                    arrayOf("dlc-one", "dlc-two", "dlc-three", "dlc-four"),
                    booleanArrayOf(true, true, true, true))
                val r1 = W.nativeDiagnosticBIsDlcInstalled(dlc1)
                val r2 = W.nativeDiagnosticBIsDlcInstalled(dlc2)
                val r3 = W.nativeDiagnosticBIsDlcInstalled(dlc3)
                val r4 = W.nativeDiagnosticBIsDlcInstalled(dlc4)
                Log.i(TAG, "probeDlcInstalled: " +
                    "dlc1 owned+self-installed=$r1 (expect true), " +
                    "dlc2 owned+parent-installed=$r2 (expect true), " +
                    "dlc3 owned+parent-installed=$r3 (expect true, parent in set), " +
                    "dlc4 unowned=$r4 (expect false)")
                // Re-evaluate r3 with parent removed:
                W.setInstalledApps(intArrayOf(dlc1))
                val r3b = W.nativeDiagnosticBIsDlcInstalled(dlc3)
                Log.i(TAG, "probeDlcInstalled: dlc3 with parent removed=$r3b (expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeDlcInstalled threw", t)
            }
        } else if (op == "probetimedtrial") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Inject trial license: 90 minutes allowed, 12 used.
                W.nativeDiagnosticInjectTrialLicense(5001, 90, 12)
                W.nativeSetAppSourcePackages(5500, intArrayOf(5001))
                W.setAppId(5500)
                val packed = try { W.nativeDiagnosticBIsTimedTrial() }
                             catch (_: UnsatisfiedLinkError) { 0L }
                val isTrial = packed != 0L
                val allowed = ((packed shr 32) and 0x7FFFFFFFL).toInt()
                val played  = (packed and 0xFFFFFFFFL).toInt()
                W.setAppId(0)
                // Non-trial control: package with no minute_limit → should
                // return false.
                W.nativeDiagnosticInjectTrialLicense(5002, 0, 0)
                W.nativeSetAppSourcePackages(5600, intArrayOf(5002))
                W.setAppId(5600)
                val nonTrialPacked = try { W.nativeDiagnosticBIsTimedTrial() }
                                     catch (_: UnsatisfiedLinkError) { 0L }
                val isNonTrial = nonTrialPacked != 0L
                W.setAppId(0)
                Log.i(TAG, "probeTimedTrial: trial app: isTrial=$isTrial " +
                    "allowed=$allowed seconds (expect 5400, =90*60), " +
                    "played=$played seconds (expect 720, =12*60); " +
                    "non-trial app: isTrial=$isNonTrial (expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeTimedTrial threw", t)
            }
        } else if (op == "probeavgrate") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Session 1: 30 kills over 60s session → 0.5 kills/sec
                val r1 = try { W.nativeDiagnosticUpdateAvgRateStat("kph_test", 30.0f, 60.0) }
                         catch (_: UnsatisfiedLinkError) { 0.0f }
                // Session 2: 60 kills over 60s session → now (90 / 120) = 0.75
                val r2 = try { W.nativeDiagnosticUpdateAvgRateStat("kph_test", 60.0f, 60.0) }
                         catch (_: UnsatisfiedLinkError) { 0.0f }
                // Session 3: 30 kills over 60s → (120 / 180) ≈ 0.6667
                val r3 = try { W.nativeDiagnosticUpdateAvgRateStat("kph_test", 30.0f, 60.0) }
                         catch (_: UnsatisfiedLinkError) { 0.0f }
                Log.i(TAG, "probeAvgRate: session1(30/60)=$r1 (expect 0.5), " +
                    "session2(+60/+60)=$r2 (expect ~0.75), " +
                    "session3(+30/+60)=$r3 (expect ~0.6667)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeAvgRate threw", t)
            }
        } else if (op == "probeappowner") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Self SteamID with account_id = 0xCAFE1234
                val selfSid = 0x0110000100000000L or 0xCAFE1234L
                W.setSteamId(selfSid)
                val selfAcct  = 0xCAFE1234.toInt()
                val otherAcct = 0x11111111.toInt()
                // pkg 4001 self-owned; pkg 4002 family-share-owned
                W.nativeDiagnosticInjectLicenseList(
                    packageIds = intArrayOf(4001, 4002),
                    ownerIds   = intArrayOf(selfAcct, otherAcct))
                // App 1100: self-owned package only
                W.nativeSetAppSourcePackages(1100, intArrayOf(4001))
                // App 2200: family-shared package only
                W.nativeSetAppSourcePackages(2200, intArrayOf(4002))
                // Also set owned_apps for both so the in_owned check passes
                W.setOwnedApps(intArrayOf(1100, 2200))

                W.setAppId(1100)
                val ownerDirect = try { W.nativeDiagnosticGetAppOwner() }
                                  catch (_: UnsatisfiedLinkError) { 0L }
                W.setAppId(2200)
                val ownerShared = try { W.nativeDiagnosticGetAppOwner() }
                                  catch (_: UnsatisfiedLinkError) { 0L }
                W.setAppId(0)

                val expectedShared = 0x0110000100000000L or 0x11111111L
                Log.i(TAG, "probeAppOwner: " +
                    "direct(1100)=${java.lang.Long.toHexString(ownerDirect)} " +
                    "shared(2200)=${java.lang.Long.toHexString(ownerShared)} " +
                    "(expect direct=${java.lang.Long.toHexString(selfSid)}, " +
                    "shared=${java.lang.Long.toHexString(expectedShared)})")
            } catch (t: Throwable) {
                Log.e(TAG, "probeAppOwner threw", t)
            }
        } else if (op == "probefamilyshare") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Self SteamID with account_id = 0xCAFE1234 (low 32 bits).
                val selfSid = 0x0110000100000000L or 0xCAFE1234L
                W.setSteamId(selfSid)
                val selfAcct = 0xCAFE1234.toInt()  // ~ -889004492 as signed
                // Two licenses: pkg 3001 owned by self, pkg 3002 owned by other.
                W.nativeDiagnosticInjectLicenseList(
                    packageIds = intArrayOf(3001, 3002),
                    ownerIds   = intArrayOf(selfAcct, 0x11111111))
                // App 1000 has only the self-owned package → not family-shared.
                W.nativeSetAppSourcePackages(1000, intArrayOf(3001))
                // App 2000 has only the other-owned package → family-shared.
                W.nativeSetAppSourcePackages(2000, intArrayOf(3002))
                // App 3000 has BOTH packages (mixed) → not family-shared
                // (any self-owned wins).
                W.nativeSetAppSourcePackages(3000, intArrayOf(3001, 3002))
                // Test each scenario by binding the appId then calling slot 27.
                W.setAppId(1000)
                val r1 = try { W.nativeDiagnosticBIsSubscribedFromFamilySharing() }
                         catch (_: UnsatisfiedLinkError) { false }
                W.setAppId(2000)
                val r2 = try { W.nativeDiagnosticBIsSubscribedFromFamilySharing() }
                         catch (_: UnsatisfiedLinkError) { false }
                W.setAppId(3000)
                val r3 = try { W.nativeDiagnosticBIsSubscribedFromFamilySharing() }
                         catch (_: UnsatisfiedLinkError) { false }
                W.setAppId(0)  // reset
                Log.i(TAG, "probeFamilyShare: app1000_selfOnly=$r1 (expect false) " +
                    "app2000_otherOnly=$r2 (expect true) " +
                    "app3000_mixed=$r3 (expect false)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeFamilyShare threw", t)
            }
        } else if (op == "probepurchasetime") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val pkgs = intArrayOf(2001, 2002, 2003)
                val owners = intArrayOf(0, 0, 0)
                W.nativeDiagnosticInjectLicenseList(pkgs, owners)
                // After inject, all 3 licenses have time_created = now.
                // We can't easily differentiate them without exposing
                // additional injection params. For this probe we just
                // verify the slot 8 lookup returns a non-zero value
                // when source packages map to known licenses.
                val testAppId = 9999
                W.nativeSetAppSourcePackages(testAppId, intArrayOf(2001, 2002))
                val earliest = try {
                    W.nativeDiagnosticGetEarliestPurchaseUnixTime(testAppId)
                } catch (_: UnsatisfiedLinkError) { -1 }
                val now = (System.currentTimeMillis() / 1000).toInt()
                val withinWindow = earliest > 0 && (now - earliest) < 60  // sanity: < 60s old
                // App with no source packages → returns 0.
                val unknownApp = try {
                    W.nativeDiagnosticGetEarliestPurchaseUnixTime(99988)
                } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probePurchaseTime: app=$testAppId earliest=$earliest " +
                    "now=$now withinWindow=$withinWindow " +
                    "unknownApp(99988)=$unknownApp " +
                    "(expect earliest within 60s of now; unknownApp=0)")
            } catch (t: Throwable) {
                Log.e(TAG, "probePurchaseTime threw", t)
            }
        } else if (op == "probebridgelicenses") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val packageIds = intArrayOf(10001, 10002, 10003)
                val ownerIds   = intArrayOf(0, 99999999, 77777777)
                try { W.nativeDiagnosticInjectLicenseList(packageIds, ownerIds) }
                catch (_: UnsatisfiedLinkError) {}
                val o1 = try { W.nativeDiagnosticGetLicenseOwner(10001) } catch (_: UnsatisfiedLinkError) { -2 }
                val o2 = try { W.nativeDiagnosticGetLicenseOwner(10002) } catch (_: UnsatisfiedLinkError) { -2 }
                val o3 = try { W.nativeDiagnosticGetLicenseOwner(10003) } catch (_: UnsatisfiedLinkError) { -2 }
                val miss = try { W.nativeDiagnosticGetLicenseOwner(99999) } catch (_: UnsatisfiedLinkError) { -2 }
                Log.i(TAG, "probeBridgeLicenses: injected 3 licenses; " +
                    "owner(10001)=$o1 (expect 0=self-owned), " +
                    "owner(10002)=$o2 (expect 99999999=family-share-A), " +
                    "owner(10003)=$o3 (expect 77777777=family-share-B), " +
                    "owner(99999)=$miss (expect -1=absent); " +
                    "look for 'license-list observer: 3 license(s) mirrored'")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeLicenses threw", t)
            }
        } else if (op == "probebridgefriends") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val sids = longArrayOf(
                    76561197960287930L,
                    76561197960265728L,
                    76561197960123456L,
                )
                try { W.nativeDiagnosticInjectFriendsList(sids) }
                catch (_: UnsatisfiedLinkError) {}
                // Wait briefly for the observer (synchronous in this thread)
                val n = try { W.nativeDiagnosticAchievementCount() }  // dummy timed call
                catch (_: UnsatisfiedLinkError) { 0 }
                // Read back via state op's friends section — use the existing
                // bootstrap-side accessor if alive, else assume the observer
                // wrote the count we expect (3).
                // For direct verification, use the WnSteamBootstrap friend
                // count — works when bootstrap dispatches to libsteamclient.
                val bs = com.winlator.cmod.feature.stores.steam.wnsteam.WnSteamBootstrap
                val frCount = bs.friendCount()
                Log.i(TAG, "probeBridgeFriends: injected 3 SIDs; bootstrap-side " +
                    "friendCount()=$frCount (expect 3 when bootstrap alive — " +
                    "look for 'friends-list observer: 3 mutual friend(s) mirrored' " +
                    "log line for direct verification)")
                @Suppress("UNUSED_VARIABLE") val _unused = n
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeFriends threw", t)
            }
        } else if (op == "probebridgelogon") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // Force baseline = false in case prior state is true.
                try { W.nativeDiagnosticInjectLogonState(false) } catch (_: UnsatisfiedLinkError) {}
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // false → true should fire SteamServersConnected_t.
                try { W.nativeDiagnosticInjectLogonState(true) } catch (_: UnsatisfiedLinkError) {}
                val depth2 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // true → true should NOT fire (idempotent dedup).
                try { W.nativeDiagnosticInjectLogonState(true) } catch (_: UnsatisfiedLinkError) {}
                val depth3 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                // true → false should fire SteamServersDisconnected_t.
                try { W.nativeDiagnosticInjectLogonState(false) } catch (_: UnsatisfiedLinkError) {}
                val depth4 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeBridgeLogon: cb depth $depth0 → $depth1 → $depth2 → $depth3 → $depth4 " +
                    "(expect Δ pattern: ? 0 +1 0 +1; the first transition is " +
                    "implementation-dependent on prior cached state)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeLogon threw", t)
            }
        } else if (op == "probebridgeclearrp") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Need a non-zero self steamId for SetRichPresence to not bail.
                W.setSteamId(0x110000100C123456L)
                val ok1 = try { W.nativeDiagnosticSetRichPresence("status", "Playing") }
                          catch (_: UnsatisfiedLinkError) { false }
                val ok2 = try { W.nativeDiagnosticSetRichPresence("connect", "+1.2.3.4:27015") }
                          catch (_: UnsatisfiedLinkError) { false }
                try { W.nativeDiagnosticClearRichPresence() }
                catch (_: UnsatisfiedLinkError) {}
                Log.i(TAG, "probeBridgeClearRp: setOk=$ok1+$ok2, then Clear — " +
                    "look for 3 wn-cm-bridge 'set_rich_presence' lines: " +
                    "1 key, 2 keys, 0 keys (the clear)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeClearRp threw", t)
            }
        } else if (op == "probebridgerp") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                val aliceId = 76561197960287930L
                try { W.nativeDiagnosticRequestFriendRichPresence(aliceId) }
                catch (_: UnsatisfiedLinkError) {}
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeBridgeRp: cb depth pre=$depth0 post=$depth1 " +
                    "(expect post-pre=1 synthetic FriendRichPresenceUpdate_t; " +
                    "wn-cm-bridge log line: 'request_user_info(...alice, flags=0x800)')")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeRp threw", t)
            }
        } else if (op == "probebridgeplayed") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Cycle 0→440→0→570→570→0. Expect bridge log per transition,
                // skip on no-change.
                W.setAppId(0)        // baseline (already 0; no-op expected)
                W.setAppId(440)      // → broadcast 440
                W.setAppId(440)      // no-op (dedup)
                W.setAppId(0)        // → clear
                W.setAppId(570)      // → broadcast 570
                W.setAppId(0)        // → clear
                Log.i(TAG, "probeBridgePlayed: cycled setAppId 0→440→440→0→570→0 — " +
                    "look for 4 wn-cm-bridge 'notify_games_played' lines " +
                    "(440 broadcast, 0 clear, 570 broadcast, 0 clear)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgePlayed threw", t)
            }
        } else if (op == "probeauthwrap") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val testAppId = 99988  // avoid colliding with seedtestschemacache 99999
                // Synthetic ownership ticket — 100 bytes of recognizable
                // gradient. Real Valve tickets are 600-800 bytes; this is
                // just enough to verify the wrapping math.
                val ownership = ByteArray(100) { ((it * 7 + 3) and 0xFF).toByte() }
                W.setAppId(testAppId)
                val inj = try { W.nativeDiagnosticInjectOwnershipTicket(testAppId, ownership) }
                          catch (_: UnsatisfiedLinkError) { false }
                val cachedLen = try { W.nativeDiagnosticGetCachedOwnershipTicket(testAppId, null) }
                                catch (_: UnsatisfiedLinkError) { -1 }
                // Call GetAuthSessionTicket via vtable. Buffer is 24+100=124 bytes
                // for the wrapped body, plus headroom.
                val buf = ByteArray(256)
                val hCall = try { W.nativeDiagnosticGetAuthTicket(buf) }
                            catch (_: UnsatisfiedLinkError) { -1 }
                // Inspect the wrapper header: first 4 bytes = size prefix 20 LE.
                val sizePrefix = (buf[0].toInt() and 0xFF) or
                                  ((buf[1].toInt() and 0xFF) shl 8) or
                                  ((buf[2].toInt() and 0xFF) shl 16) or
                                  ((buf[3].toInt() and 0xFF) shl 24)
                val connId     = (buf[12].toInt() and 0xFF) or
                                  ((buf[13].toInt() and 0xFF) shl 8) or
                                  ((buf[14].toInt() and 0xFF) shl 16) or
                                  ((buf[15].toInt() and 0xFF) shl 24)
                val connCount  = (buf[20].toInt() and 0xFF) or
                                  ((buf[21].toInt() and 0xFF) shl 8) or
                                  ((buf[22].toInt() and 0xFF) shl 16) or
                                  ((buf[23].toInt() and 0xFF) shl 24)
                // First byte of ownership-ticket region should match
                // ownership[0] = (0*7+3) & 0xFF = 3.
                val firstOwnByte = buf[24].toInt() and 0xFF
                Log.i(TAG, "probeAuthWrap: inj=$inj cachedLen=$cachedLen hCall=$hCall " +
                    "sizePrefix=$sizePrefix connId=$connId connCount=$connCount " +
                    "firstOwnByte=$firstOwnByte " +
                    "(expect inj=true cachedLen=100 sizePrefix=20 connId=hCall " +
                    "connCount=1 firstOwnByte=3)")
                // Reset appId so other probes aren't surprised by 99988.
                W.setAppId(0)
            } catch (t: Throwable) {
                Log.e(TAG, "probeAuthWrap threw", t)
            }
        } else if (op == "probebridgeticket") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // TF2 = 440 — common test appId. Until wn-session has
                // run get_app_ownership_ticket for this appId, the
                // cache is empty and the bridge returns 0.
                val sizeOnly = try {
                    W.nativeDiagnosticGetCachedOwnershipTicket(440, null)
                } catch (_: UnsatisfiedLinkError) { -1 }
                val tooSmall = ByteArray(8)
                val sizeReport = try {
                    W.nativeDiagnosticGetCachedOwnershipTicket(440, tooSmall)
                } catch (_: UnsatisfiedLinkError) { -1 }
                val bigBuf = ByteArray(8192)
                val filled = try {
                    W.nativeDiagnosticGetCachedOwnershipTicket(440, bigBuf)
                } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeBridgeTicket: appId=440 sizeOnly=$sizeOnly " +
                    "tooSmall=$sizeReport bigBuf=$filled " +
                    "(expect all=0 until wn-session pre-fetches the ticket; " +
                    "non-zero means cache hit + bytes copied or size-needed)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeTicket threw", t)
            }
        } else if (op == "probebridgereqbulk") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Mix of valid sids + 0 (should be filtered) + duplicates
                // (CMClient dedups internally on send).
                val sids = longArrayOf(
                    76561197960287932L,
                    76561197960287933L,
                    0L,                       // filtered
                    76561197960287934L,
                )
                val full = try {
                    W.nativeDiagnosticRequestUserInfoBulk(sids, flags = 0)  // default 0x47
                } catch (_: UnsatisfiedLinkError) { false }
                val nameOnly = try {
                    W.nativeDiagnosticRequestUserInfoBulk(sids, flags = 0x01)
                } catch (_: UnsatisfiedLinkError) { false }
                val empty = try {
                    W.nativeDiagnosticRequestUserInfoBulk(longArrayOf(), flags = 0)
                } catch (_: UnsatisfiedLinkError) { false }
                val allZero = try {
                    W.nativeDiagnosticRequestUserInfoBulk(longArrayOf(0L, 0L), flags = 0)
                } catch (_: UnsatisfiedLinkError) { false }
                Log.i(TAG, "probeBridgeReqBulk: full=$full nameOnly=$nameOnly " +
                    "empty=$empty allZero=$allZero " +
                    "(expect full=true nameOnly=true empty=false allZero=false; " +
                    "wn-cm-bridge lines should show count=3 for full/nameOnly)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeReqBulk threw", t)
            }
        } else if (op == "probebridgereq") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // A fresh sid that earlier seed ops didn't touch.
                val uncachedSid = 76561197960287932L
                val cachedSid   = 76561197960287930L  // possibly seeded by earlier seedtestfriends
                val r1 = try {
                    W.nativeDiagnosticRequestUserInformation(uncachedSid, nameOnly = false)
                } catch (_: UnsatisfiedLinkError) { false }
                val r2 = try {
                    W.nativeDiagnosticRequestUserInformation(uncachedSid, nameOnly = true)
                } catch (_: UnsatisfiedLinkError) { false }
                val r3 = try {
                    W.nativeDiagnosticRequestUserInformation(cachedSid, nameOnly = false)
                } catch (_: UnsatisfiedLinkError) { false }
                Log.i(TAG, "probeBridgeReq: " +
                    "uncached(fullSet)=$r1 uncached(nameOnly)=$r2 cached=$r3 " +
                    "(expect uncached=true=true cached=$r3 — look for wn-cm-bridge " +
                    "'request_user_info' log lines)")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeReq threw", t)
            }
        } else if (op == "probebridgename") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Invoke ISteamFriends.SetPersonaName via vtable slot 1
                // — exact same path a Wine-side game's lsteamclient.dll
                // takes. Exercises:
                //   1. Local pushed_state cache update
                //   2. PersonaStateChange_t(kPersonaChangeName) emit
                //   3. cm_bridge → wn_cm_set_persona_name dispatch
                //   4. SetPersonaNameResponse_t CallResult schedule
                val before = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                val unique = "BridgeProbe-${System.currentTimeMillis() % 100000}"
                val hCall = try {
                    W.nativeDiagnosticSetPersonaName(unique)
                } catch (_: UnsatisfiedLinkError) { 0L }
                val after = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "probeBridgeName: SetPersonaName('$unique') vtable[1] → " +
                    "hCall=$hCall cb depth before=$before after=$after " +
                    "(expect after-before=1 PersonaStateChange + check wn-cm-bridge " +
                    "log line: 'set_persona_name…→ live CMClient')")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridgeName threw", t)
            }
        } else if (op == "probebridge") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                // Toggle persona state — flips the cached value AND
                // dispatches through the cm_bridge to the live CMClient
                // (if registered). The bridge logs whether it dispatched
                // or dropped the call. Use a non-zero state to also
                // emit the come-online flag, exercising the full path.
                W.setPersonaState(0)  // baseline
                Thread.sleep(50)
                W.setPersonaState(1)  // online — should hit bridge
                Thread.sleep(50)
                W.setPersonaState(3)  // away — should hit bridge
                Log.i(TAG, "probeBridge: dispatched 3 setPersonaState calls — " +
                    "look for 'wn-cm-bridge' log lines (each call either logs " +
                    "'no active CMClient' or 'dispatched to live CMClient')")
            } catch (t: Throwable) {
                Log.e(TAG, "probeBridge threw", t)
            }
        } else if (op == "seedtestoverlay") {
            try {
                val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                val depth0 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setGameOverlayActive(true)            // false→true: +1
                W.setGameOverlayActive(true)            // dedup: +0
                val depthMid = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                W.setGameOverlayActive(false)           // true→false: +1
                W.setGameOverlayActive(false)           // dedup: +0
                W.setGameOverlayActive(true)            // false→true: +1
                val depth1 = try { W.nativeDiagnosticCallbackDepth() } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestOverlay: cb depth pre=$depth0 mid=$depthMid post=$depth1 " +
                    "(expect mid-pre=1, post-mid=2 — 3 transitions, 2 redundant sets de-duped)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestOverlay threw", t)
            }
        } else if (op == "seedteststats") {
            try {
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .setAchievementSchema(
                        apiNames    = arrayOf("ACH_TEST_FIRST",  "ACH_TEST_SECOND", "ACH_HIDDEN"),
                        displayNames = arrayOf("First Blood",    "Double Tap",      "???"),
                        descriptions = arrayOf("Score one kill.", "Score two kills.", "Reach the hidden ending."),
                        icons        = arrayOf("",                "",                ""),
                        hidden       = booleanArrayOf(false, false, true),
                    )
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .setAchievementProgress("ACH_TEST_FIRST", achieved = true, unlockTimeUnix = 1700000000)
                // Layer a non-English locale onto ACH_TEST_FIRST. A game
                // launched with PrefManager.containerLanguage="spanish"
                // would see GetAchievementDisplayAttribute("name") return
                // "Primera Sangre" rather than "First Blood" — locale
                // fallback chain test.
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .addAchievementLocale("ACH_TEST_FIRST", "spanish",
                        displayName = "Primera Sangre",
                        description = "Consigue una eliminación.")
                val cbDepthAfterSchema = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticCallbackDepth()
                } catch (_: UnsatisfiedLinkError) { -1 }
                // Unlock ACH_TEST_SECOND via the stub's SetAchievement
                // (slot 7). Flips pending_store so the next StoreStats
                // emits one UserAchievementStored_t for it.
                val setOk = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticSetAchievement("ACH_TEST_SECOND")
                } catch (_: UnsatisfiedLinkError) { false }
                // Also exercise StoreStats — emits UserStatsStored_t
                // AND one UserAchievementStored_t per pending unlock.
                val storeOk = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticStoreStats()
                } catch (_: UnsatisfiedLinkError) { false }
                val cbDepthAfterStore = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticCallbackDepth()
                } catch (_: UnsatisfiedLinkError) { -1 }
                // Progress-indicate on ACH_HIDDEN (still locked) — emits
                // UserAchievementStored_t with curProgress=50/max=100.
                val progressOk = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticIndicateAchievementProgress(
                            "ACH_HIDDEN", current = 50, max = 100)
                } catch (_: UnsatisfiedLinkError) { false }
                val cbDepthAfterProgress = try {
                    com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                        .nativeDiagnosticCallbackDepth()
                } catch (_: UnsatisfiedLinkError) { -1 }
                Log.i(TAG, "seedTestStats: pushed 3 synthetic achievements " +
                    "(cb depth post-schema=$cbDepthAfterSchema, " +
                    "setAch ok=$setOk, storeStats ok=$storeOk, " +
                    "cb depth post-store=$cbDepthAfterStore, " +
                    "indicateProgress ok=$progressOk, " +
                    "cb depth post-progress=$cbDepthAfterProgress)")
            } catch (t: Throwable) {
                Log.e(TAG, "seedTestStats threw", t)
            }
        } else if (op != "query" && op != "state" && after != before) {
            PrefManager.wnHybridMode = after
            try {
                SteamService.setHybridModeRuntime(after)
            } catch (t: Throwable) {
                Log.e(TAG, "setHybridModeRuntime threw", t)
            }
        }

        // Single stable marker line — grep target for cron-loop logcat
        // captures. Includes everything a verifier needs in one row.
        Log.i(
            TAG,
            "op=$op before=$before after=$after loggedIn=${SteamService.isLoggedIn}" +
                " bootstrapAlive=$bsInitialized steamId=${WnSteamBootstrap.steamId()}" +
                " persona='${WnSteamBootstrap.personaName() ?: ""}'",
        )

        // Verbose-state dump on `--es op state` for diagnostics. Exercises
        // every read-side hybrid accessor — useful for verifying the JNI
        // surface end-to-end without UI navigation. Per-app-bound
        // accessors (DLC count, app owner, etc.) return 0/false when the
        // bootstrap is in library/prewarm mode (appId=0) — the dump
        // surfaces that state explicitly.
        if (op == "state") {
            val bs = WnSteamBootstrap
            // Direct libsteamclient.so pushed-state reads. The legacy
            // bootstrap-side reads (bs.*) are kept for reference but
            // the .so reads are the source of truth in libsteamclient-
            // only mode — bootstrap is dormant. Showing both lets a
            // verifier spot any drift between layers.
            val W = com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
            val pSid     = runCatching { W.nativeGetPushedSteamId() }.getOrNull() ?: 0L
            val pName    = runCatching { W.nativeGetPushedPersonaName() }.getOrNull() ?: ""
            val pPState  = runCatching { W.nativeGetPushedPersonaState() }.getOrNull() ?: 0
            val pLoggedOn= runCatching { W.nativeGetPushedLoggedOn() }.getOrNull() ?: false
            val pAppId   = runCatching { W.nativeGetPushedAppId() }.getOrNull() ?: 0
            val pOwned   = runCatching { W.nativeGetPushedOwnedAppCount() }.getOrNull() ?: 0
            val pInst    = runCatching { W.nativeGetPushedInstalledAppCount() }.getOrNull() ?: 0
            val pFriends = runCatching { W.nativeGetPushedFriendCount() }.getOrNull() ?: 0
            val pCFiles  = runCatching { W.nativeGetPushedCloudFileCount() }.getOrNull() ?: 0
            val pCAcct   = runCatching { W.nativeGetPushedCloudEnabledAccount() }.getOrNull() ?: false
            val pCApp    = runCatching { W.nativeGetPushedCloudEnabledApp() }.getOrNull() ?: false
            val pETkt    = if (pAppId > 0) runCatching {
                W.nativeGetPushedEncryptedAppTicketSize(pAppId)
            }.getOrNull() ?: 0 else 0
            val pIpCountry = runCatching { W.nativeGetPushedIpCountry() }.getOrNull() ?: ""
            Log.i(TAG, "pushed: sid=$pSid pname='$pName' pstate=$pPState " +
                "loggedOn=$pLoggedOn appId=$pAppId owned=$pOwned installed=$pInst " +
                "friends=$pFriends cloudFiles=$pCFiles cloudAcct=$pCAcct cloudApp=$pCApp " +
                "ip='$pIpCountry' encTicketBytes=$pETkt")
            val sb = StringBuilder()
            sb.append("user(bootstrap): liveSid=").append(bs.liveSteamId())
                .append(" pname='").append(bs.personaName() ?: "").append('\'')
                .append(" pstate=").append(bs.personaState())
                .append(" loggedOnPub=").append(bs.loggedOnPublic())
            Log.i(TAG, sb.toString())

            // wn-session CMClient state: -1=noSession, 0=Disconnected,
            // 1=Connecting, 2=Connected, 3=LoggedOn. Cloud / ticket /
            // persona round-trips require state==3.
            val wnState = com.winlator.cmod.feature.stores.steam.service
                .SteamService.wnSessionStateForDiag()
            val wnStateLabel = when (wnState) {
                -1 -> "noSession"
                0 -> "Disconnected"
                1 -> "Connecting"
                2 -> "Connected"
                3 -> "LoggedOn"
                else -> "raw=$wnState"
            }
            val suspendReason = com.winlator.cmod.feature.stores.steam.service
                .SteamService.wnSessionSuspensionReasonForDiag()
            Log.i(TAG, "wn-session: state=$wnState ($wnStateLabel) " +
                "logonGateUntilMs=${com.winlator.cmod.feature.stores.steam.service.SteamService.logonGateUntilMs} " +
                "lastEResult=${com.winlator.cmod.feature.stores.steam.service.SteamService.lastLogonFailureEresult} " +
                "consecutive=${com.winlator.cmod.feature.stores.steam.service.SteamService.consecutiveLogonFailures} " +
                "suspend=[$suspendReason]")

            val appId = bs.currentAppId()
            sb.setLength(0)
            sb.append("apps(bootstrap): boundAppId=").append(appId)
                .append(" subscribed=").append(bs.isSubscribedApp(appId))
                .append(" installed=").append(bs.isAppInstalled(appId))
                .append(" lang=").append(bs.currentGameLanguage() ?: "")
                .append(" dlcCount=").append(bs.dlcCount(appId))
                .append(" owner=").append(bs.appOwner())
                .append(" famShared=").append(bs.isSubscribedFromFamilySharing())
            Log.i(TAG, sb.toString())

            sb.setLength(0)
            sb.append("cloud(bootstrap): account=").append(bs.cloudEnabledForAccount())
                .append(" app=").append(bs.cloudEnabledForApp())
                .append(" files=").append(bs.cloudFileCount())
                .append(" quota=").append(bs.cloudQuota().joinToString("/"))
            Log.i(TAG, sb.toString())

            val achList = bs.listAchievements()
            // Diagnostic — directDirectFromPushed reads pushed().achievements.size()
            // straight from libsteamclient.so without going through the ISteamUserStats
            // vtable, so a mismatch between this and bs.numAchievements() (which
            // dispatches through the bootstrap's vtable[14]) localizes plumbing bugs.
            val directFromPushed = try {
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .nativeDiagnosticAchievementCount()
            } catch (_: UnsatisfiedLinkError) { -1 }
            val cbDepth = try {
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .nativeDiagnosticCallbackDepth()
            } catch (_: UnsatisfiedLinkError) { -1 }
            sb.setLength(0)
            sb.append("stats: numAch=").append(bs.numAchievements())
                .append(" listLen=").append(achList.size)
                .append(" directPushed=").append(directFromPushed)
                .append(" cbQueueDepth=").append(cbDepth)
                .append(" firstAch=").append(achList.firstOrNull() ?: "")
            Log.i(TAG, sb.toString())

            // Prefer bootstrap accessors when alive (they exercise the
            // ISteamFriends vtable); fall back to pushed-state when the
            // bootstrap is dormant in single-client mode.
            val friends = bs.listFriends()
            val pushedFriendCount = runCatching { W.nativeGetPushedFriendCount() }.getOrNull() ?: 0
            val pushedFirstFriend = runCatching { W.nativeGetPushedFirstFriend() }.getOrNull() ?: 0L
            sb.setLength(0)
            sb.append("friends: immediateCount=")
                .append(bs.friendCount(WnSteamBootstrap.FriendFlags.Immediate))
                .append(" listLen=").append(friends.size)
                .append(" firstId=").append(friends.firstOrNull() ?: 0)
                .append(" (pushed=").append(pushedFriendCount)
                .append(" firstSid=").append(pushedFirstFriend).append(")")
            Log.i(TAG, sb.toString())

            // ISteamUtils accessors via the bootstrap vtable; pushed-state
            // values follow so the values are still visible when the
            // bootstrap is dormant in single-client mode.
            val pushedServerRealTime = runCatching { W.nativeGetPushedServerRealTime() }.getOrNull() ?: 0
            val pushedIpCountryUtil  = runCatching { W.nativeGetPushedIpCountry() }.getOrNull() ?: ""
            val pushedUiLang         = runCatching { W.nativeGetPushedUiLanguage() }.getOrNull() ?: ""
            sb.setLength(0)
            sb.append("utils: serverTime=").append(bs.serverRealTime())
                .append(" ipCountry=").append(bs.ipCountry() ?: "")
                .append(" uiLang=").append(bs.steamUiLanguage() ?: "")
                .append(" battery=").append(bs.currentBatteryPower())
                .append(" (pushed: time=").append(pushedServerRealTime)
                .append(" ip='").append(pushedIpCountryUtil).append("'")
                .append(" lang='").append(pushedUiLang).append("')")
            Log.i(TAG, sb.toString())

            // TCP IPC listener status — accepted-connection counter from
            // our open-source libsteamclient.so. Going from 0 to non-zero
            // means a Wine-side lsteamclient.dll has reached the .so.
            val tcpAccepted = try {
                com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                    .nativeDiagnosticTcpAccepted()
            } catch (_: UnsatisfiedLinkError) { -1 }
            Log.i(TAG, "tcp: accepted=$tcpAccepted")
        }
    }

    companion object {
        const val ACTION = "com.winnative.cmod.action.HYBRID_MODE"
        private const val TAG = "HybridModeReceiver"
    }
}
