package com.winlator.cmod.feature.steamcloudsync

import android.app.Activity
import timber.log.Timber
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// Bridges wn-steam-launcher.exe cloud-conflict requests to the Steam conflict dialog.
object SteamLauncherConflictWatcher {
    private const val TAG = "SteamLauncherConflict"
    private const val REQ = "wn-cloud-conflict.req"
    private const val RESP = "wn-cloud-conflict.resp"

    @Volatile private var running = false

    // Poll the prefix drive_c for a launcher conflict request.
    @JvmStatic
    fun start(activity: Activity, driveCDir: File) {
        if (running) return
        running = true
        val req = File(driveCDir, REQ)
        val resp = File(driveCDir, RESP)
        req.delete()
        resp.delete()
        Thread {
            while (running) {
                try {
                    if (req.exists()) {
                        val content = req.readText().trim()
                        req.delete()
                        handleConflict(activity, content, resp)
                    }
                    Thread.sleep(1000)
                } catch (e: InterruptedException) {
                    return@Thread
                } catch (e: Exception) {
                    Timber.tag(TAG).w(e, "conflict watcher")
                }
            }
        }.apply {
            isDaemon = true
            name = "SteamLauncherConflictWatcher"
        }.start()
    }

    @JvmStatic
    fun stop() {
        running = false
    }

    // Show the conflict dialog and write the user's choice back for the launcher.
    private fun handleConflict(activity: Activity, content: String, resp: File) {
        val parts = content.split(Regex("\\s+"))
        if (parts.size < 3) return
        val localT = parts[1].toLongOrNull() ?: return
        val remoteT = parts[2].toLongOrNull() ?: return
        Timber.tag(TAG).i("conflict localT=%d remoteT=%d", localT, remoteT)

        val timestamps =
            SteamCloudConflictTimestamps(formatTime(localT), formatTime(remoteT))
        val latch = CountDownLatch(1)
        var useLocal = true
        activity.runOnUiThread {
            SteamCloudConflictDialog.show(
                activity,
                timestamps,
                onUseCloud = {
                    useLocal = false
                    latch.countDown()
                },
                onUseLocal = {
                    useLocal = true
                    latch.countDown()
                },
            )
        }
        if (!latch.await(10, TimeUnit.MINUTES)) {
            Timber.tag(TAG).w("conflict dialog timed out; keeping local")
        }
        runCatching { resp.writeText(if (useLocal) "local" else "cloud") }
            .onFailure { Timber.tag(TAG).w(it, "write conflict response") }
    }

    private fun formatTime(unixSeconds: Long): String =
        if (unixSeconds <= 0L) {
            "No save"
        } else {
            SimpleDateFormat("MMM d, yyyy h:mm a", Locale.getDefault())
                .format(Date(unixSeconds * 1000L))
        }
}
