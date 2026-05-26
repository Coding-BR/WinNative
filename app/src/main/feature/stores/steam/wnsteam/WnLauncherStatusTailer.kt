package com.winlator.cmod.feature.stores.steam.wnsteam

import android.os.Handler
import android.os.Looper
import timber.log.Timber
import java.io.File
import java.io.RandomAccessFile
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Tails the in-Wine launcher's log file (`<prefix>/drive_c/wn-launcher.log`)
 * during a Steam Launcher startup and maps key milestones to user-facing
 * splash strings. Replaces the "Initializing" placeholder text in the
 * preloader dialog with phase updates like:
 *
 *   "Loading Steam client…" → "Signing in to Steam…" → "Launching MH Rise…"
 *
 * Operates entirely from Kotlin; the launcher itself was unchanged beyond
 * hiding its Wine console window. The tailer just polls the same log file
 * the launcher's `log_line()` already writes to.
 *
 * Lifecycle:
 *   tailer = WnLauncherStatusTailer(logFile, gameName) { phaseText -> updateUi(phaseText) }
 *   tailer.start()                  // spawn background thread
 *   …game launches…
 *   tailer.stop()                    // on game-window-appears or activity destroyed
 *
 * Thread model: a single daemon thread polls the file every [pollIntervalMs]
 * via `RandomAccessFile.seek(lastOffset) + readLine()`. The callback runs
 * on the main thread (posted via [Handler]).
 */
class WnLauncherStatusTailer(
    private val logFile: File,
    private val gameDisplayName: String,
    private val pollIntervalMs: Long = 200L,
    private val onPhase: (phaseText: String) -> Unit,
    // Fires when the launcher confirms the game process has started
    // (LaunchApp poll succeeded + the named exe is running). This is
    // the dismiss signal for the preloader in Steam Launcher mode —
    // the launcher is done driving the launch and the game is now
    // running, so the splash can close.
    private val onLaunchComplete: (() -> Unit)? = null,
) {
    private val running = AtomicBoolean(false)
    private val main = Handler(Looper.getMainLooper())
    private var thread: Thread? = null
    @Volatile private var lastEmitted: String = ""

    /** Begin tailing. Idempotent; second call is a no-op. */
    fun start() {
        if (!running.compareAndSet(false, true)) return
        android.util.Log.i(TAG, "start: path=" + logFile.absolutePath
                + " exists=" + logFile.exists()
                + " size=" + (if (logFile.exists()) logFile.length() else -1L)
                + " canRead=" + logFile.canRead())
        thread = Thread({ tailLoop() }, "WnLauncherStatusTailer").apply {
            isDaemon = true
            start()
        }
    }

    /** Stop tailing. Safe to call from any thread. */
    fun stop() {
        running.set(false)
        // Best-effort interrupt — the loop checks `running` between reads
        // so it'll exit on the next poll cycle even without interrupt.
        thread?.interrupt()
        thread = null
    }

    private fun tailLoop() {
        var lastOffset = 0L
        var openedOnce = false
        var iter = 0
        var totalLinesRead = 0
        android.util.Log.i(TAG, "tailLoop: entered, polling every ${pollIntervalMs}ms")
        while (running.get()) {
            iter++
            try {
                if (!logFile.exists()) {
                    if (iter % 25 == 1) {
                        android.util.Log.i(TAG, "tailLoop iter=$iter: file does not yet exist at ${logFile.absolutePath}")
                    }
                    Thread.sleep(pollIntervalMs)
                    continue
                }
                var linesThisIter = 0
                RandomAccessFile(logFile, "r").use { raf ->
                    val len = raf.length()
                    if (!openedOnce) {
                        // Seek to END of any pre-existing content. The launcher's
                        // log is reused across launches — if we read from the
                        // start, we'd replay the PREVIOUS launch's phases (which
                        // includes its terminal "LaunchApp: ... is running"
                        // line, which would prematurely dismiss the splash).
                        // The launcher truncates the file in main() before it
                        // writes any new lines, so the shrink-detection below
                        // will reset lastOffset and we'll then read the fresh
                        // content from byte 0.
                        lastOffset = len
                        openedOnce = true
                        android.util.Log.i(TAG, "tailLoop: first read; file len=$len — seeking to end (skipping any stale content from previous launch); waiting for launcher to truncate + write new content")
                    } else if (len < lastOffset) {
                        // File shrank: the launcher's main() ran fopen("w") to
                        // truncate. Reset to read the fresh content from byte 0.
                        android.util.Log.i(TAG, "tailLoop iter=$iter: file shrank from $lastOffset to $len bytes — launcher truncated, resetting offset")
                        lastOffset = 0L
                    }
                    raf.seek(lastOffset)
                    while (true) {
                        val line = raf.readLine() ?: break
                        linesThisIter++
                        totalLinesRead++
                        consumeLine(line)
                    }
                    lastOffset = raf.filePointer
                }
                if (linesThisIter > 0) {
                    android.util.Log.i(TAG, "tailLoop iter=$iter: read $linesThisIter new line(s), totalRead=$totalLinesRead, offset=$lastOffset")
                }
            } catch (ie: InterruptedException) {
                Thread.currentThread().interrupt()
                break
            } catch (e: Exception) {
                android.util.Log.e(TAG, "tail iteration failed", e)
            }
            try {
                Thread.sleep(pollIntervalMs)
            } catch (ie: InterruptedException) {
                Thread.currentThread().interrupt()
                break
            }
        }
        android.util.Log.i(TAG, "tailLoop: exiting (running=${running.get()}, totalLinesRead=$totalLinesRead)")
    }

    private fun consumeLine(line: String) {
        // Cheap pre-filter: every launcher milestone line is prefixed
        // `[wn-launcher]`. Anything else is something writing to the same
        // file by accident, ignore.
        if (!line.contains("[wn-launcher]")) return
        // Terminal phase: launcher confirmed the game process is now
        // running. Two paths to "running":
        //   1. `LaunchApp: "<exe>" is running` — IClientAppManager
        //      successfully spawned the game via its DRM/launch path
        //   2. `game process started pid=N` — IClientAppManager succeeded
        //      but the launcher's process scan didn't find the named exe
        //      (common for games where steamclient renames or wraps the
        //      exe), and the launcher fell back to direct CreateProcess
        //      which succeeded
        // Both are "the game is now alive" and both should fire the
        // dismiss-the-splash callback (one-shot).
        val isTerminal = (line.contains("is running") && line.contains("LaunchApp"))
                || line.contains("game process started pid=")
        val phase = phaseFor(line)
        if (phase != null && phase != lastEmitted) {
            lastEmitted = phase
            android.util.Log.i(TAG, "phase change: \"$phase\" (from line: ${line.take(80)})")
            main.post { onPhase(phase) }
        }
        if (isTerminal) {
            android.util.Log.i(TAG, "terminal phase (LaunchApp is running) — signaling launch complete")
            main.post { onLaunchComplete?.invoke() }
        }
    }

    /**
     * Map a launcher log line to a user-facing phase string. Order of the
     * `when` clauses matches the actual launcher's emission order so the
     * splash advances monotonically through phases. Returning null means
     * "no UI update for this line".
     */
    private fun phaseFor(line: String): String? = when {
        // Phase order matches wn-steam-launcher/src/main.cpp emission order.
        line.contains("in-process Steam launcher starting") -> "Starting Steam Launcher…"
        line.contains("steamclient64.dll loaded") -> "Loading Steam client…"
        line.contains("Steam_CreateGlobalUser OK") -> "Connecting to Steam…"
        line.contains("LogOn(") && line.contains("EResult=1") -> "Signing in to Steam…"
        line.contains("callback 101 SteamServersConnected") -> "Signed in — fetching game info…"
        line.contains("Steam_BLoggedOn=true") -> "Steam ready"
        line.contains("RequestAppInfoUpdate(appId=") -> "Updating game info…"
        line.contains("GetAppInstallState(appId=") -> "Verifying install…"
        // Per-container redistributable scan/install (replaces Steam's
        // RunInstallScript path). The launcher emits one line per state
        // change; we map the most user-visible ones.
        line.contains("redist scan: scanning") -> "Scanning redistributables…"
        line.contains("installing redistributable:") -> phaseForInstallingRedist(line)
        // Summary line now has fields: installed N, skipped M, failed-marked K,
        // timed-out-unmarked T. We treat any of those endings as terminal.
        line.contains("redist scan: installed") -> "Redistributables ready"
        line.contains("redist scan: ") && line.contains(" of ") -> "Redistributables ready"
        line.contains("redist scan: 0 *.exe installers") -> "No redistributables to install"
        line.contains("redist scan: no _CommonRedist") -> "No redistributables to install"
        // Legacy Steam RunInstallScript path — kept as a fallback if
        // someone toggles kRunSteamInstallScript=true in main.cpp.
        line.contains("RunInstallScript(appId=") -> "Installing redistributables…"
        line.contains("install script finished") -> "Redistributables installed"
        line.contains("steamservice: post-start state=4") -> "Steam service running"
        line.contains("IClientAppManager.LaunchApp(appId=") -> "Launching $gameDisplayName…"
        // No phase text change on the terminal "is running" line — keep
        // "Launching <Game>…" visible during the 3-second hold the
        // dismiss callback in XServerDisplayActivity adds before
        // closing the splash. The terminal signal still fires via the
        // isTerminal branch in consumeLine; only the text stays stable.
        else -> null
    }

    // Pretty-print "installing redistributable: <name> (<n>/<total>, …)" as
    // "Installing <name>… (n/total)" for the splash. Falls back to a generic
    // string when the pattern doesn't match the expected shape.
    private fun phaseForInstallingRedist(line: String): String {
        val marker = "installing redistributable:"
        val start = line.indexOf(marker)
        if (start < 0) return "Installing redistributable…"
        val rest = line.substring(start + marker.length).trim()
        // Format: "<name> (<n>/<total>, <bytes> bytes)"
        val name = rest.substringBefore(" (").trim()
        val ratio = rest.substringAfter("(", "").substringBefore(",", "").trim()
        return if (name.isNotEmpty() && ratio.contains("/")) {
            "Installing $name… ($ratio)"
        } else if (name.isNotEmpty()) {
            "Installing $name…"
        } else {
            "Installing redistributable…"
        }
    }

    companion object {
        private const val TAG = "WnLauncherTailer"
    }
}
