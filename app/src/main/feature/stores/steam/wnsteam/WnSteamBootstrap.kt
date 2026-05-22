package com.winlator.cmod.feature.stores.steam.wnsteam

import android.content.Context
import android.util.Log
import com.winlator.cmod.feature.stores.steam.utils.PrefManager
import com.winlator.cmod.runtime.display.environment.ImageFs
import java.io.File

/**
 * JNI facade over libwnsteambootstrap.so — our open-source equivalent of
 * the closed-source libsteambootstrap.so that other Wine-on-Android
 * launchers use. Loads Valve's native libsteamclient.so in this process
 * so Wine's lsteamclient.dll (inside our Proton prefix) has a peer to
 * talk to over the Steam3Master / SteamClientService TCP sockets that
 * libsteamclient.so binds when it sees those env vars.
 *
 * Lifecycle:
 *   1. Before exec'ing wine for a Steam game:
 *        WnSteamBootstrap.start(ctx, libPath, home, steam3Master,
 *                                steamClientService, extraEnv,
 *                                accountName, refreshToken, steamId64)
 *      → -1 = libsteamclient.so not on disk yet (online play / DLC
 *             checks will fall through; basic launch still works via
 *             our local PICS / ticket cache)
 *      → -2/-3/-4 = libsteamclient.so loaded but failed to set up
 *                   (see logcat for the dlerror or interface name)
 *      →  0 = ready; libsteamclient.so is hosting the TCP services
 *
 *   2. (optional, immediately after start): prepareApp(parent, dlcs)
 *      so libsteamclient.so warms its own PICS cache before Wine queries.
 *
 *   3. After the wine subprocess exits: stop()
 *
 * Threading: nativeInit / nativeShutdown serialize internally on a
 * mutex; safe to call from any thread. The other methods are no-ops
 * when not initialized.
 *
 * libsteamclient.so source:
 *   We do NOT bundle Valve's binary. The launcher provisions it at
 *   first run (Phase 8b.3) from a configurable URL into
 *   <imageFs.libDir>/libsteamclient.so. If absent, start() returns -1
 *   and the rest of this object short-circuits.
 */
object WnSteamBootstrap {

    private const val TAG = "WnSteamBootstrap"

    @Volatile private var initialized = false
    @Volatile private var prewarmRan = false

    init {
        try {
            System.loadLibrary("wnsteambootstrap")
        } catch (t: UnsatisfiedLinkError) {
            // CMake build might not be enabled in all variants; surface a
            // warning rather than crashing import order.
            Log.w(TAG, "libwnsteambootstrap.so not found in jniLibs: ${t.message}")
        }
    }

    /**
     * Initialize libsteamclient.so and connect a Steam pipe + global user.
     * @param extraEnv array of "KEY=value" strings to setenv() BEFORE
     *                  dlopen — anything libsteamclient.so reads at
     *                  module init time has to be here.
     * @return 0 on success, negative error code otherwise.
     */
    /**
     * Initialize libsteamclient.so.
     *
     * @param appId  the Steam app this session is for (binds
     *               ISteamApps/RemoteStorage/UserStats to its context).
     *               Pass 0 for library/prewarm mode — auth-gated
     *               app-scoped interfaces will stay null but
     *               User/Utils/Friends still work.
     */
    @Synchronized
    fun start(
        context: Context,
        libPath: String,
        home: String,
        steam3Master: String,
        steamClientService: String,
        extraEnv: Array<String>,
        accountName: String?,
        refreshToken: String?,
        steamId64: Long,
        appId: Int = 0,
    ): Int {
        if (initialized) {
            Log.i(TAG, "start: already initialized")
            return 0
        }
        val rc = try {
            nativeInit(context, libPath, home, steam3Master, steamClientService,
                       extraEnv, accountName, refreshToken, steamId64, appId)
        } catch (t: UnsatisfiedLinkError) {
            Log.w(TAG, "nativeInit unavailable: ${t.message}")
            return -100
        }
        if (rc == 0) initialized = true
        Log.i(TAG, "start rc=$rc initialized=$initialized appId=$appId")
        return rc
    }

    /**
     * Pre-warm the native Steam client: stage libsteamclient.so and log it
     * on ONCE — early, from a long-lived caller (SteamService, a foreground
     * service) — so when a Bionic Steam game launches later the
     * authenticated session already exists and [start] is a no-op (the
     * native re-entry guard), instead of a fresh logon firing in the
     * launch path. The session then stays live for the whole process
     * lifetime via the bootstrap's callback-pump thread.
     *
     * Runs off the caller's thread (nativeInit blocks up to ~10s on the
     * logon poll). Idempotent. No-op if no Steam credentials are stored
     * yet (a later call retries).
     */
    fun prewarm(context: Context) {
        if (prewarmRan || initialized) return
        prewarmRan = true
        val app = context.applicationContext
        Thread({
            try {
                val account = PrefManager.username
                val token = PrefManager.refreshToken
                val steamId = PrefManager.steamUserSteamId64
                if (account.isEmpty() || token.isEmpty() || steamId <= 0L) {
                    Log.i(TAG, "prewarm: no Steam credentials yet; skipping")
                    prewarmRan = false   // permit a retry once signed in
                    return@Thread
                }
                if (!WnSteamAssetsInstaller.installBionicRuntime(app)) {
                    Log.w(TAG, "prewarm: libsteamclient.so staging failed")
                    prewarmRan = false
                    return@Thread
                }
                val imageFs = ImageFs.find(app)
                val libPath = File(imageFs.rootDir, "usr/lib/libsteamclient.so").absolutePath
                val home = File(imageFs.rootDir, "home").absolutePath
                val rc = start(
                    app, libPath, home,
                    "127.0.0.1:57343", "127.0.0.1:57344",
                    emptyArray(), account, token, steamId,
                    appId = 0,   // library/prewarm: no specific game bound
                )
                Log.i(TAG, "prewarm: bootstrap start rc=$rc (Steam client pre-warmed, appId=0)")
            } catch (t: Throwable) {
                Log.e(TAG, "prewarm failed", t)
                prewarmRan = false
            }
        }, "WnSteamPrewarm").start()
    }

    @Synchronized
    fun stop() {
        if (!initialized) return
        try { nativeShutdown() } catch (_: UnsatisfiedLinkError) {}
        initialized = false
        Log.i(TAG, "stop done")
    }

    /**
     * Pre-warm libsteamclient.so's PICS cache for the game + DLC about to
     * launch. No-op when not initialized.
     */
    fun prepareApp(parentAppId: Int, dlcAppIds: IntArray) {
        if (!initialized) return
        val all = IntArray(1 + dlcAppIds.size).also {
            it[0] = parentAppId
            System.arraycopy(dlcAppIds, 0, it, 1, dlcAppIds.size)
        }
        try { nativePrepareApp(all) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Toggle per-app cloud sync. No-op when not initialized. */
    fun setCloudEnabled(appId: Int, enabled: Boolean) {
        if (!initialized) return
        try { nativeSetCloudEnabled(appId, enabled) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Whether libsteamclient.so reports a logged-on user. False before
     * [start] succeeds, false when libsteamclient.so connects anonymously
     * (no cached credentials in the Wine prefix), true once authentication
     * has landed. Cheap synchronous read; safe from any thread.
     */
    fun isLoggedOn(): Boolean {
        if (!initialized) return false
        return try { nativeIsLoggedOn() } catch (_: UnsatisfiedLinkError) { false }
    }

    /**
     * SteamID64 libsteamclient.so reports for the current user, or 0 when
     * not logged on. Useful for verifying that bootstrap auth landed for
     * the same account our wn-steam-client session is using.
     */
    fun steamId(): Long {
        if (!initialized) return 0
        return try { nativeGetSteamId() } catch (_: UnsatisfiedLinkError) { 0L }
    }

    @JvmStatic private external fun nativeInit(
        context: Context,
        libPath: String,
        home: String,
        steam3Master: String,
        steamClientService: String,
        extraEnv: Array<String>,
        accountName: String?,
        refreshToken: String?,
        steamId64: Long,
        appId: Int,
    ): Int
    @JvmStatic private external fun nativeShutdown()
    @JvmStatic private external fun nativePrepareApp(appIds: IntArray)
    @JvmStatic private external fun nativeSetCloudEnabled(appId: Int, enabled: Boolean)
    @JvmStatic private external fun nativeIsLoggedOn(): Boolean
    @JvmStatic private external fun nativeGetSteamId(): Long
    @JvmStatic private external fun nativeBIsSubscribedApp(appId: Int): Boolean
    @JvmStatic private external fun nativeISteamAppsBIsAppInstalled(appId: Int): Boolean
    @JvmStatic private external fun nativeISteamAppsGetAppInstallDir(appId: Int): String?
    @JvmStatic private external fun nativeISteamAppsGetInstalledDepots(appId: Int): IntArray
    @JvmStatic private external fun nativeISteamAppsGetCurrentGameLanguage(): String?
    @JvmStatic private external fun nativeISteamAppsBIsDlcInstalled(dlcAppId: Int): Boolean
    @JvmStatic private external fun nativeISteamAppsGetEarliestPurchaseUnixTime(appId: Int): Int
    @JvmStatic private external fun nativeISteamAppsGetDLCCount(appId: Int): Int
    @JvmStatic private external fun nativeISteamAppsGetAppOwner(): Long
    @JvmStatic private external fun nativeISteamAppsBIsSubscribedFromFamilySharing(): Boolean
    @JvmStatic private external fun nativeISteamAppsGetAppBuildId(): Int
    @JvmStatic private external fun nativeISteamUserBLoggedOn(): Boolean
    @JvmStatic private external fun nativeISteamUserHasLicenseForApp(steamId64: Long, appId: Int): Int
    @JvmStatic private external fun nativeISteamUserGetSteamID(): Long
    @JvmStatic private external fun nativeISteamUtilsGetAppID(): Int
    @JvmStatic private external fun nativeISteamUtilsGetServerRealTime(): Int
    @JvmStatic private external fun nativeISteamUtilsGetIPCountry(): String?
    @JvmStatic private external fun nativeISteamUtilsGetSteamUILanguage(): String?
    @JvmStatic private external fun nativeISteamUtilsGetCurrentBatteryPower(): Int
    @JvmStatic private external fun nativeISteamUtilsGetImageSize(imageHandle: Int): IntArray
    @JvmStatic private external fun nativeISteamUtilsGetImageRGBA(imageHandle: Int, outRgba: ByteArray): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageGetFileCount(): Int
    @JvmStatic private external fun nativeISteamRemoteStorageIsCloudEnabledForAccount(): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageIsCloudEnabledForApp(): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageGetQuota(): LongArray?
    @JvmStatic private external fun nativeISteamRemoteStorageListFiles(): Array<String>?
    @JvmStatic private external fun nativeISteamRemoteStorageFileExists(name: String): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageFileRead(name: String): ByteArray?
    @JvmStatic private external fun nativeISteamRemoteStorageFileWrite(name: String, data: ByteArray): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageFileDelete(name: String): Boolean
    @JvmStatic private external fun nativeISteamRemoteStorageSetCloudEnabledForApp(enabled: Boolean)
    @JvmStatic private external fun nativeISteamUserStatsRequestCurrentStats(): Boolean
    @JvmStatic private external fun nativeISteamUserStatsGetNumAchievements(): Int
    @JvmStatic private external fun nativeISteamUserStatsListAchievements(): Array<String>?
    @JvmStatic private external fun nativeISteamUserStatsGetAchievementAndUnlockTime(name: String): IntArray?
    @JvmStatic private external fun nativeISteamUserStatsSetAchievement(name: String): Boolean
    @JvmStatic private external fun nativeISteamUserStatsClearAchievement(name: String): Boolean
    @JvmStatic private external fun nativeISteamUserStatsStoreStats(): Boolean
    @JvmStatic private external fun nativeISteamUserStatsGetStatInt(name: String): Int
    @JvmStatic private external fun nativeISteamUserStatsGetStatFloat(name: String): Float
    @JvmStatic private external fun nativeISteamUserStatsSetStatInt(name: String, data: Int): Boolean
    @JvmStatic private external fun nativeISteamUserStatsSetStatFloat(name: String, data: Float): Boolean
    @JvmStatic private external fun nativeISteamUserStatsUpdateAvgRateStat(name: String, countThisSession: Float, sessionLength: Double): Boolean
    @JvmStatic private external fun nativeISteamUserStatsGetAchievementDisplayAttribute(name: String, key: String): String?
    @JvmStatic private external fun nativeISteamUserStatsGetAchievementIcon(name: String): Int
    @JvmStatic private external fun nativeISteamFriendsGetPersonaName(): String?
    @JvmStatic private external fun nativeISteamFriendsGetPersonaState(): Int
    @JvmStatic private external fun nativeISteamFriendsGetFriendCount(flags: Int): Int
    @JvmStatic private external fun nativeISteamFriendsListFriends(flags: Int): LongArray
    @JvmStatic private external fun nativeISteamFriendsGetFriendPersonaName(steamId: Long): String?
    @JvmStatic private external fun nativeISteamFriendsGetFriendPersonaState(steamId: Long): Int
    @JvmStatic private external fun nativeSubscribeCallback(id: Int)
    @JvmStatic private external fun nativeUnsubscribeCallback(id: Int)
    @JvmStatic private external fun nativeAwaitCallback(id: Int, timeoutMs: Int): ByteArray?

    /**
     * Hybrid stage-2: query the bootstrap's cached ISteamApps interface
     * for ownership of [appId]. Returns false when the bootstrap isn't
     * initialized OR the session isn't authenticated OR ISteamApps wasn't
     * obtainable (auth-gated by libsteamclient.so). Synchronous and cheap
     * — safe from any thread. The first real backend method we route
     * through libsteamclient.so's public ISteam* surface instead of the
     * wn-steam-client CM session; verified by the stage-2 smoke test
     * (BIsSubscribedApp(242760) → 1 once logged on).
     */
    fun isSubscribedApp(appId: Int): Boolean {
        if (!initialized) return false
        return try {
            nativeBIsSubscribedApp(appId)
        } catch (_: UnsatisfiedLinkError) {
            false
        }
    }

    /**
     * Hybrid stage-2 — ISteamApps install-state surface.
     * All auth-gated; the bootstrap's view of the SteamAppId-bound app
     * unless otherwise noted. The SteamService install-state /
     * library-state ops can route through these instead of
     * wn-steam-client's PrefManager + PICS lookups.
     */
    fun isAppInstalled(appId: Int): Boolean =
        if (!initialized) false
        else try { nativeISteamAppsBIsAppInstalled(appId) }
             catch (_: UnsatisfiedLinkError) { false }

    fun appInstallDir(appId: Int): String? =
        if (!initialized) null
        else try { nativeISteamAppsGetAppInstallDir(appId) }
             catch (_: UnsatisfiedLinkError) { null }

    fun installedDepots(appId: Int): IntArray =
        if (!initialized) IntArray(0)
        else try { nativeISteamAppsGetInstalledDepots(appId) }
             catch (_: UnsatisfiedLinkError) { IntArray(0) }

    /** Steam's notion of the current game language (e.g. "english"). */
    fun currentGameLanguage(): String? =
        if (!initialized) null
        else try { nativeISteamAppsGetCurrentGameLanguage() }
             catch (_: UnsatisfiedLinkError) { null }

    /** Is the given DLC installed for the bound app (SteamAppId). */
    fun isDlcInstalled(dlcAppId: Int): Boolean =
        if (!initialized) false
        else try { nativeISteamAppsBIsDlcInstalled(dlcAppId) }
             catch (_: UnsatisfiedLinkError) { false }

    /** Unix epoch (rtime32) when the user first acquired the app; 0 if unowned. */
    fun earliestPurchaseUnixTime(appId: Int): Int =
        if (!initialized) 0
        else try { nativeISteamAppsGetEarliestPurchaseUnixTime(appId) }
             catch (_: UnsatisfiedLinkError) { 0 }

    /** Number of DLCs the app has. 0 if not bound/unauthenticated. */
    fun dlcCount(appId: Int): Int =
        if (!initialized) 0
        else try { nativeISteamAppsGetDLCCount(appId) }
             catch (_: UnsatisfiedLinkError) { 0 }

    /**
     * Owner SteamID for the bound app. Equals the current user under
     * normal ownership; differs when the app is being played via Steam
     * Family Sharing (returns the lender's SteamID). 0 if not authed.
     */
    fun appOwner(): Long =
        if (!initialized) 0L
        else try { nativeISteamAppsGetAppOwner() }
             catch (_: UnsatisfiedLinkError) { 0L }

    /** Whether the bound app's license came via Family Sharing (vs direct ownership). */
    fun isSubscribedFromFamilySharing(): Boolean =
        if (!initialized) false
        else try { nativeISteamAppsBIsSubscribedFromFamilySharing() }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * PICS public-branch build id for the bound app. 0 when no app is
     * bound or no PICS data has resolved yet.
     */
    fun appBuildId(): Int =
        if (!initialized) 0
        else try { nativeISteamAppsGetAppBuildId() }
             catch (_: UnsatisfiedLinkError) { 0 }

    /**
     * Hybrid stage-2 — ISteamUser surface.
     * [loggedOnPublic] routes BLoggedOn through the public iface
     * (same answer as [isLoggedOn] but a second-path sanity check).
     * [userHasLicenseForApp] result: 0 = has license · 1 = no license ·
     * 2 = no-auth (call dispatched before logon completed). Replaces
     * wn-steam-client's CMsgClientGetLicenseList membership lookup.
     */
    fun loggedOnPublic(): Boolean =
        if (!initialized) false
        else try { nativeISteamUserBLoggedOn() }
             catch (_: UnsatisfiedLinkError) { false }

    fun userHasLicenseForApp(steamId64: Long, appId: Int): Int =
        if (!initialized) 2
        else try { nativeISteamUserHasLicenseForApp(steamId64, appId) }
             catch (_: UnsatisfiedLinkError) { 2 }

    /**
     * Hybrid stage-2: the SteamID64 libsteamclient.so reports for the
     * current ISteamUser session, or 0 when the session isn't logged on
     * yet. Differs from [steamId] (which returns the value Kotlin
     * supplied at init from PrefManager) — comparing the two is the
     * strongest cross-check that the bootstrap really is authenticated
     * as the same account.
     */
    fun liveSteamId(): Long {
        if (!initialized) return 0
        return try {
            nativeISteamUserGetSteamID()
        } catch (_: UnsatisfiedLinkError) {
            0L
        }
    }

    /**
     * Hybrid stage-2: AppID libsteamclient.so currently has bound for
     * this session (from the SteamAppId env we set before dlopen, or
     * whatever app context the session attached to). 0 when ISteamUtils
     * isn't cached — should not happen if init succeeded.
     */
    fun currentAppId(): Int {
        if (!initialized) return 0
        return try {
            nativeISteamUtilsGetAppID()
        } catch (_: UnsatisfiedLinkError) {
            0
        }
    }

    /**
     * Hybrid stage-2 — ISteamUtils auth-FREE accessors.
     * Work even without a logged-on session because ISteamUtils is
     * pipe-only (no user handle). The simplest way to verify the
     * bootstrap JNI surface end-to-end while Steam-side authentication
     * is unavailable (rate-limit, expired token, etc.).
     */
    fun serverRealTime(): Int =
        if (!initialized) 0
        else try { nativeISteamUtilsGetServerRealTime() }
             catch (_: UnsatisfiedLinkError) { 0 }

    /** ISO 3166-1 alpha-2 (e.g. "US"); null if unknown. */
    fun ipCountry(): String? =
        if (!initialized) null
        else try { nativeISteamUtilsGetIPCountry() }
             catch (_: UnsatisfiedLinkError) { null }

    /** Steam UI language (e.g. "english"); null if unknown. */
    fun steamUiLanguage(): String? =
        if (!initialized) null
        else try { nativeISteamUtilsGetSteamUILanguage() }
             catch (_: UnsatisfiedLinkError) { null }

    /** Battery 0..100, or 255 when on AC power. */
    fun currentBatteryPower(): Int =
        if (!initialized) 255
        else try { nativeISteamUtilsGetCurrentBatteryPower() }
             catch (_: UnsatisfiedLinkError) { 255 }

    /**
     * Returns [width, height] of the image at [imageHandle] (a
     * HSteamImage from GetMediumFriendAvatar / GetAchievementIcon /
     * etc.). Both 0 if the handle is invalid or the image isn't loaded.
     */
    fun imageSize(imageHandle: Int): IntArray =
        if (!initialized) intArrayOf(0, 0)
        else try {
            nativeISteamUtilsGetImageSize(imageHandle)
        } catch (_: UnsatisfiedLinkError) { intArrayOf(0, 0) }

    /**
     * Materialize the image's RGBA8888 pixels. Caller must size [outRgba]
     * to width * height * 4 (from [imageSize]). Returns true on success.
     */
    fun imageRGBA(imageHandle: Int, outRgba: ByteArray): Boolean =
        if (!initialized) false
        else try { nativeISteamUtilsGetImageRGBA(imageHandle, outRgba) }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Hybrid stage-2 — ISteamRemoteStorage readouts. All auth-gated:
     * return 0/false when the session isn't logged on (the bootstrap
     * couldn't instantiate ISteamRemoteStorage at init). The cloud-sync
     * surface the hybrid backend port will route through these instead
     * of wn-steam-client's CMsgClientGetCloudFile* CM calls.
     */
    fun cloudFileCount(): Int =
        if (!initialized) 0
        else try { nativeISteamRemoteStorageGetFileCount() }
             catch (_: UnsatisfiedLinkError) { 0 }

    fun cloudEnabledForAccount(): Boolean =
        if (!initialized) false
        else try { nativeISteamRemoteStorageIsCloudEnabledForAccount() }
             catch (_: UnsatisfiedLinkError) { false }

    fun cloudEnabledForApp(): Boolean =
        if (!initialized) false
        else try { nativeISteamRemoteStorageIsCloudEnabledForApp() }
             catch (_: UnsatisfiedLinkError) { false }

    /** [totalBytes, availableBytes] — both 0 if unavailable. */
    fun cloudQuota(): LongArray =
        if (!initialized) longArrayOf(0L, 0L)
        else try {
            nativeISteamRemoteStorageGetQuota() ?: longArrayOf(0L, 0L)
        } catch (_: UnsatisfiedLinkError) {
            longArrayOf(0L, 0L)
        }

    /**
     * Hybrid stage-2 — kick the ISteamUserStats schema/values download.
     * Returns true if the call dispatched; results arrive later via the
     * UserStatsReceived_t callback the bootstrap's callback pump drains.
     * The hybrid achievements port (replaces wn-steam-client's
     * CMsgClientGetUserStats) will need this plus a callback bridge.
     */
    fun requestCurrentStats(): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsRequestCurrentStats() }
             catch (_: UnsatisfiedLinkError) { false }

    /** Number of achievements the schema reports — 0 before [requestCurrentStats] completes. */
    fun numAchievements(): Int =
        if (!initialized) 0
        else try { nativeISteamUserStatsGetNumAchievements() }
             catch (_: UnsatisfiedLinkError) { 0 }

    /**
     * Cloud file entry — a name + size pair as returned by
     * ISteamRemoteStorage.GetFileNameAndSize. Size is bytes; -1 means
     * the size couldn't be parsed (defensive; the SDK doesn't return -1).
     */
    data class CloudFileEntry(val name: String, val size: Int)

    /**
     * Hybrid stage-2 — enumerate every cloud file the bootstrap session
     * has access to. Empty list when the session isn't logged on or the
     * call returns nothing. The wn-steam-client `getCloudFileList`
     * replacement target: callers can iterate entries to seed the
     * SteamService cloud-list UI, then [cloudFileRead] each one for the
     * bytes. Synchronous + safe from any thread.
     */
    fun listCloudFiles(): List<CloudFileEntry> {
        if (!initialized) return emptyList()
        val raw = try {
            nativeISteamRemoteStorageListFiles()
        } catch (_: UnsatisfiedLinkError) {
            null
        } ?: return emptyList()
        return raw.map { row ->
            val tab = row.indexOf('\t')
            if (tab <= 0) {
                CloudFileEntry(row, -1)
            } else {
                val name = row.substring(0, tab)
                val size = row.substring(tab + 1).toIntOrNull() ?: -1
                CloudFileEntry(name, size)
            }
        }
    }

    /** Does the named cloud file currently exist for the bootstrap session? */
    fun cloudFileExists(name: String): Boolean =
        if (!initialized) false
        else try { nativeISteamRemoteStorageFileExists(name) }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Read the named cloud file. Returns null when the bootstrap isn't
     * authenticated, the file doesn't exist, or the read was truncated
     * (same rejection policy as wn-steam-client's commit 6bbb36e). For
     * large files prefer the async variants (not yet wired).
     */
    fun cloudFileRead(name: String): ByteArray? =
        if (!initialized) null
        else try { nativeISteamRemoteStorageFileRead(name) }
             catch (_: UnsatisfiedLinkError) { null }

    /** Write data to the named cloud file. False on any failure. */
    fun cloudFileWrite(name: String, data: ByteArray): Boolean =
        if (!initialized) false
        else try { nativeISteamRemoteStorageFileWrite(name, data) }
             catch (_: UnsatisfiedLinkError) { false }

    /** Delete the named cloud file. False on any failure. */
    fun cloudFileDelete(name: String): Boolean =
        if (!initialized) false
        else try { nativeISteamRemoteStorageFileDelete(name) }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Set the per-app cloud-sync toggle. The SteamService `setCloudEnabled`
     * replacement target — this is what the per-game "Cloud Saves" UI flips.
     * No-op when the bootstrap isn't authenticated.
     */
    fun setCloudEnabledForApp(enabled: Boolean) {
        if (!initialized) return
        try { nativeISteamRemoteStorageSetCloudEnabledForApp(enabled) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Achievement schema entry as returned by libsteamclient.so. The
     * Goldberg achievements.json file the wn-steam-client used to
     * generate ([[project_wnsteam_userstats_achievements]]) is built from
     * the same fields — the hybrid backend port can populate that file
     * by walking [listAchievementsFull] instead of the JavaSteam path.
     */
    data class Achievement(
        val apiName: String,
        val displayName: String?,
        val description: String?,
        val hidden: Boolean,
        val achieved: Boolean,
        val unlockTimeRtime32: Int,
    )

    /** All achievement API names; empty before [requestCurrentStats] completes. */
    fun listAchievements(): List<String> =
        if (!initialized) emptyList()
        else try {
            (nativeISteamUserStatsListAchievements() ?: emptyArray()).toList()
        } catch (_: UnsatisfiedLinkError) {
            emptyList()
        }

    /**
     * Walks every achievement and fills the display + state fields in
     * one call so callers don't pay per-entry JNI overhead. Costs
     * 3·N+1 JNI calls under the hood (list + 3 per entry); for the
     * typical handful of achievements that's still microseconds.
     */
    fun listAchievementsFull(): List<Achievement> = listAchievements().map { name ->
        val unlock = try {
            nativeISteamUserStatsGetAchievementAndUnlockTime(name)
        } catch (_: UnsatisfiedLinkError) { null }
        val achieved = (unlock?.getOrNull(0) ?: 0) != 0
        val unlockTime = unlock?.getOrNull(1) ?: 0
        val display = try {
            nativeISteamUserStatsGetAchievementDisplayAttribute(name, "name")
        } catch (_: UnsatisfiedLinkError) { null }
        val desc = try {
            nativeISteamUserStatsGetAchievementDisplayAttribute(name, "desc")
        } catch (_: UnsatisfiedLinkError) { null }
        val hidden = try {
            nativeISteamUserStatsGetAchievementDisplayAttribute(name, "hidden") == "1"
        } catch (_: UnsatisfiedLinkError) { false }
        Achievement(name, display, desc, hidden, achieved, unlockTime)
    }

    /** Marks the achievement unlocked locally; flush with [storeStats]. */
    fun setAchievement(name: String): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsSetAchievement(name) }
             catch (_: UnsatisfiedLinkError) { false }

    /** Re-locks the achievement locally; flush with [storeStats]. */
    fun clearAchievement(name: String): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsClearAchievement(name) }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Flushes pending stat/achievement writes to the server. Steam
     * batches set/clear locally until StoreStats(); per the SDK contract,
     * the call dispatches and the actual commit result arrives later
     * via UserStatsStored_t (id [Callback.UserStatsStored]). Use
     * [awaitCallback] to block for the commit response if needed.
     */
    fun storeStats(): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsStoreStats() }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Hybrid stage-2 — ISteamUserStats013 stat read/write surface.
     * Stats live alongside achievements in the per-app schema; pair
     * with [storeStats] for the actual server commit. Caller must
     * know the stat's type (int32 vs float) — these are app-specific
     * schema definitions, not auto-discoverable through the public
     * SDK. The Goldberg generator produces a stats.json listing them.
     */
    fun getStatInt(name: String): Int =
        if (!initialized) 0
        else try { nativeISteamUserStatsGetStatInt(name) }
             catch (_: UnsatisfiedLinkError) { 0 }

    fun getStatFloat(name: String): Float =
        if (!initialized) 0.0f
        else try { nativeISteamUserStatsGetStatFloat(name) }
             catch (_: UnsatisfiedLinkError) { 0.0f }

    fun setStatInt(name: String, data: Int): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsSetStatInt(name, data) }
             catch (_: UnsatisfiedLinkError) { false }

    fun setStatFloat(name: String, data: Float): Boolean =
        if (!initialized) false
        else try { nativeISteamUserStatsSetStatFloat(name, data) }
             catch (_: UnsatisfiedLinkError) { false }

    /**
     * Specialized stat update for "averagerate" stat type — Steam
     * computes a running average of [countThisSession] / [sessionLength].
     */
    fun updateAvgRateStat(name: String, countThisSession: Float, sessionLength: Double): Boolean =
        if (!initialized) false
        else try {
            nativeISteamUserStatsUpdateAvgRateStat(name, countThisSession, sessionLength)
        } catch (_: UnsatisfiedLinkError) { false }

    /**
     * Well-known Steam callback IDs (from the public Steamworks SDK
     * `iCallback` enum bases) used with [awaitCallback]. Add more here
     * as the hybrid backend port consumes them.
     */
    object Callback {
        // k_iSteamUserCallbacks = 100
        const val EncryptedAppTicketResponse  = 154

        // k_iSteamUtilsCallbacks = 700
        const val SteamAPICallCompleted       = 703

        // k_iSteamFriendsCallbacks = 300
        const val PersonaStateChange          = 304
        const val GameOverlayActivated        = 331

        // k_iSteamAppsCallbacks = 1000
        const val SteamAppInstalled           = 1041
        const val SteamAppUninstalled         = 1042

        // k_iSteamUserStatsCallbacks = 1100
        const val UserStatsReceived           = 1101
        const val UserStatsStored             = 1102
        const val UserAchievementStored       = 1103

        // k_iSteamRemoteStorageCallbacks = 1300
        const val RemoteStorageFileShareResult            = 1307
        const val RemoteStorageSubscribePublishedFileResult = 1314
        const val RemoteStorageDownloadUGCResult          = 1317
    }

    /**
     * Subscribe the pump to capture payloads for [callbackId]. Until
     * unsubscribed, every callback of that id has its raw param bytes
     * stashed in g_state, ready for [awaitCallback]. Idempotent.
     */
    fun subscribeCallback(callbackId: Int) {
        if (!initialized) return
        try { nativeSubscribeCallback(callbackId) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /** Stop capturing [callbackId] and drop any pending stashed payload. */
    fun unsubscribeCallback(callbackId: Int) {
        if (!initialized) return
        try { nativeUnsubscribeCallback(callbackId) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Hybrid stage-2 — ISteamFriends EFriendFlags bits used by
     * [friendCount] / [listFriends]. `Immediate` = your friend list;
     * `All` = every friend-ish relationship libsteamclient.so tracks
     * (blocked, requested, etc. included).
     */
    object FriendFlags {
        const val None              = 0x00
        const val Blocked           = 0x01
        const val FriendshipRequested = 0x02
        const val Immediate         = 0x04
        const val Clan              = 0x08
        const val OnGameServer      = 0x10
        const val RequestingFriendship = 0x80
        const val RequestingInfo    = 0x100
        const val Ignored           = 0x200
        const val IgnoredFriend     = 0x400
        const val ChatMember        = 0x1000
        const val All               = 0xFFFF
    }

    /**
     * Hybrid stage-2 — ISteamFriends accessors. Auth-gated through the
     * cached pointer (set during bootstrap stage-2 self-test). The
     * SteamService persona/friends surface (setPersonaState,
     * getSelfPersona, requestUserPersona) routes through these to
     * replace wn-steam-client's CM persona ops.
     */
    fun personaName(): String? =
        if (!initialized) null
        else try { nativeISteamFriendsGetPersonaName() }
             catch (_: UnsatisfiedLinkError) { null }

    /** EPersonaState (0=Offline 1=Online 2=Busy 3=Away 4=Snooze 5=LookingToTrade 6=LookingToPlay 7=Invisible). */
    fun personaState(): Int =
        if (!initialized) 0
        else try { nativeISteamFriendsGetPersonaState() }
             catch (_: UnsatisfiedLinkError) { 0 }

    /**
     * Diagnostic — drives ISteamUser.RequestEncryptedAppTicket (slot 21)
     * followed by GetEncryptedAppTicket (slot 22) on the open-source
     * libsteamclient.so's vtable. Returns the (hCall, body) pair from
     * the synthetic-ticket round-trip. Verification only — game-side
     * paths go through the SDK's flat-C exports instead.
     */
    fun diagnosticRequestEncryptedAppTicket(): Pair<Long, ByteArray> {
        val out = ByteArray(128)
        val h = try {
            com.winlator.cmod.feature.stores.steam.wnsteam.WnLibSteamClient
                .nativeDiagnosticRequestEncryptedAppTicket(out)
        } catch (_: UnsatisfiedLinkError) { 0L }
        return h to out
    }

    fun friendCount(flags: Int = FriendFlags.Immediate): Int =
        if (!initialized) 0
        else try { nativeISteamFriendsGetFriendCount(flags) }
             catch (_: UnsatisfiedLinkError) { 0 }

    /** SteamID64 list. Empty when the session isn't authenticated. */
    fun listFriends(flags: Int = FriendFlags.Immediate): LongArray =
        if (!initialized) LongArray(0)
        else try { nativeISteamFriendsListFriends(flags) }
             catch (_: UnsatisfiedLinkError) { LongArray(0) }

    fun friendPersonaName(steamId: Long): String? =
        if (!initialized) null
        else try { nativeISteamFriendsGetFriendPersonaName(steamId) }
             catch (_: UnsatisfiedLinkError) { null }

    fun friendPersonaState(steamId: Long): Int =
        if (!initialized) 0
        else try { nativeISteamFriendsGetFriendPersonaState(steamId) }
             catch (_: UnsatisfiedLinkError) { 0 }

    /**
     * Block up to [timeoutMs] for the next instance of [callbackId] and
     * return its raw param bytes. Null on timeout. Auto-subscribes the
     * id; subscription persists so a subsequent await sees the next
     * callback. The bytes are the m_pubParam payload as defined by the
     * SDK callback struct for that id (e.g. UserStatsReceived_t for
     * id 1101 — caller can parse with a ByteBuffer / little-endian).
     *
     * Designed to be wrapped in a coroutine via withContext(IO) — the
     * call is synchronous + blocking so it MUST be off the main thread.
     */
    fun awaitCallback(callbackId: Int, timeoutMs: Int): ByteArray? {
        if (!initialized) return null
        return try {
            nativeAwaitCallback(callbackId, timeoutMs)
        } catch (_: UnsatisfiedLinkError) {
            null
        }
    }
}
