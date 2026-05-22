package com.winlator.cmod.feature.stores.steam.wnsteam

import android.util.Log
import org.json.JSONArray

/**
 * Kotlin facade over `libsteamclient.so` — the open-source binary we
 * build from `app/src/main/cpp/wn-libsteamclient/`. Provides setters
 * that push fresh session state INTO the .so so its interface stubs
 * (`ISteamUser.GetSteamID`, `ISteamFriends.GetPersonaName`, etc.)
 * return real values to callers without requiring deep RE or a
 * separate IPC layer.
 *
 * Loaded by [System.loadLibrary] at object init. The .so is also
 * dlopen'd later by the bootstrap (via its explicit
 * `nativeInit(libPath)` path), but this loader gives Kotlin a way to
 * reach the setters directly — those don't go through the bootstrap
 * path.
 */
object WnLibSteamClient {
    private const val TAG = "WnLibSteamClient"

    @Volatile private var loaded = false

    /**
     * Load `libsteamclient.so` via System.loadLibrary, which resolves to
     * the APK's nativeLibraryDir. This decouples Kotlin's JNI surface from
     * whatever binary sits at `imagefs/usr/lib/libsteamclient.so`:
     *
     *   - APK lib  (System.loadLibrary)       — OUR build, holds the JNI
     *                                            symbols Kotlin calls.
     *   - imagefs lib (bootstrap's explicit
     *      dlopen, wine PE bridge's dlopen)   — can be either OUR build
     *                                            or Valve's bionic build.
     *
     * The original "two mappings" concern (2026-05-19) was about setters
     * and getters going to different singletons. That mattered when the
     * wine PE bridge also dispatched through OUR binary at imagefs — both
     * sides needed to see the same in-process state. In the Valve-binary
     * setup the wine bridge talks to Valve's binary (which has its own CM
     * connection and real state), and our APK lib is only the JNI host
     * for the app's Steam tab UI — no shared-state contract.
     *
     * Setters that push state still go to OUR APK lib's singletons; Steam
     * tab UI reads from those. Game-side queries go through wine bridge to
     * Valve's binary and never touch our APK lib.
     */
    fun ensureLoaded(context: android.content.Context): Boolean {
        if (loaded) return true
        return try {
            System.loadLibrary("steamclient")
            loaded = true
            Log.i(TAG, "libsteamclient.so loaded via System.loadLibrary (APK nativeLibraryDir)")
            true
        } catch (t: UnsatisfiedLinkError) {
            Log.w(TAG, "System.loadLibrary(\"steamclient\") failed: ${t.message}; setters will no-op")
            false
        }
    }

    /** Push the current user's full SteamID64. Accessible to ISteamUser.GetSteamID. */
    fun setSteamId(steamId64: Long) {
        if (!loaded) return
        try { nativeSetSteamId(steamId64) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Tell the .so the session is (or isn't) logged on. */
    fun setLoggedOn(loggedOn: Boolean) {
        if (!loaded) return
        try { nativeSetLoggedOn(loggedOn) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Display name returned by ISteamFriends.GetPersonaName. */
    fun setPersonaName(name: String?) {
        if (!loaded) return
        try { nativeSetPersonaName(name.orEmpty()) } catch (_: UnsatisfiedLinkError) {}
    }

    /** EPersonaState (0=Offline 1=Online 2=Busy 3=Away 4=Snooze 5=LookingToTrade 6=LookingToPlay 7=Invisible). */
    fun setPersonaState(state: Int) {
        if (!loaded) return
        try { nativeSetPersonaState(state) } catch (_: UnsatisfiedLinkError) {}
    }

    /** AppID the .so reports for ISteamUtils.GetAppID. */
    fun setAppId(appId: Int) {
        if (!loaded) return
        try { nativeSetAppId(appId) } catch (_: UnsatisfiedLinkError) {}
    }

    /** ISO 3166-1 alpha-2 (e.g. "US"). */
    fun setIPCountry(country: String?) {
        if (!loaded) return
        try { nativeSetIPCountry(country.orEmpty()) } catch (_: UnsatisfiedLinkError) {}
    }

    /** UI language ("english", "spanish", …). */
    fun setUiLanguage(language: String?) {
        if (!loaded) return
        try { nativeSetUiLanguage(language.orEmpty()) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push everything we know at app-startup time — even before a Steam
     * sign-in. Lets `ISteamUtils.GetAppID` / `ISteamFriends.GetPersonaName`
     * / etc. return meaningful values from cached PrefManager state
     * immediately after the library loads, rather than waiting for the
     * full sign-in round-trip. Safe to call multiple times (idempotent
     * — each setter is a single atomic write).
     *
     * Per-game appId is pushed separately at game-launch time
     * ([WnSteamSession]'s bionic setup path is the natural call site);
     * this seeding leaves it at 0 (library/launcher mode).
     */
    fun seedFromPrefManager(context: android.content.Context) {
        if (!ensureLoaded(context)) return
        try {
            // Resolve the SteamID64 — prefer the cached value, but fall
            // back to decoding the JWT `sub` claim out of the stored
            // refresh token if the cached value is 0. This recovers
            // pre-fix installs where the user signed in BEFORE the
            // sign-in path learned to persist steamUserSteamId64 — they
            // shouldn't have to re-sign-in just because their cache row
            // is empty. Also self-heals from any future regression
            // that clears one field without the other.
            var sid = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.steamUserSteamId64
            if (sid == 0L) {
                val rt = com.winlator.cmod.feature.stores.steam.utils
                    .PrefManager.refreshToken
                if (rt.isNotBlank()) {
                    runCatching {
                        val sub = com.auth0.android.jwt.JWT(rt).subject
                        sub?.toLongOrNull()?.let { decoded ->
                            if (decoded != 0L) {
                                sid = decoded
                                com.winlator.cmod.feature.stores.steam.utils
                                    .PrefManager.steamUserSteamId64 = decoded
                                com.winlator.cmod.feature.stores.steam.utils
                                    .PrefManager.steamUserAccountId =
                                    (decoded and 0xFFFFFFFFL).toInt()
                                Log.i(TAG, "seedFromPrefManager: recovered " +
                                    "steamId64=$decoded from refresh-token JWT")
                            }
                        }
                    }.onFailure {
                        Log.w(TAG, "seedFromPrefManager: JWT decode for " +
                            "steamId recovery failed: ${it.message}")
                    }
                }
            }
            if (sid != 0L) {
                nativeSetSteamId(sid)
                // Mark logged-on at warmup so BLoggedOn returns true
                // immediately. Real Steam Client behavior: once you've
                // signed in once, the client reports "online" even while
                // the CM connection is reconnecting in the background.
                // Games that gate startup on BLoggedOn (most of them)
                // proceed past the splash without us having to wait for
                // a full CM round-trip. The onWnDisconnected path can
                // flip this back to false if the CM reconnect ultimately
                // fails.
                nativeSetLoggedOn(true)
            }
            val name = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.steamUserName
            if (name.isNotEmpty()) nativeSetPersonaName(name)
            val pstate = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.personaState
            if (pstate != 0) nativeSetPersonaState(pstate)
            val lang = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.containerLanguage
            if (lang.isNotEmpty()) nativeSetUiLanguage(lang)
            // If we have a cached SteamID, the user has signed in at
            // least once — Steam Cloud is on by default for all signed-
            // in accounts, so report account-level Cloud as enabled.
            // Per-app Cloud stays off until a game-launch path flips it.
            if (sid != 0L) nativeSetCloudEnabledForAccount(true)
            // Self-avatar warmup. If PrefManager has a cached hash from
            // a previous session, hex-decode + mirror it + kick off a
            // background CDN fetch so ColdClient/Unpack/DRM-injection
            // paths see a populated avatar even before wn-session
            // reconnects. The hash setter dedups by content; if
            // wn-session later receives the same hash, no re-push
            // happens. If the hash has changed since last boot,
            // wn-session's fresher value displaces this one.
            val hashHex = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.steamUserAvatarHash
            if (sid != 0L && hashHex.isNotEmpty()
                    && hashHex.length % 2 == 0
                    && hashHex.all { it in '0'..'9' || it in 'a'..'f' }) {
                val bytes = ByteArray(hashHex.length / 2)
                for (k in bytes.indices) {
                    val hi = Character.digit(hashHex[k * 2], 16)
                    val lo = Character.digit(hashHex[k * 2 + 1], 16)
                    bytes[k] = ((hi shl 4) or lo).toByte()
                }
                try { nativeSetFriendAvatarHash(sid, bytes) } catch (_: UnsatisfiedLinkError) {}
                // Prefetch all three tiers — overlay typically renders
                // medium in the list + large in profile popup.
                AvatarFetcher.enqueueAllTiers(sid, hashHex)
                Log.i(TAG, "seedFromPrefManager: warmed self avatar sid=$sid hash=$hashHex")
            } else {
                Log.i(TAG, "seedFromPrefManager: name='$name' sid=$sid pstate=$pstate lang=$lang " +
                    "(no avatar hash cached)")
            }
        } catch (_: UnsatisfiedLinkError) {}
        // Friend snapshot replay. Independent try block — a malformed
        // friends_snapshot_json shouldn't poison the self-side seed
        // that succeeded above.
        try {
            val friendsJson = com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.friendsSnapshotJson
            if (friendsJson.isNotEmpty()) {
                val pushed = pushFriendPersonasJson(friendsJson, persistSnapshot = false)
                Log.i(TAG, "seedFromPrefManager: replayed $pushed friend persona(s) from snapshot")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "seedFromPrefManager: friend snapshot replay failed: ${t.message}")
        }
    }

    /**
     * Push a friend-persona JSON array into the .so. Same shape
     * wn-session's `nativeGetFriendPersonas` returns: each element
     * carries `{sid:Long, name:String, state:Int, app:Int,
     * avatarHash:String(hex)}`. Each non-empty field is routed to its
     * dedicated setter; the avatar hash also kicks an
     * [AvatarFetcher.enqueue] so games see real bytes for friend
     * avatars on the warm path.
     *
     * When [persistSnapshot] is true (default), the JSON is mirrored
     * to [com.winlator.cmod.feature.stores.steam.utils.PrefManager
     * .friendsSnapshotJson] so the next cold-boot's
     * [seedFromPrefManager] replays it. The warmup path passes
     * `persistSnapshot=false` to avoid a self-write loop.
     *
     * Returns the number of personas (entries with non-zero sid)
     * pushed — used by callers for logging.
     */
    fun pushFriendPersonasJson(json: String, persistSnapshot: Boolean = true): Int {
        if (!loaded) return 0
        val arr = try { JSONArray(json) } catch (_: Exception) { return 0 }
        if (arr.length() == 0) return 0
        val sids = ArrayList<Long>(arr.length())
        for (i in 0 until arr.length()) {
            val obj = arr.optJSONObject(i) ?: continue
            val sid = obj.optLong("sid", 0L)
            if (sid == 0L) continue
            sids.add(sid)

            obj.optString("name", "").takeIf { it.isNotEmpty() }?.let {
                setFriendPersonaName(sid, it)
            }
            obj.optInt("state", -1).takeIf { it >= 0 }?.let {
                setFriendPersonaState(sid, it)
            }
            obj.optInt("app", -1).takeIf { it >= 0 }?.let {
                setFriendGamePlayed(sid, it)
            }
            val hashHex = obj.optString("avatarHash", "")
            if (hashHex.isNotEmpty() && hashHex.length % 2 == 0
                    && hashHex.all { c -> c in '0'..'9' || c in 'a'..'f' }) {
                val bytes = ByteArray(hashHex.length / 2)
                for (k in bytes.indices) {
                    val hi = Character.digit(hashHex[k * 2], 16)
                    val lo = Character.digit(hashHex[k * 2 + 1], 16)
                    bytes[k] = ((hi shl 4) or lo).toByte()
                }
                setFriendAvatarHash(sid, bytes)
                // Prefetch all three tiers (small/medium/large) so
                // games' GetMediumFriendAvatar / GetLargeFriendAvatar
                // also resolve, not just GetSmallFriendAvatar.
                AvatarFetcher.enqueueAllTiers(sid, hashHex)
            }
        }
        if (sids.isNotEmpty()) {
            setFriendsList(sids.toLongArray())
        }
        if (persistSnapshot && sids.isNotEmpty()) {
            com.winlator.cmod.feature.stores.steam.utils
                .PrefManager.friendsSnapshotJson = json
        }
        return sids.size
    }

    /** Push the list of owned AppIDs (full set; replaces prior). */
    fun setOwnedApps(appIds: IntArray) {
        if (!loaded) return
        try { nativeSetOwnedApps(appIds) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Push the list of locally-installed AppIDs. */
    fun setInstalledApps(appIds: IntArray) {
        if (!loaded) return
        try { nativeSetInstalledApps(appIds) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Per-app install directory (filesystem path). Empty string clears. */
    fun setAppInstallDir(appId: Int, dir: String?) {
        if (!loaded) return
        try { nativeSetAppInstallDir(appId, dir.orEmpty()) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the per-app DLC list. Three parallel arrays — dlcAppIds[i]
     * paired with dlcNames[i] (empty for unresolved) and available[i]
     * (whether the DLC is currently purchasable; default true). Replaces
     * the entry for [parentAppId]; empty list clears it.
     */
    fun setAppDlcs(
        parentAppId: Int,
        dlcAppIds: IntArray,
        dlcNames: Array<String>,
        available: BooleanArray,
    ) {
        if (!loaded || parentAppId <= 0) return
        if (dlcAppIds.size != dlcNames.size || dlcAppIds.size != available.size) return
        try { nativeSetAppDlcs(parentAppId, dlcAppIds, dlcNames, available) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /** Per-app installed depot ids. Empty / null clears the entry. */
    fun setAppInstalledDepots(appId: Int, depotIds: IntArray?) {
        if (!loaded || appId <= 0) return
        try { nativeSetAppInstalledDepots(appId, depotIds) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the per-app subscribed-and-installed Workshop item list.
     * Powers ISteamUGC slots 70-75 (GetNumSubscribedItems, GetSubscribed
     * Items, GetItemState, GetItemInstallInfo, GetItemDownloadInfo,
     * DownloadItem) for Bionic mode — games can enumerate installed
     * mods at boot rather than seeing an empty Workshop folder. All four
     * parallel arrays must be the same length; empty / null clears the
     * entry. Source data is WorkshopModsGenerator's staging dir
     * (installedItemIds + contentDir + metaFile).
     */
    fun setAppWorkshopItems(
        appId: Int,
        publishedFileIds: LongArray,
        installDirs: Array<String>,
        sizesBytes: LongArray,
        timestamps: LongArray,
    ) {
        if (!loaded || appId <= 0) return
        if (publishedFileIds.size != installDirs.size ||
            publishedFileIds.size != sizesBytes.size ||
            publishedFileIds.size != timestamps.size) return
        try { nativeSetAppWorkshopItems(appId, publishedFileIds, installDirs, sizesBytes, timestamps) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the per-app ISteamInventory item-definition catalog. Schema:
     * defIds[N] paired with propCountsPerDef[N]; per-def property
     * entries are concatenated into propKeys / propVals in defIds
     * order (sum of propCountsPerDef must equal propKeys.size and
     * propVals.size). Replaces the pushed entry for [appId]; empty
     * defIds clears it. Powers ISteamInventory slots 20 LoadItem
     * Definitions, 21 GetItemDefinitionIDs, 22 GetItemDefinitionProperty.
     * Source data is the same items.json archive InventoryItemsGenerator
     * already produces for ColdClient's `steam_settings/items.json`.
     */
    fun setInventoryItemDefs(
        appId: Int,
        defIds: IntArray,
        propCountsPerDef: IntArray,
        propKeys: Array<String>,
        propVals: Array<String>,
    ) {
        if (!loaded || appId <= 0) return
        if (defIds.size != propCountsPerDef.size) return
        if (propKeys.size != propVals.size) return
        if (propCountsPerDef.sum() != propKeys.size) return
        try { nativeSetInventoryItemDefs(appId, defIds, propCountsPerDef, propKeys, propVals) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Bulk push per-app human-readable names. [appIds] paired with
     * [names]; arrays must be the same length. Replaces / inserts
     * entries individually (does NOT clear other apps). Empty name
     * clears that entry. Powers ISteamAppList.GetAppName.
     */
    fun setAppNames(appIds: IntArray, names: Array<String>) {
        if (!loaded || appIds.size != names.size) return
        if (appIds.isEmpty()) return
        try { nativeSetAppNames(appIds, names) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Per-app PICS public-branch buildid. Surfaces through
     * ISteamApps.GetAppBuildId. [buildId] <= 0 clears the entry.
     */
    fun setAppBuildId(appId: Int, buildId: Int) {
        if (!loaded || appId <= 0) return
        try { nativeSetAppBuildId(appId, buildId) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Friends-list (full set). */
    fun setFriendsList(steamIds: LongArray) {
        if (!loaded) return
        try { nativeSetFriendsList(steamIds) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Per-friend persona name (empty clears). */
    fun setFriendPersonaName(steamId64: Long, name: String?) {
        if (!loaded) return
        try { nativeSetFriendPersonaName(steamId64, name.orEmpty()) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Per-friend EPersonaState (0=Offline 1=Online 2=Busy 3=Away 4=Snooze
     * 5=LookingToTrade 6=LookingToPlay 7=Invisible). Negative clears the
     * entry. Surfaces through ISteamFriends.GetFriendPersonaState.
     */
    fun setFriendPersonaState(steamId64: Long, state: Int) {
        if (!loaded) return
        try { nativeSetFriendPersonaState(steamId64, state) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Per-friend currently-played appId. 0 / negative clears the entry.
     * Surfaces through ISteamFriends.GetFriendGamePlayed.
     */
    fun setFriendGamePlayed(steamId64: Long, appId: Int) {
        if (!loaded) return
        try { nativeSetFriendGamePlayed(steamId64, appId) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Push Steam server realtime (unix seconds). 0 falls back to local clock. */
    fun setServerRealTime(serverRealTimeUnix: Int) {
        if (!loaded) return
        try { nativeSetServerRealTime(serverRealTimeUnix) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the bound app's command-line argv (joined with spaces).
     * Surfaces through ISteamApps.GetLaunchCommandLine. Empty string
     * clears the value. The launch context calls this once per game
     * launch after the args are assembled.
     */
    fun setLaunchCommandLine(cli: String?) {
        if (!loaded) return
        try { nativeSetLaunchCommandLine(cli.orEmpty()) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push whether the bound app is family-shared. Surfaces through
     * ISteamApps.BIsSubscribedFromFamilySharing. The launch context
     * checks the user's familyGroupMembers + app ownership chain and
     * flips this accordingly.
     */
    fun setAppFamilyShared(familyShared: Boolean) {
        if (!loaded) return
        try { nativeSetAppFamilyShared(familyShared) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push real encrypted-app-ticket bytes from a wn-session
     * CMsgClientRequestEncryptedAppTicket response into our .so's
     * per-app cache. Subsequent ISteamUser.RequestEncryptedAppTicket
     * / GetEncryptedAppTicket calls serve these bytes verbatim
     * (instead of the synthetic "WNETKT" placeholder).
     *
     * Empty / null [body] clears the cache entry (forces the next
     * fetch to fall back to synthetic).
     *
     * @param eresult Steamworks EResult (1 = OK, 2 = Fail, etc.).
     */
    fun setEncryptedAppTicket(appId: Int, body: ByteArray?, eresult: Int) {
        if (!loaded) return
        try { nativeSetEncryptedAppTicket(appId, body, eresult) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Surface a Steam logon failure to games observing the .so. Emits
     * SteamServerConnectFailure_t (id 102). [eresult] is the
     * Steamworks-SDK EResult (1 OK, 5 InvalidPassword, 15 AccessDenied,
     * 84 RateLimit, etc.). [stillRetrying] tells games whether to
     * keep their reconnect UI active or surface a hard failure.
     */
    fun reportLogonFailure(eresult: Int, stillRetrying: Boolean) {
        if (!loaded) return
        try { nativeReportLogonFailure(eresult, stillRetrying) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /** Account-level Steam Cloud enablement (ISteamRemoteStorage.IsCloudEnabledForAccount). */
    fun setCloudEnabledForAccount(enabled: Boolean) {
        if (!loaded) return
        try { nativeSetCloudEnabledForAccount(enabled) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Per-app Steam Cloud enablement for the currently-bound game. */
    fun setCloudEnabledForApp(enabled: Boolean) {
        if (!loaded) return
        try { nativeSetCloudEnabledForApp(enabled) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Cloud quota in bytes: total budget + currently-available remaining. */
    fun setCloudQuota(totalBytes: Long, availableBytes: Long) {
        if (!loaded) return
        try { nativeSetCloudQuota(totalBytes, availableBytes) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the cloud-file list. The three arrays must be the same length —
     * names[i] paired with sizes[i] (bytes) and timestamps[i] (unix seconds).
     * Replaces the entire pushed list; empty arrays clear it.
     */
    fun setCloudFiles(names: Array<String>, sizes: IntArray, timestamps: LongArray) {
        if (!loaded) return
        if (names.size != sizes.size || names.size != timestamps.size) return
        try { nativeSetCloudFiles(names, sizes, timestamps) } catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the achievement schema for the currently-bound app. Five
     * parallel arrays — apiNames[i] paired with displayNames[i],
     * descriptions[i], icons[i] (URL or empty), hidden[i]. Replaces the
     * entire schema; empty/null clears it. Marks the stats cache as
     * ready so [requestCurrentStats] returns true.
     */
    fun setAchievementSchema(
        apiNames: Array<String>,
        displayNames: Array<String>,
        descriptions: Array<String>,
        icons: Array<String>,
        hidden: BooleanArray,
    ) {
        if (!loaded) return
        if (apiNames.size != displayNames.size ||
            apiNames.size != descriptions.size ||
            apiNames.size != icons.size ||
            apiNames.size != hidden.size) return
        try {
            nativeSetAchievementSchema(apiNames, displayNames, descriptions, icons, hidden)
        } catch (_: UnsatisfiedLinkError) {}
    }

    /** Update one achievement's (achieved, unlockTime) tuple. No-op if not in the schema. */
    fun setAchievementProgress(apiName: String, achieved: Boolean, unlockTimeUnix: Int) {
        if (!loaded || apiName.isEmpty()) return
        try { nativeSetAchievementProgress(apiName, achieved, unlockTimeUnix) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Layer the bit-pack mapping (block_id + bit_index per achievement)
     * onto an already-pushed schema. Parallel arrays. Call AFTER
     * setAchievementSchema. Powers ISteamUserStats.StoreStats's
     * CMsgClientStoreUserStats2 path.
     */
    fun setAchievementBlockBits(
        apiNames: Array<String>,
        blockIds: IntArray,
        bitIndices: IntArray,
    ) {
        if (!loaded) return
        if (apiNames.size != blockIds.size || apiNames.size != bitIndices.size) return
        try { nativeSetAchievementBlockBits(apiNames, blockIds, bitIndices) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Push the name→numeric-id mapping for stats. Required for
     * SetStatInt/Float to upload to Steam on the next StoreStats —
     * without this, dirty stats are skipped from the upload (no
     * stat_id resolution).
     */
    fun setStatIds(names: Array<String>, ids: IntArray) {
        if (!loaded) return
        if (names.size != ids.size) return
        try { nativeSetStatIds(names, ids) }
        catch (_: UnsatisfiedLinkError) {}
    }

    /**
     * Layer a per-locale display string + description onto an achievement
     * already in the schema (no-op when the api name isn't found yet).
     * Call once per (achievement, locale). Empty strings skip the
     * corresponding map insert — Steam schemas often have partial
     * localizations (display_name but no description in some locales).
     */
    fun addAchievementLocale(apiName: String, locale: String,
                             displayName: String?, description: String?) {
        if (!loaded || apiName.isEmpty() || locale.isEmpty()) return
        if (displayName.isNullOrEmpty() && description.isNullOrEmpty()) return
        try {
            nativeAddAchievementLocale(apiName, locale,
                displayName.orEmpty(), description.orEmpty())
        } catch (_: UnsatisfiedLinkError) {}
    }

    /** Set a per-stat int32 value (ISteamUserStats.GetStatInt readback). */
    fun setStatInt(name: String, value: Int) {
        if (!loaded || name.isEmpty()) return
        try { nativeSetStatInt(name, value) } catch (_: UnsatisfiedLinkError) {}
    }

    /** Set a per-stat float value (ISteamUserStats.GetStatFloat readback). */
    fun setStatFloat(name: String, value: Float) {
        if (!loaded || name.isEmpty()) return
        try { nativeSetStatFloat(name, value) } catch (_: UnsatisfiedLinkError) {}
    }

    @JvmStatic private external fun nativeSetSteamId(steamId64: Long)
    @JvmStatic private external fun nativeSetLoggedOn(loggedOn: Boolean)
    @JvmStatic private external fun nativeSetPersonaName(name: String)
    @JvmStatic private external fun nativeSetPersonaState(state: Int)
    @JvmStatic private external fun nativeSetAppId(appId: Int)
    @JvmStatic private external fun nativeSetIPCountry(country: String)
    @JvmStatic private external fun nativeSetUiLanguage(language: String)
    @JvmStatic private external fun nativeSetOwnedApps(appIds: IntArray)
    @JvmStatic private external fun nativeSetInstalledApps(appIds: IntArray)
    @JvmStatic private external fun nativeSetAppInstallDir(appId: Int, dir: String)
    @JvmStatic private external fun nativeSetAppDlcs(
        parentAppId: Int, dlcAppIds: IntArray, dlcNames: Array<String>,
        available: BooleanArray)
    @JvmStatic private external fun nativeSetAppInstalledDepots(appId: Int, depotIds: IntArray?)
    @JvmStatic private external fun nativeSetAppWorkshopItems(
        appId: Int,
        publishedFileIds: LongArray,
        installDirs: Array<String>,
        sizesBytes: LongArray,
        timestamps: LongArray,
    )
    @JvmStatic private external fun nativeSetInventoryItemDefs(
        appId: Int,
        defIds: IntArray,
        propCountsPerDef: IntArray,
        propKeys: Array<String>,
        propVals: Array<String>,
    )
    @JvmStatic private external fun nativeSetAppNames(appIds: IntArray, names: Array<String>)
    @JvmStatic private external fun nativeSetAppBuildId(appId: Int, buildId: Int)
    @JvmStatic private external fun nativeSetFriendsList(steamIds: LongArray)
    @JvmStatic private external fun nativeSetFriendPersonaName(steamId64: Long, name: String)
    @JvmStatic private external fun nativeSetFriendPersonaState(steamId64: Long, state: Int)
    @JvmStatic private external fun nativeSetFriendGamePlayed(steamId64: Long, appId: Int)
    @JvmStatic private external fun nativeSetServerRealTime(serverRealTimeUnix: Int)
    @JvmStatic private external fun nativeReportLogonFailure(eresult: Int, stillRetrying: Boolean)
    @JvmStatic private external fun nativeSetEncryptedAppTicket(appId: Int, body: ByteArray?, eresult: Int)
    @JvmStatic private external fun nativeSetLaunchCommandLine(cli: String)
    @JvmStatic private external fun nativeSetAppFamilyShared(familyShared: Boolean)
    @JvmStatic private external fun nativeSetCloudEnabledForAccount(enabled: Boolean)
    @JvmStatic private external fun nativeSetCloudEnabledForApp(enabled: Boolean)
    @JvmStatic private external fun nativeSetCloudQuota(totalBytes: Long, availBytes: Long)
    @JvmStatic private external fun nativeSetCloudFiles(
        names: Array<String>, sizes: IntArray, timestamps: LongArray)
    @JvmStatic private external fun nativeSetAchievementSchema(
        apiNames: Array<String>, displayNames: Array<String>,
        descriptions: Array<String>, icons: Array<String>, hidden: BooleanArray)
    @JvmStatic private external fun nativeSetAchievementBlockBits(
        apiNames: Array<String>, blockIds: IntArray, bitIndices: IntArray)
    @JvmStatic private external fun nativeSetStatIds(names: Array<String>, ids: IntArray)
    @JvmStatic private external fun nativeSetAchievementProgress(
        apiName: String, achieved: Boolean, unlockTimeUnix: Int)
    @JvmStatic private external fun nativeAddAchievementLocale(
        apiName: String, locale: String, displayName: String, description: String)
    @JvmStatic private external fun nativeSetStatInt(name: String, value: Int)
    @JvmStatic private external fun nativeSetStatFloat(name: String, value: Float)
    /** Diagnostic — reads pushed().achievements.size() directly, no vtable. */
    @JvmStatic external fun nativeDiagnosticAchievementCount(): Int
    /** Diagnostic — queue depth, non-destructive (does not consume). */
    @JvmStatic external fun nativeDiagnosticCallbackDepth(): Int
    /** Diagnostic — invokes the .so's ISteamUserStats::StoreStats. */
    @JvmStatic external fun nativeDiagnosticStoreStats(): Boolean
    /**
     * Diagnostic — invokes the .so's ISteamUserStats::SetAchievement
     * via vtable. Returns true on a known achievement that wasn't
     * already unlocked (false otherwise — name unknown or already
     * achieved).  Marks the entry pending_store so the next StoreStats
     * emits UserAchievementStored_t for it.
     */
    @JvmStatic external fun nativeDiagnosticSetAchievement(name: String): Boolean
    /**
     * Diagnostic — invokes ISteamUserStats::IndicateAchievementProgress
     * via vtable. Emits UserAchievementStored_t with the cur/max
     * progress fields filled. Returns true on a known, not-yet-unlocked
     * achievement.
     */
    @JvmStatic external fun nativeDiagnosticIndicateAchievementProgress(
        name: String, current: Int, max: Int): Boolean
    /**
     * Wn-session-side push of a remote friend's rich-presence (key,
     * value). Called when CMsgClientPersonaState delivers RP for a
     * peer. Empty/null value REMOVES the key (matches SDK
     * SetRichPresence semantics). Emits FriendRichPresenceUpdate_t.
     */
    @JvmStatic external fun nativeSetFriendRichPresence(
        steamId: Long, key: String, value: String?)
    /** Diagnostic — invokes ISteamFriends::SetRichPresence (slot 43) via vtable. */
    @JvmStatic external fun nativeDiagnosticSetRichPresence(
        key: String, value: String?): Boolean
    /** Diagnostic — invokes ISteamFriends::ClearRichPresence (slot 44) via vtable. */
    @JvmStatic external fun nativeDiagnosticClearRichPresence()
    /** Diagnostic — invokes ISteamFriends::GetFriendRichPresenceKeyCount (slot 46) via vtable. */
    @JvmStatic external fun nativeDiagnosticRichPresenceKeyCount(steamId: Long): Int
    /**
     * Diagnostic — invokes ISteamFriends::SetPersonaName (slot 1) via
     * vtable. Returns the SteamAPICall_t hCall the stub allocated.
     * Exercises the full stub path: pushed-state cache update +
     * PersonaStateChange_t emit + cm_bridge CMsgClientChangeStatus
     * dispatch + SetPersonaNameResponse_t CallResult schedule.
     */
    @JvmStatic external fun nativeDiagnosticSetPersonaName(name: String): Long
    /** Diagnostic — invokes ISteamFriends.RequestUserInformation (slot 37) via vtable. */
    @JvmStatic external fun nativeDiagnosticRequestUserInformation(steamId: Long, nameOnly: Boolean): Boolean
    /** Diagnostic — invokes ISteamFriends.RequestFriendRichPresence (slot 48) via vtable. */
    @JvmStatic external fun nativeDiagnosticRequestFriendRichPresence(steamId: Long)
    /**
     * Diagnostic — drives cm_bridge.wn_cm_request_user_info_bulk for an
     * array of SteamID64s. `flags` ≤ 0 → standard set (name|state|game|
     * avatar = 0x47). Returns true if the bridge dispatched to a live
     * CMClient; false otherwise. Zero entries in the array are filtered
     * out (one CM round-trip for all valid sids).
     */
    @JvmStatic external fun nativeDiagnosticRequestUserInfoBulk(sids: LongArray, flags: Int): Boolean
    /**
     * Diagnostic — reads CMClient's cached ownership ticket for [appId].
     * Returns the ticket length (0 = cache miss, no out_buf, or invalid
     * args). When [out] is non-null and large enough, fills with the
     * ticket bytes. When [out] is null, returns size-only.
     *
     * Pre-fetch is wn-session's job — the cache is populated when
     * CMsgClientGetAppOwnershipTicketResponse arrives. Until that
     * happens (or after the ticket expires), this returns 0.
     */
    @JvmStatic external fun nativeDiagnosticGetCachedOwnershipTicket(
        appId: Int, out: ByteArray?): Int
    /**
     * Diagnostic — injects synthetic ownership-ticket bytes into
     * CMClient's WnTicketCache for [appId]. Lets test paths exercise
     * the cache-hit branch of GetAuthSessionTicket without a real CM
     * round-trip. Returns true on success.
     */
    @JvmStatic external fun nativeDiagnosticInjectOwnershipTicket(
        appId: Int, bytes: ByteArray): Boolean
    /**
     * Diagnostic — synthetically fires the cm_bridge logon-state
     * observer. Mirrors a real CMClient state transition so observer
     * dispatch + SteamServersConnected_t / Disconnected_t emission can
     * be verified without a real wn-session sign-in.
     */
    @JvmStatic external fun nativeDiagnosticInjectLogonState(loggedOn: Boolean)
    /**
     * Diagnostic — synthetically fires the cm_bridge friends-list
     * observer with the given SID array. Mirrors a real
     * CMsgClientFriendsList arrival; observer wholesale-replaces
     * pushed.friends with the array contents.
     */
    @JvmStatic external fun nativeDiagnosticInjectFriendsList(sids: LongArray?)
    /**
     * Diagnostic — synthetically fires the cm_bridge license-list
     * observer. Parallel arrays of package IDs and owner account IDs.
     * Used to verify ISteamApps.BIsSubscribedFromFamilySharing
     * resolution offline.
     */
    @JvmStatic external fun nativeDiagnosticInjectLicenseList(
        packageIds: IntArray, ownerIds: IntArray?)
    /** Diagnostic — reads pushed.licenses[pkg].owner_id; -1 = absent. */
    @JvmStatic external fun nativeDiagnosticGetLicenseOwner(packageId: Int): Int
    /**
     * Push the per-app source-package list (which packages grant
     * access to this app). Powers ISteamApps.GetEarliestPurchaseUnix
     * Time + BIsSubscribedFromFreeWeekend by joining against
     * pushed.licenses on package_id. Empty / null clears the entry.
     */
    @JvmStatic external fun nativeSetAppSourcePackages(
        appId: Int, packageIds: IntArray?)
    /** Diagnostic — invokes ISteamApps::GetEarliestPurchaseUnixTime via vtable slot 8. */
    @JvmStatic external fun nativeDiagnosticGetEarliestPurchaseUnixTime(appId: Int): Int
    /** Diagnostic — invokes ISteamApps::BIsSubscribedFromFreeWeekend via vtable slot 9 (bound app). */
    @JvmStatic external fun nativeDiagnosticBIsSubscribedFromFreeWeekend(): Boolean
    /** Diagnostic — invokes ISteamApps::BIsSubscribedFromFamilySharing via vtable slot 27. */
    @JvmStatic external fun nativeDiagnosticBIsSubscribedFromFamilySharing(): Boolean
    /** Diagnostic — invokes ISteamApps::GetAppOwner via vtable slot 20. */
    @JvmStatic external fun nativeDiagnosticGetAppOwner(): Long
    /** Diagnostic — patches/creates a license entry with trial fields. */
    @JvmStatic external fun nativeDiagnosticInjectTrialLicense(
        packageId: Int, minuteLimit: Int, minutesUsed: Int)
    /**
     * Diagnostic — invokes ISteamApps::BIsTimedTrial (slot 28) via vtable.
     * Packed: bit 63 set = true result; bits 32-62 = seconds_allowed,
     * bits 0-31 = seconds_played. Returns 0 if result is false.
     */
    @JvmStatic external fun nativeDiagnosticBIsTimedTrial(): Long
    /** Diagnostic — invokes ISteamApps::BIsDlcInstalled (slot 7). */
    @JvmStatic external fun nativeDiagnosticBIsDlcInstalled(appId: Int): Boolean
    /**
     * Push the active beta branch name for an app. null / empty clears
     * the entry (= public branch). Powers
     * ISteamApps.GetCurrentBetaName (slot 15). Called from SteamService
     * on game launch from WnLibrary's selected beta.
     */
    @JvmStatic external fun nativeSetAppCurrentBeta(appId: Int, branch: String?)

    /** Kotlin-friendly wrapper around nativeSetAppCurrentBeta. */
    fun setAppCurrentBeta(appId: Int, branch: String?) {
        if (!loaded) return
        try { nativeSetAppCurrentBeta(appId, branch) } catch (_: UnsatisfiedLinkError) {}
    }
    /** Diagnostic — invokes slot 15 + returns the written branch (or null on false). */
    @JvmStatic external fun nativeDiagnosticGetCurrentBetaName(): String?
    /**
     * Push active download progress for an app. bytesTotal ≤ 0 clears
     * the entry (treated as download-terminated). SteamService calls
     * this from depot-downloader progress callbacks. Powers
     * ISteamApps.GetDlcDownloadProgress (slot 22).
     */
    @JvmStatic external fun nativeSetAppDownloadProgress(
        appId: Int, bytesDownloaded: Long, bytesTotal: Long)
    /** Diagnostic — invokes slot 22, returns true if download active. */
    @JvmStatic external fun nativeDiagnosticGetDlcDownloadProgress(appId: Int): Boolean
    /** Read-back of bytesDownloaded from the most-recent slot-22 diagnostic call. */
    @JvmStatic external fun nativeDiagnosticGetDlcDownloadProgressBytes(): Long
    /** Read-back of bytesTotal from the most-recent slot-22 diagnostic call. */
    @JvmStatic external fun nativeDiagnosticGetDlcDownloadProgressTotal(): Long
    /**
     * Push the local "remote" cloud-mirror dir for an app (path to
     * `.../userdata/<acct>/<appId>/remote`). null / empty clears.
     * SteamService pushes this on logon for each installed app so that
     * ISteamRemoteStorage.FileWrite/FileRead/FileDelete have a target.
     */
    @JvmStatic external fun nativeSetAppCloudRemoteDir(appId: Int, path: String?)
    /** Diagnostic — invokes ISteamRemoteStorage::FileWrite (slot 0). */
    @JvmStatic external fun nativeDiagnosticCloudFileWrite(
        name: String, data: ByteArray): Boolean
    /** Diagnostic — invokes ISteamRemoteStorage::FileRead (slot 1). */
    @JvmStatic external fun nativeDiagnosticCloudFileRead(
        name: String, maxBytes: Int): ByteArray?
    /** Diagnostic — invokes ISteamRemoteStorage::FileDelete (slot 6). */
    @JvmStatic external fun nativeDiagnosticCloudFileDelete(name: String): Boolean
    /** Diagnostic — invokes FileWriteAsync (slot 2). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticCloudFileWriteAsync(
        name: String, data: ByteArray): Long
    /** Diagnostic — invokes FileReadAsync (slot 3). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticCloudFileReadAsync(
        name: String, offset: Int, cubToRead: Int): Long
    /** Diagnostic — invokes FileReadAsyncComplete (slot 4). */
    @JvmStatic external fun nativeDiagnosticCloudFileReadAsyncComplete(
        hCall: Long, cubToRead: Int): ByteArray?
    /** Diagnostic — FileWriteStreamOpen (slot 9). */
    @JvmStatic external fun nativeDiagnosticCloudStreamOpen(name: String): Long
    /** Diagnostic — FileWriteStreamWriteChunk (slot 10). */
    @JvmStatic external fun nativeDiagnosticCloudStreamWriteChunk(
        hStream: Long, data: ByteArray): Boolean
    /** Diagnostic — FileWriteStreamClose (slot 11). */
    @JvmStatic external fun nativeDiagnosticCloudStreamClose(hStream: Long): Boolean
    /** Diagnostic — FileWriteStreamCancel (slot 12). */
    @JvmStatic external fun nativeDiagnosticCloudStreamCancel(hStream: Long): Boolean
    /** flagKind=0 sets app_low_violence; flagKind=1 sets app_vac_banned. */
    @JvmStatic external fun nativeSetAppFlag(flagKind: Int, appId: Int, on: Boolean)
    /** Diagnostic — invoke ISteamApps slots 0/1/2/3 (BIs* family). */
    @JvmStatic external fun nativeDiagnosticAppsBool(slot: Int): Boolean
    /** Diagnostic — invoke ISteamApps::SetDlcContext (slot 29). */
    @JvmStatic external fun nativeDiagnosticSetDlcContext(appId: Int): Boolean
    /** Diagnostic — invoke FileForget (slot 5) on ISteamRemoteStorage. */
    @JvmStatic external fun nativeDiagnosticCloudFileForget(name: String): Boolean
    /** Diagnostic — invoke FilePersisted (slot 14) on ISteamRemoteStorage. */
    @JvmStatic external fun nativeDiagnosticCloudFilePersisted(name: String): Boolean
    /**
     * Push account-info flags. flagKind: 0=phone_verified,
     * 1=two_factor_enabled, 2=phone_identifying,
     * 3=phone_requires_verification.
     */
    @JvmStatic external fun nativeSetAccountFlag(flagKind: Int, on: Boolean)
    /** Diagnostic — invoke ISteamUser slots 26-29 (phone/2FA family). */
    @JvmStatic external fun nativeDiagnosticUserBool(slot: Int): Boolean
    /** Diagnostic — invoke BSetDurationControlOnlineState (slot 32). */
    @JvmStatic external fun nativeDiagnosticSetDurationControl(state: Int): Boolean
    /** Diagnostic — invoke GetUserDataFolder (slot 6). */
    @JvmStatic external fun nativeDiagnosticGetUserDataFolder(): String?
    /** Diagnostic — invoke ISteamFriends::GetFriendRelationship (slot 5). */
    @JvmStatic external fun nativeDiagnosticGetFriendRelationship(sid: Long): Int
    /** Diagnostic — invoke ISteamFriends::HasFriend (slot 17). */
    @JvmStatic external fun nativeDiagnosticHasFriend(sid: Long, flags: Int): Boolean
    /** Diagnostic — invoke GetAuthTicketForWebApi (slot 14). Returns hAuthTicket. */
    @JvmStatic external fun nativeDiagnosticGetAuthTicketForWebApi(identity: String?): Long
    /** Push a per-friend Steam profile XP level (level<0 clears). */
    @JvmStatic external fun nativeSetFriendSteamLevel(sid: Long, level: Int)
    /** Poll whether an app has been MarkContentCorrupt'd by the game. */
    @JvmStatic external fun nativeIsAppMarkedCorrupt(appId: Int): Boolean
    /** Clear the MarkContentCorrupt flag once the downloader picks it up. */
    @JvmStatic external fun nativeClearAppCorruptFlag(appId: Int)
    /** Diagnostic — invoke UserHasLicenseForApp (slot 18). */
    @JvmStatic external fun nativeDiagnosticUserHasLicense(sid: Long, appId: Int): Int
    /** Diagnostic — invoke MarkContentCorrupt (slot 16 ISteamApps). */
    @JvmStatic external fun nativeDiagnosticMarkContentCorrupt(missingOnly: Boolean): Boolean
    /** Diagnostic — invoke GetFriendSteamLevel (slot 10 ISteamFriends). */
    @JvmStatic external fun nativeDiagnosticGetFriendSteamLevel(sid: Long): Int
    /** Push self Steam profile XP level. level<0 clears. */
    @JvmStatic external fun nativeSetSelfPlayerLevel(level: Int)
    /** Push self per-game badge tier. tier<0 clears. */
    @JvmStatic external fun nativeSetSelfGameBadge(appId: Int, nSeries: Int, bFoil: Boolean, tier: Int)
    /** Diagnostic — GetPlayerSteamLevel (slot 24). */
    @JvmStatic external fun nativeDiagnosticGetPlayerSteamLevel(): Int
    /** Diagnostic — GetGameBadgeLevel (slot 23). */
    @JvmStatic external fun nativeDiagnosticGetGameBadgeLevel(nSeries: Int, bFoil: Boolean): Int
    /** Diagnostic — RequestStoreAuthURL (slot 25). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticRequestStoreAuthURL(redirect: String?): Long
    /** Diagnostic — GetMarketEligibility (slot 30). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticGetMarketEligibility(): Long
    /** Diagnostic — GetDurationControl (slot 31). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticGetDurationControl(): Long
    /** Diagnostic — FileShare (slot 7 ISteamRemoteStorage). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticCloudFileShare(name: String): Long
    /** Diagnostic — ISteamApps::GetFileDetails (slot 25) for binary-integrity. */
    @JvmStatic external fun nativeDiagnosticAppsGetFileDetails(name: String): Long
    /** Push a user-set nickname for a SteamID. Null/empty clears. */
    @JvmStatic external fun nativeSetPlayerNickname(sid: Long, nickname: String?)
    /** Diagnostic — GetPlayerNickname (ISteamFriends slot 11). */
    @JvmStatic external fun nativeDiagnosticGetPlayerNickname(sid: Long): String?
    /** Diagnostic — ISteamUtils::CheckFileSignature (slot 19). Returns hCall. */
    @JvmStatic external fun nativeDiagnosticCheckFileSignature(name: String?): Long
    /**
     * Direct pushed-state read-back — what libsteamclient.so itself
     * would report for each slot, bypassing the bootstrap reads. The
     * legacy state-dump op reads WnSteamBootstrap which is dormant in
     * the libsteamclient-only mode; these getters reflect the .so's
     * actual state so the diagnostic shows reality.
     */
    @JvmStatic external fun nativeGetPushedSteamId(): Long
    @JvmStatic external fun nativeGetPushedPersonaName(): String
    @JvmStatic external fun nativeGetPushedIpCountry(): String
    @JvmStatic external fun nativeGetPushedPersonaState(): Int
    @JvmStatic external fun nativeGetPushedLoggedOn(): Boolean
    @JvmStatic external fun nativeGetPushedAppId(): Int
    @JvmStatic external fun nativeGetPushedOwnedAppCount(): Int
    @JvmStatic external fun nativeGetPushedInstalledAppCount(): Int
    @JvmStatic external fun nativeGetPushedFriendCount(): Int
    @JvmStatic external fun nativeGetPushedFirstFriend(): Long
    @JvmStatic external fun nativeGetPushedUiLanguage(): String
    @JvmStatic external fun nativeGetPushedServerRealTime(): Int
    @JvmStatic external fun nativeGetPushedCloudFileCount(): Int
    @JvmStatic external fun nativeGetPushedCloudEnabledAccount(): Boolean
    @JvmStatic external fun nativeGetPushedCloudEnabledApp(): Boolean
    @JvmStatic external fun nativeGetPushedEncryptedAppTicketSize(appId: Int): Int
    /**
     * Diagnostic — synthetically dispatch the cm_bridge account-info
     * observer. Verifies the observer registration + pushed-state
     * mirror path offline (CM round-trip not required).
     */
    @JvmStatic external fun nativeDiagnosticInjectAccountInfo(
        twoFactor: Boolean, phoneVerified: Boolean,
        phoneIdentifying: Boolean, phoneNeedsVerification: Boolean)
    /** Diagnostic — invokes UpdateAvgRateStat (slot 5) then reads GetStat (float). */
    @JvmStatic external fun nativeDiagnosticUpdateAvgRateStat(
        name: String, countThisSession: Float, sessionLength: Double): Float
    /**
     * Diagnostic — synthetically fires the cm_bridge persona observer
     * with a fully-controlled payload. Mirrors a real CMsgClientPersonaState
     * arrival from Steam so the observer's pushed_state mirror +
     * PersonaStateChange_t / FriendRichPresenceUpdate_t emissions can
     * be verified without wn-session traffic.
     *
     * [personaState]: -1 = absent (UINT32_MAX in the POD); 0..7 = real
     * [gameAppId]: 0 = not in game
     * [name]: null / empty skips
     * [avatarHash]: null / empty skips
     * [rpKeys] / [rpValues]: paired arrays (same length); null skips RP
     */
    @JvmStatic external fun nativeDiagnosticInjectPersonaEvent(
        steamId: Long,
        personaState: Int,
        gameAppId: Int,
        name: String?,
        avatarHash: ByteArray?,
        rpKeys: Array<String>?,
        rpValues: Array<String>?,
    )

    /**
     * Notify the .so that the Steam-style overlay has been activated
     * or deactivated. Emits GameOverlayActivated_t (callback id 731)
     * on every state transition (no-op when the flag matches the last
     * call). Games typically gate pause/unpause on this — e.g.
     * `if (active) game.pause() else game.resume()`.
     *
     * Call sites: any in-app modal that obscures the game and should
     * trigger an SDK-level pause (in-game settings panel, IME, etc).
     */
    @JvmStatic external fun nativeSetGameOverlayActive(active: Boolean)

    /** UnsatisfiedLinkError-safe wrapper for early-boot callers. */
    fun setGameOverlayActive(active: Boolean) {
        try { nativeSetGameOverlayActive(active) }
        catch (_: UnsatisfiedLinkError) { /* .so not loaded yet */ }
    }

    /**
     * Drain the next ActivateGameOverlay* request the running game has
     * fired on its ISteamFriends slots (28..33). Returns null when no
     * request is pending. The string is `kind\x01arg1\x01sid\x01appid`:
     *   - kind ∈ {"webpage","store","user","invite","dialog"}
     *   - arg1: URL (webpage) / dialog name (dialog,user) / empty
     *   - sid:  decimal uint64 (user SID or lobby SID; 0 if N/A)
     *   - appid: decimal uint32 (store appid; 0 if N/A)
     *
     * SteamService polls this from a background coroutine and dispatches
     * Intent.ACTION_VIEW with the matching Steam Community / store URL.
     */
    @JvmStatic external fun nativePollOverlayRequest(): String?

    fun pollOverlayRequest(): String? =
        try { nativePollOverlayRequest() }
        catch (_: UnsatisfiedLinkError) { null }

    /**
     * Push an avatar's RGBA8 bytes for a friend (or self). [tier] is
     * 0/1/2 = small/medium/large. [rgba] length must equal width*
     * height*4. Returns the allocated image handle (0 on bad args).
     * Emits AvatarImageLoaded_t so any game/UI gating on the callback
     * proceeds to render.
     */
    @JvmStatic external fun nativePushFriendAvatar(
        steamId: Long, tier: Int, width: Int, height: Int, rgba: ByteArray): Int
    /** Diagnostic — slot 34 + ISteamUtils slot 5; hi32=handle, lo32=(w<<16)|h. */
    @JvmStatic external fun nativeDiagnosticGetSmallAvatarSize(steamId: Long): Long
    /** Diagnostic — slot (34+tier) + ISteamUtils slot 5. tier∈{0,1,2}; same pack. */
    @JvmStatic external fun nativeDiagnosticGetTieredAvatarSize(steamId: Long, tier: Int): Long
    /** Diagnostic — slot 6 (GetImageRGBA); fills [out], returns bytes copied. */
    @JvmStatic external fun nativeDiagnosticGetImageRGBA(handle: Int, out: ByteArray): Int

    /**
     * Push the raw avatar-hash bytes (typically 20-byte SHA-1) for a
     * friend. Null/empty clears the slot. Emits PersonaStateChange_t
     * with kPersonaChangeAvatar on content change only — repeated
     * pushes with the same hash do not re-emit. Foundation for the
     * background avatar-CDN fetcher coroutine.
     */
    @JvmStatic external fun nativeSetFriendAvatarHash(steamId: Long, hash: ByteArray?)
    /** Diagnostic — returns the stored hash as lowercase hex (no separator). */
    @JvmStatic external fun nativeDiagnosticGetFriendAvatarHashHex(steamId: Long): String

    /** Diagnostic — reads friend_persona_states[sid] direct from pushed state. -1 = not in map. */
    @JvmStatic external fun nativeDiagnosticGetFriendPersonaState(steamId: Long): Int

    /** UnsatisfiedLinkError-safe wrapper for the wn-session push path. */
    fun setFriendAvatarHash(steamId: Long, hash: ByteArray?) {
        try { nativeSetFriendAvatarHash(steamId, hash) }
        catch (_: UnsatisfiedLinkError) { /* .so not loaded yet */ }
    }

    /**
     * Convenience wrapper for [nativeSetFriendRichPresence] — guards
     * against the JNI not being loaded (early boot before any wn-
     * session activity).
     */
    fun setFriendRichPresence(steamId: Long, key: String, value: String?) {
        try {
            nativeSetFriendRichPresence(steamId, key, value)
        } catch (_: UnsatisfiedLinkError) { /* .so not loaded yet */ }
    }
    /** Diagnostic — registers a probe CCallbackBase, drains queue, returns Run-count. */
    @JvmStatic external fun nativeDiagnosticRegisterAndDrain(iCallback: Int): Int
    /** Diagnostic — pushes synthetic CCallResult + drains. Packed: hi32=runs, lo32=eresult. */
    @JvmStatic external fun nativeDiagnosticPushAndDrainCallResult(callbackId: Int, eresult: Int): Long
    /** Diagnostic — calls ISteamUser.GetAuthSessionTicket via vtable; fills [buf] with ticket bytes. */
    @JvmStatic external fun nativeDiagnosticGetAuthTicket(buf: ByteArray): Int
    /** Diagnostic — calls ISteamUser.RequestEncryptedAppTicket then GetEncryptedAppTicket; returns hCall. */
    @JvmStatic external fun nativeDiagnosticRequestEncryptedAppTicket(outBody: ByteArray): Long
    /** Diagnostic — pushes a synthetic CallResult, reads it back via ISteamUtils.GetAPICallResult slot 13. */
    @JvmStatic external fun nativeDiagnosticUtilsGetAPICallResult(callbackId: Int, eresult: Int): Int
    /** Diagnostic — count of inbound TCP IPC connections since process start. */
    @JvmStatic external fun nativeDiagnosticTcpAccepted(): Int
    /** Diagnostic — releases + recreates the pipe so the shutdown emit chain fires. */
    @JvmStatic external fun nativeDiagnosticShutdownPipe(): Boolean

    /** Cycles the pipe (SteamShutdown_t + SteamServersDisconnected_t emit chain). */
    fun shutdownPipe(): Boolean =
        if (!loaded) false
        else try { nativeDiagnosticShutdownPipe() } catch (_: UnsatisfiedLinkError) { false }
}
