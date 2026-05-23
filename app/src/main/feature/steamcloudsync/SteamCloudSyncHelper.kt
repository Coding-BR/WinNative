package com.winlator.cmod.feature.steamcloudsync

import android.content.Context
import com.winlator.cmod.feature.stores.steam.enums.PathType
import com.winlator.cmod.feature.stores.steam.enums.SaveLocation
import com.winlator.cmod.feature.stores.steam.enums.SyncResult
import com.winlator.cmod.feature.stores.steam.service.SteamService
import com.winlator.cmod.feature.stores.steam.utils.ContainerUtils
import com.winlator.cmod.feature.stores.steam.utils.FileUtils
import com.winlator.cmod.feature.stores.steam.utils.PrefManager
import com.winlator.cmod.feature.sync.google.GameSaveBackupManager
import com.winlator.cmod.runtime.container.Container
import com.winlator.cmod.runtime.container.ContainerManager
import com.winlator.cmod.runtime.container.Shortcut
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import timber.log.Timber
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.Paths
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

object SteamCloudSyncHelper {
    // Sentinel labels for the conflict dialog. Rendered as-is in
    // SteamCloudConflictUi so they need to be human-readable; we use them
    // (instead of raw "Unknown") to disambiguate "cloud unreachable"
    // from "no remote saves yet" and from "no local saves yet" — each
    // tells the user something actionable instead of leaving them
    // guessing which side actually exists.
    private const val LABEL_NO_LOCAL_SAVES = "No local saves"
    private const val LABEL_NO_CLOUD_SAVES = "No cloud saves"
    private const val LABEL_CLOUD_UNREACHABLE = "Cloud unreachable"

    private fun formatTimestamp(
        timestampMs: Long?,
        emptyLabel: String,
    ): String {
        if (timestampMs == null || timestampMs <= 0L) return emptyLabel
        return SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date(timestampMs))
    }

    @JvmStatic
    fun isOfflineMode(shortcut: Shortcut?): Boolean =
        shortcut != null && shortcut.getExtra("offline_mode", "0") == "1"

    @JvmStatic
    fun forceDownloadOnContainerSwap(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        if (shortcut.getExtra("game_source") != "STEAM") return false
        if (shortcut.getExtra("cloud_force_download").isEmpty()) return false

        val result = runBlocking { forceDownload(context, shortcut) }
        if (result) {
            shortcut.putExtra("cloud_force_download", null)
            shortcut.saveData()
        }

        Timber.i("Force Steam cloud download for %s: %s", shortcut.name, result)
        return result
    }

    suspend fun forceDownload(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        val appId = shortcut.getExtra("app_id").toIntOrNull() ?: return false
        return forceDownloadById(context, appId, shortcut.container)
    }

    suspend fun forceDownloadById(
        context: Context,
        appId: Int,
        containerHint: Container? = null,
    ): Boolean =
        try {
            val prefixToPath = steamPrefixResolver(context, appId, containerHint)
            val syncInfo =
                SteamService
                    .forceSyncUserFiles(
                        appId = appId,
                        prefixToPath = prefixToPath,
                        preferredSave = SaveLocation.Remote,
                        overrideLocalChangeNumber = -1,
                    ).await()

            val ok = syncInfo?.syncResult == SyncResult.Success || syncInfo?.syncResult == SyncResult.UpToDate
            // Refresh libsteamclient.so's cloud_files mirror after the
            // download so a game launched immediately after the
            // \"Restore\" (or any other forceDownloadById caller — the
            // user-facing \"Sync from Steam Cloud\" button + the
            // Cloud Saves history Restore button both land here) sees
            // the freshly-downloaded file list via ISteamRemoteStorage
            // .FileExists / GetFileCount. Without this, the mirror
            // stays stale until the next launch path runs the full
            // enrichSteamSettings.
            if (ok) {
                probeCache.remove(appId)
                runCatching {
                    SteamService.pushCloudStateToLibSteamClient(appId)
                }.onFailure { e ->
                    Timber.w(e, "forceDownloadById: libsteamclient mirror refresh failed for app=%d", appId)
                }
            }
            // Intentionally no snapshot capture here. The user-facing "Sync from Steam Cloud"
            // button and launch-time auto-downloads should just place files locally; an
            // automatic rollback snapshot on every download bloats local storage and isn't
            // what the user asked for. Exit-sync still snapshots via SteamExitCloudSync.
            ok
        } catch (e: Exception) {
            Timber.e(e, "Failed to force Steam cloud download for appId=%d", appId)
            false
        }

    @JvmStatic
    fun hasLocalCloudSaves(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        if (shortcut.getExtra("game_source") != "STEAM") return false
        val appId = shortcut.getExtra("app_id")
        if (appId.isEmpty()) return false
        val appIdInt = appId.toIntOrNull() ?: return false

        // Always consult the filesystem rather than a "synced" pref. The pref
        // shortcut previously trusted a fingerprint match per container, but
        // it stayed valid across symlink-flips that physically moved cloud
        // saves OUT of the container's Steam dir into the shared store (e.g.
        // planW launches where the Android-side download wrote through the
        // symlink, then installPlanWValveSteam replaced the symlink with an
        // empty real dir — saves orphaned, pref still said "synced"). Disk
        // is the only honest answer.
        return hasActualLocalSaves(context, appIdInt)
    }

    fun hasActualLocalSaves(
        context: Context,
        appId: Int,
    ): Boolean {
        val appInfo = SteamService.getAppInfoOf(appId) ?: return false
        val prefixToPath = steamPrefixResolver(context, appId)

        val userDataPath = Paths.get(prefixToPath(PathType.SteamUserData.name))
        if (FileUtils.anyFileMatches(userDataPath, "*", maxDepth = 5)) return true

        val savePatterns =
            appInfo.ufs.saveFilePatterns
                .filter { it.root.isWindows && it.root != PathType.SteamUserData }

        return savePatterns.any { pattern ->
            val basePath = Paths.get(prefixToPath(pattern.root.name), pattern.substitutedPath)
            FileUtils.anyFileMatches(
                rootPath = basePath,
                pattern = pattern.pattern,
                maxDepth = if (pattern.recursive != 0) -1 else 0,
            )
        }
    }

    private fun newestTimestampInFiles(
        rootPath: Path,
        pattern: String,
        maxDepth: Int,
    ): Long? {
        val stream = FileUtils.findFilesRecursive(rootPath, pattern, maxDepth = maxDepth)
        return try {
            var newest = 0L
            stream.forEach { path ->
                val modified =
                    runCatching {
                        if (Files.isRegularFile(path)) Files.getLastModifiedTime(path).toMillis() else 0L
                    }.getOrDefault(0L)
                if (modified > newest) {
                    newest = modified
                }
            }
            newest.takeIf { it > 0L }
        } finally {
            stream.close()
        }
    }

    private fun getNewestActualLocalCloudSaveTimestamp(
        context: Context,
        appId: Int,
    ): Long? {
        val appInfo = SteamService.getAppInfoOf(appId) ?: return null
        val prefixToPath = steamPrefixResolver(context, appId)

        val userDataNewest =
            newestTimestampInFiles(
                Paths.get(prefixToPath(PathType.SteamUserData.name)),
                "*",
                maxDepth = 5,
            )

        val patternNewest =
            appInfo.ufs.saveFilePatterns
                .filter { it.root.isWindows && it.root != PathType.SteamUserData }
            .mapNotNull { pattern ->
                val basePath = Paths.get(prefixToPath(pattern.root.name), pattern.substitutedPath)
                newestTimestampInFiles(
                    rootPath = basePath,
                    pattern = pattern.pattern,
                    maxDepth = if (pattern.recursive != 0) -1 else 0,
                )
            }.maxOrNull()

        return listOfNotNull(userDataNewest, patternNewest).maxOrNull()
    }

    @JvmStatic
    fun cloudSavesDiffer(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        if (!hasLocalCloudSaves(context, shortcut)) return false
        val appId = shortcut.getExtra("app_id").toIntOrNull() ?: return false
        return runBlocking {
            try {
                SteamService.cloudSavesDiffer(appId) ?: true
            } catch (e: Exception) {
                Timber.e(e, "Steam cloud save diff check failed for %s", shortcut.name)
                true
            }
        }
    }

    /**
     * Holds both the diff result and the newest remote timestamp from a single
     * `getAppFileListChange` call so the launch-time prompt doesn't need to
     * round-trip Steam separately for each.
     */
    data class CloudConflictProbe(
        val differs: Boolean,
        val timestamps: SteamCloudConflictTimestamps,
    )

    private const val PROBE_CACHE_TTL_MS = 60_000L
    private val probeCache = ConcurrentHashMap<Int, Pair<Long, CloudConflictProbe>>()

    @JvmStatic
    fun probeCloudConflict(
        context: Context,
        shortcut: Shortcut,
    ): CloudConflictProbe {
        val appId = shortcut.getExtra("app_id").toIntOrNull()
        if (appId == null || !hasLocalCloudSaves(context, shortcut)) {
            return CloudConflictProbe(
                differs = false,
                timestamps = SteamCloudConflictTimestamps(LABEL_NO_LOCAL_SAVES, LABEL_NO_CLOUD_SAVES),
            )
        }
        // Re-launching the same game within PROBE_CACHE_TTL_MS reuses the
        // last successful probe — the cloud and local state can't meaningfully
        // diverge in this window, and skipping the Steam round-trip + SHA-1
        // of every local save file is a noticeable launch-time win.
        // Invalidated by forceDownloadById / uploadLocalSaves when local state
        // actually changes; cloud-unreachable results are intentionally NOT
        // cached so the next launch retries with a hopefully-live session.
        probeCache[appId]?.let { (ts, cached) ->
            if (System.currentTimeMillis() - ts < PROBE_CACHE_TTL_MS) return cached
        }
        // hasLocalCloudSaves can short-circuit on a `synced_STEAM_$appId` pref entry
        // without going through steamPrefixResolver, so the symlink may still be stale
        // when fetchCloudConflictSnapshot reads local files for the SHA comparison.
        // Activate this shortcut's container explicitly so the SHA check sees the
        // correct per-game wineprefix.
        activateContainer(context, shortcut.container)
        return runBlocking {
            try {
                // Pass context so the snapshot can do a SHA-aware content check (not CN-only)
                // and avoid spurious conflict dialogs when local matches cloud after a pull.
                // Snapshot is null when wn-session can't reach the CM (cold start race,
                // network drop, EResult 84/15). In that case we do NOT claim a
                // conflict — popping a dialog with "Unknown" on the cloud side
                // (the historical bug) tells the user nothing actionable and
                // would force them to pick blind. Instead we let the game launch
                // with local files; the user can use "Sync from Steam Cloud"
                // manually once the connection comes back.
                val snapshot = SteamService.fetchCloudConflictSnapshot(appId, context)
                val localActual = getNewestActualLocalCloudSaveTimestamp(context, appId)
                val localTracked =
                    SteamService.getTrackedCloudSaveFiles(appId)?.maxOfOrNull { it.timestamp }
                val localTs = localActual ?: localTracked
                val cloudLabel =
                    when {
                        snapshot == null -> LABEL_CLOUD_UNREACHABLE
                        else -> formatTimestamp(snapshot.newestRemoteTimestamp, LABEL_NO_CLOUD_SAVES)
                    }
                val probe = CloudConflictProbe(
                    differs = snapshot?.differs ?: false,
                    timestamps =
                        SteamCloudConflictTimestamps(
                            localTimestampLabel = formatTimestamp(localTs, LABEL_NO_LOCAL_SAVES),
                            cloudTimestampLabel = cloudLabel,
                        ),
                )
                if (snapshot != null) probeCache[appId] = System.currentTimeMillis() to probe
                probe
            } catch (e: Exception) {
                Timber.e(e, "Steam cloud conflict probe failed for %s", shortcut.name)
                CloudConflictProbe(
                    // Same fail-soft policy as the snapshot==null path —
                    // don't claim conflict when we have no evidence either way.
                    differs = false,
                    timestamps = SteamCloudConflictTimestamps(LABEL_NO_LOCAL_SAVES, LABEL_CLOUD_UNREACHABLE),
                )
            }
        }
    }

    @JvmStatic
    fun getConflictTimestamps(
        context: Context,
        shortcut: Shortcut,
    ): SteamCloudConflictTimestamps {
        val appId = shortcut.getExtra("app_id").toIntOrNull()
        return runBlocking {
            try {
                val localActual = appId?.let { getNewestActualLocalCloudSaveTimestamp(context, it) }
                val localTracked =
                    appId
                        ?.let { SteamService.getTrackedCloudSaveFiles(it) }
                        ?.maxOfOrNull { it.timestamp }
                // getNewestRemoteCloudSaveTimestamp folds the same snapshot
                // path used in probeCloudConflict; null here means the same
                // "cloud unreachable" condition, so surface that to the UI
                // rather than the ambiguous "Unknown".
                val snapshot = appId?.let { SteamService.fetchCloudConflictSnapshot(it, context) }
                val cloudLabel =
                    when {
                        appId == null -> LABEL_NO_CLOUD_SAVES
                        snapshot == null -> LABEL_CLOUD_UNREACHABLE
                        else -> formatTimestamp(snapshot.newestRemoteTimestamp, LABEL_NO_CLOUD_SAVES)
                    }
                SteamCloudConflictTimestamps(
                    localTimestampLabel = formatTimestamp(localActual ?: localTracked, LABEL_NO_LOCAL_SAVES),
                    cloudTimestampLabel = cloudLabel,
                )
            } catch (e: Exception) {
                Timber.e(e, "Failed to build Steam cloud conflict timestamps for %s", shortcut.name)
                SteamCloudConflictTimestamps(LABEL_NO_LOCAL_SAVES, LABEL_CLOUD_UNREACHABLE)
            }
        }
    }

    @JvmStatic
    fun downloadCloudSaves(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        if (shortcut.getExtra("game_source") != "STEAM") return false
        val result = runBlocking { forceDownload(context, shortcut) }
        if (result) {
            // forceDownloadById already refreshes the libsteamclient
            // mirror on success, so the inline push that used to live
            // here is redundant. Just mark the snapshot.
            markCloudSaveSynced(context, shortcut.getExtra("app_id"), shortcut.container)
        }
        Timber.i("Steam cloud save download for %s: %s", shortcut.name, result)
        return result
    }

    /**
     * Force-upload the on-disk Steam save files for [appId] to overwrite Steam Cloud.
     *
     * Used after the launch-time conflict dialog when the user picks "Use Local" — without
     * a push here, the conflict recurs on every subsequent sync because Steam's
     * `changeNumber` for the local side never bumps past the cloud side's. Per Steam
     * protocol (`ClientConflictResolution_Notification.chose_local_files=true`), the
     * canonical "my local wins" resolution is an explicit upload batch.
     */
    suspend fun uploadLocalSaves(
        context: Context,
        appId: Int,
        containerHint: Container? = null,
    ): Boolean =
        try {
            val prefixToPath = steamPrefixResolver(context, appId, containerHint)
            val syncInfo =
                SteamService
                    .forceSyncUserFiles(
                        appId = appId,
                        prefixToPath = prefixToPath,
                        preferredSave = SaveLocation.Local,
                        overrideLocalChangeNumber = -1,
                    ).await()
            val ok = syncInfo?.syncResult == SyncResult.Success || syncInfo?.syncResult == SyncResult.UpToDate
            if (ok) {
                probeCache.remove(appId)
                // Detached snapshot capture so the upload caller isn't held waiting on disk I/O.
                CoroutineScope(Dispatchers.IO).launch {
                    runCatching {
                        SteamSaveSnapshotManager.recordSnapshot(
                            context,
                            appId,
                            GameSaveBackupManager.BackupOrigin.LOCAL,
                        )
                    }.onFailure { Timber.w(it, "Snapshot after Use-Local upload failed for appId=%d", appId) }
                }
            }
            ok
        } catch (e: Exception) {
            Timber.e(e, "Failed to upload local Steam saves for appId=%d", appId)
            false
        }

    @JvmStatic
    fun uploadLocalSavesBlocking(
        context: Context,
        shortcut: Shortcut,
    ): Boolean {
        if (shortcut.getExtra("game_source") != "STEAM") return false
        val appId = shortcut.getExtra("app_id").toIntOrNull() ?: return false
        return runBlocking { uploadLocalSaves(context, appId, shortcut.container) }
    }

    @JvmStatic
    fun downloadCloudSaves(
        context: Context,
        gameId: String,
    ): Boolean {
        val appId = gameId.toIntOrNull() ?: return false
        val result = runBlocking { forceDownloadById(context, appId) }
        if (result) markCloudSaveSynced(context, gameId, ContainerUtils.getUsableContainerOrNull(context, gameId))
        Timber.i("Steam cloud save download for %s: %s", gameId, result)
        return result
    }

    private fun markCloudSaveSynced(
        context: Context,
        appId: String,
        container: Container?,
    ) {
        if (appId.isEmpty()) return
        val prefs = context.getSharedPreferences("cloud_sync_state", Context.MODE_PRIVATE)
        prefs.edit().putString("synced_STEAM_$appId", containerFingerprint(container)).apply()
    }

    /**
     * Identity of the container *incarnation* the cloud saves were synced into.
     * Combines the container id (changes on a container switch) with the .wine
     * prefix dir's last-modified time (changes when the container is
     * reinstalled/recreated). When this no longer matches a stored "synced"
     * flag, the flag is stale and the cloud download must run again.
     */
    private fun containerFingerprint(container: Container?): String {
        if (container == null) return "none"
        val root = container.rootDir ?: return "id-${container.id}"
        val wineDir = File(root, ".wine")
        val sig = if (wineDir.exists()) wineDir.lastModified() else 0L
        return "${container.id}:$sig"
    }

    private fun steamPrefixResolver(
        context: Context,
        appId: Int,
        containerHint: Container? = null,
    ): (String) -> String {
        // PathType.toAbsPath resolves Windows-side prefixes like WinSavedGames against the
        // GLOBAL `imagefs/home/xuser/.wine` path — `home/xuser` is a symlink that
        // ContainerManager.activateContainer flips to point at the active container's
        // per-game home (`home/xuser-N`). If a Steam cloud read/write fires before the
        // game's container is activated (Save History restore from the launcher, "Sync
        // from Cloud" button, launch-time pre-flight before XServerDisplayActivity runs),
        // the symlink still points at whatever container was last active — files land in
        // the wrong game's wineprefix.
        //
        // Prefer the caller-provided container (taken straight from the shortcut), since
        // ContainerUtils.getUsableContainerOrNull falls through to the global "default
        // x86 container" preference — that's appId-agnostic and would activate the
        // wrong container for any Steam game configured with its own non-default
        // container. The appId-based fallback only runs when no shortcut is available.
        activateContainerForCloudOp(context, appId, containerHint)

        // Derive accountId from steamUserSteamId64 BEFORE the steamUserAccountId
        // pref so the Android cloud manager and the in-Wine launcher always
        // agree on which userdata/<accountId>/ subdir to use. The launcher
        // computes accId = WN_STEAM_STEAMID & 0xFFFFFFFF (main.cpp:435), where
        // WN_STEAM_STEAMID is steamUserSteamId64 (XServerDisplayActivity.java:5564).
        // steamUserAccountId can be stale from a prior account login (or a
        // restored Drive backup) and silently point the download at a
        // different userdata folder than the running game reads from.
        val accountId =
            SteamService.userSteamId?.accountID?.toLong()
                ?: PrefManager.steamUserSteamId64.takeIf { it != 0L }?.let { it and 0xFFFFFFFFL }
                ?: PrefManager.steamUserAccountId.takeIf { it != 0 }?.toLong()
                ?: 0L
        return { prefix -> PathType.from(prefix).toAbsPath(context, appId, accountId) }
    }

    private fun activateContainerForCloudOp(
        context: Context,
        appId: Int,
        containerHint: Container?,
    ) {
        val target =
            containerHint
                ?: ContainerUtils.getUsableContainerOrNull(context, appId.toString())
                ?: return
        activateContainer(context, target)
    }

    private fun activateContainer(
        context: Context,
        container: Container,
    ) {
        runCatching {
            ContainerManager(context).activateContainer(container)
        }.onFailure { Timber.w(it, "Failed to activate container id=%d", container.id) }
    }
}
