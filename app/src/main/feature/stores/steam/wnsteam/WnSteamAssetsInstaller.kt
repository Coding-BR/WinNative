package com.winlator.cmod.feature.stores.steam.wnsteam

import android.content.Context
import com.winlator.cmod.runtime.container.Container
import com.winlator.cmod.runtime.display.environment.ImageFs
import com.winlator.cmod.shared.io.TarCompressorUtils
import timber.log.Timber
import java.io.File

/**
 * Stages the bundled Wine/Steam IPC binaries into the right places on disk
 * the first time we launch a Steam game (or whenever the Wine version
 * changes). Idempotent: each extract is gated on a sentinel file so we
 * don't blow CPU re-decompressing on every launch.
 *
 * Assets (shipped at app/src/main/assets/wnsteam/):
 *
 *   steam-androidarm64.tzst        Linux/Android side of the Steam Runtime.
 *                                  Extracts into <imageFs.rootDir>, dropping
 *                                  libsteamclient.so + libsteamnetworkingsockets.so
 *                                  + steamservice.so + libtier0_s.so +
 *                                  libvstdlib_s.so under usr/lib/. The JNI
 *                                  bootstrap (WnSteamBootstrap) dlopens
 *                                  libsteamclient.so from this path.
 *
 *   lsteamclient-arm64ec.tzst      Wine-side bridge for ARM64EC Proton.
 *                                  aarch64-windows/lsteamclient.dll  -> drive_c/windows/system32
 *                                  i386-windows/lsteamclient.dll     -> drive_c/windows/syswow64
 *                                  aarch64-unix/lsteamclient.so      -> wine lib (Wine loader picks it up automatically)
 *
 *   lsteamclient-x86_64.tzst       Same shape for x86_64 Proton:
 *                                  x86_64-windows/lsteamclient.dll   -> drive_c/windows/system32
 *                                  i386-windows/lsteamclient.dll     -> drive_c/windows/syswow64
 *                                  x86_64-unix/lsteamclient.so       -> wine lib
 *
 * Container.wineVersion is sniffed to pick the right lsteamclient archive
 * (matches the convention: substring "arm64ec" → ARM64EC build, otherwise
 * x86_64). Containers with a non-Proton Wine variant get neither (the
 * caller should check [isSupportedFor] before calling).
 */
object WnSteamAssetsInstaller {

    private const val TAG = "WnSteamAssets"

    private const val ASSET_DIR     = "wnsteam"
    private const val STEAM_TZST    = "steam-androidarm64.tzst"
    private const val LSC_ARM64EC   = "lsteamclient-arm64ec.tzst"
    private const val LSC_X86_64    = "lsteamclient-x86_64.tzst"
    // Use our own steampipe asset under wnsteam/ — the bundled
    // assets/steampipe/ is the Goldberg-style 18MB DLL used by ColdClient
    // mode; we want the GameNative-derived 7.3MB bridge that calls
    // LoadLibrary("steamclient64.dll") and engages our libsteamclient.so.
    private const val STEAMPIPE_API64 = "wnsteam/steampipe/steam_api64.dll"
    private const val STEAMPIPE_API32 = "wnsteam/steampipe/steam_api.dll"
    // Steam Launcher — Valve's REAL Windows Steam binaries bundle.
    private const val VALVE_STEAM_X64 = "valve-steam-x86_64.tzst"
    // gbe_fork backend the bridge PE-export-forwards to. Staged
    // alongside the bridge so the Windows loader can resolve forwarder
    // entries at LoadLibrary time.
    private const val STEAMPIPE_ORIGINAL_API64 = "wnsteam/steampipe/original_steam_api64.dll"

    /** True if we have any bundled IPC binaries that apply to this container. */
    fun isSupportedFor(container: Container): Boolean =
        lsteamclientArchive(container) != null

    /**
     * Run the install pass for [container]. Safe to call on every launch;
     * idempotent via the sentinel files written under [stamp].
     */
    /**
     * Container-independent half of [install]: extract the bionic Steam
     * runtime (libsteamclient.so + libsteamnetworkingsockets / steamservice /
     * tier0 / vstdlib) into `usr/lib/`, and stage the bridge copy of
     * libsteamclient.so in filesDir. Safe to call early — e.g. at Steam
     * login, before any container is chosen — to pre-warm WnSteamBootstrap.
     * Idempotent via the stamp file.
     */
    fun installBionicRuntime(context: Context): Boolean {
        val imageFs = ImageFs.find(context)
        val steamStamp = File(imageFs.libDir, ".wnsteam-androidarm64.stamp")
        if (!steamStamp.exists()) {
            Timber.tag(TAG).i("Installing $STEAM_TZST → ${imageFs.rootDir}")
            val ok = TarCompressorUtils.extract(
                TarCompressorUtils.Type.ZSTD,
                context,
                "$ASSET_DIR/$STEAM_TZST",
                imageFs.rootDir,
            )
            if (!ok) {
                Timber.tag(TAG).e("Failed to extract $STEAM_TZST")
                return false
            }
            steamStamp.writeText(STEAM_TZST)
        }
        // 2026-05-21: DISABLED. Keeping Valve's real bionic
        // libsteamclient.so (37 MB, shipped inside steam-androidarm64
        // .tzst) at imagefs/usr/lib/. Our overlay was replacing it
        // with the 1.4 MB custom build from app/src/main/cpp/wn-
        // libsteamclient/, which broke the wine PE bridge's
        // matchmaking/P2P/SDR path — Forest's MULTIPLAYER got fake
        // gbe state instead of real Steam lobbies.
        //
        // The custom libsteamclient.so still ships in the APK at
        // applicationInfo.nativeLibraryDir, and the app's JNI side
        // continues to use it via System.loadLibrary("steamclient").
        // Wine bridge talks to the imagefs path (now Valve's binary),
        // app JNI talks to the APK lib path (still ours) — same name,
        // different files, no conflict. See
        // [[project-bionic-use-valve-lsclient]] for full diagnosis.
        //
        // 2026-05-21: overlay DISABLED so Valve's 37 MB bionic libsteamclient.so
        // stays at imagefs/usr/lib/ — the wine PE bridge dlopens it from there
        // and its full ISteam* surface (real CM, real matchmaking, real SDR)
        // takes over for game-side calls. WnLibSteamClient.kt now loads the
        // JNI side via System.loadLibrary("steamclient") which resolves to the
        // APK's nativeLibraryDir (our open-source build with JNI symbols), so
        // the regression diagnosed earlier ("No implementation found for
        // nativePollOverlayRequest") is fixed: Kotlin JNI and wine guest see
        // different libraries by design.
        // overlayOpenSourceLibsteamclient(context, imageFs)
        // Stage a copy of libsteamclient.so directly in filesDir,
        // at the short path the patched lsteamclient.so will dlopen (see
        // patchLsteamclientLibPath — the imagefs/usr/lib path is too long
        // for lsteamclient.so's fixed 65-byte path field).
        stageBridgeLibsteamclient(context, imageFs)
        return true
    }

    /**
     * Replace the libsteamclient.so under
     * `<rootDir>/usr/lib/` with the open-source build from
     * `app/src/main/cpp/wn-libsteamclient/`. The NDK installs it into
     * `applicationInfo.nativeLibraryDir` — same dir [System.loadLibrary]
     * resolves against — so we just copy that file over.
     *
     * Returns true on success or no-op (already overlaid + up to date).
     * Idempotent — keyed by source mtime in a stamp file alongside.
     */
    private fun overlayOpenSourceLibsteamclient(context: Context, imageFs: ImageFs): Boolean {
        val src = File(context.applicationInfo.nativeLibraryDir, "libsteamclient.so")
        if (!src.exists()) {
            Timber.tag(TAG).w("overlay: %s missing from APK — skipping (using bundled fallback)",
                src.absolutePath)
            return false
        }
        val dest = File(imageFs.rootDir, "usr/lib/libsteamclient.so")
        // Fingerprint by (size, full-SHA256-hex). APK install doesn't
        // update file mtime reliably across reinstalls, and an earlier
        // size+first-16-bytes-hex fingerprint falsely accepted stale
        // .so files because the ELF prefix is identical across every
        // arm64 build (see fingerprint() docstring). Full SHA-256 over
        // a 1.4 MB .so is ~15ms — fine for once-per-app-boot.
        val stamp = File(imageFs.libDir, ".wn-libsteamclient.overlay.stamp")
        val srcFingerprint = fingerprint(src)
        if (stamp.exists() && stamp.readText().trim() == srcFingerprint && dest.exists()
            && dest.length() == src.length()) {
            Timber.tag(TAG).d("overlay: dest already matches src (%s) — skipping copy", srcFingerprint)
            return true
        }
        Timber.tag(TAG).i("overlay: src=%d bytes dest=%d bytes — copying open-source build",
            src.length(), if (dest.exists()) dest.length() else -1)
        return try {
            // Make sure dest dir exists (defensive; extractor should
            // have created it from the .tzst). Then atomic-ish rename:
            // copy to .new, then rename over the live file so a half-
            // written file never gets read by a parallel prewarm.
            dest.parentFile?.mkdirs()
            val tmp = File(dest.parentFile, "libsteamclient.so.new")
            src.copyTo(tmp, overwrite = true)
            tmp.renameTo(dest) || run {
                tmp.copyTo(dest, overwrite = true); tmp.delete(); true
            }
            stamp.writeText(srcFingerprint)
            Timber.tag(TAG).i("overlay: open-source libsteamclient.so (%d bytes) -> %s (fp=%s)",
                dest.length(), dest.absolutePath, srcFingerprint)
            true
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "overlay failed; bootstrap will use bundled fallback")
            false
        }
    }

    /**
     * Content fingerprint — full SHA-256 of the file. Earlier
     * implementation used size + first-16-bytes-hex which falsely
     * accepted stale .so files across reinstalls: two arm64 ELF .so
     * builds always share the same first 16 bytes (ELF magic + ABI
     * marker + ELF version) and an unchanged build size happens often
     * when the C++ change only flips an initializer value. Result: the
     * overlay-stamp check returned "up to date" even when the APK
     * shipped a freshly-rebuilt .so. Captured concretely as
     * BIsSubscribed env-fallback shipping but Stage2 still showing
     * `cloud_account=0` because the on-device extracted .so was the
     * pre-default-flip build. Full SHA-256 is ~12ms for our 1.4MB .so
     * — cheap enough for once-per-app-boot, and stable proof.
     */
    private fun fingerprint(f: File): String {
        return try {
            val md = java.security.MessageDigest.getInstance("SHA-256")
            java.io.FileInputStream(f).use { fis ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val n = fis.read(buf)
                    if (n <= 0) break
                    md.update(buf, 0, n)
                }
            }
            buildString {
                append(f.length())
                append('_')
                md.digest().forEach { append(String.format("%02x", it)) }
            }
        } catch (_: Exception) {
            "size${f.length()}"
        }
    }

    fun install(context: Context, container: Container): Boolean {
        // 1) Linux/Android side — usr/lib/ libsteamclient.so + friends.
        if (!installBionicRuntime(context)) return false
        val imageFs = ImageFs.find(context)

        // 2) Wine-side bridge (arm64ec or x86_64 lsteamclient.dll).
        val lscArchive = lsteamclientArchive(container)
        if (lscArchive == null) {
            Timber.tag(TAG).w(
                "No lsteamclient archive for wineVersion=%s; skipping Wine bridge install",
                container.wineVersion,
            )
            return true   // .so side is fine; just no Wine bridge
        }
        val wineStamp = File(imageFs.libDir, ".wnsteam-${lscArchive}.stamp")
        if (wineStamp.exists()) return true

        // Stage into a per-arch tmp dir, then copy the .dlls into the prefix
        // and (optionally) the .so onto the Wine lib path.
        val stagingRoot = File(imageFs.tmpDir, "wnsteam-stage").apply {
            deleteRecursively(); mkdirs()
        }
        Timber.tag(TAG).i("Installing $lscArchive → $stagingRoot")
        val staged = TarCompressorUtils.extract(
            TarCompressorUtils.Type.ZSTD,
            context,
            "$ASSET_DIR/$lscArchive",
            stagingRoot,
        )
        if (!staged) {
            Timber.tag(TAG).e("Failed to extract $lscArchive")
            return false
        }

        val isArm64ec = lscArchive == LSC_ARM64EC
        val winNative = if (isArm64ec) "aarch64-windows" else "x86_64-windows"
        val unixSide  = if (isArm64ec) "aarch64-unix"    else "x86_64-unix"

        val system32 = File(imageFs.wineprefix, "drive_c/windows/system32").apply { mkdirs() }
        val syswow64 = File(imageFs.wineprefix, "drive_c/windows/syswow64").apply { mkdirs() }

        val systemSrc = File(stagingRoot, "$winNative/lsteamclient.dll")
        val syswowSrc = File(stagingRoot, "i386-windows/lsteamclient.dll")
        if (!systemSrc.exists() || !syswowSrc.exists()) {
            Timber.tag(TAG).e("Staged lsteamclient.dlls missing in $stagingRoot")
            return false
        }
        systemSrc.copyTo(File(system32, "lsteamclient.dll"), overwrite = true)
        syswowSrc.copyTo(File(syswow64, "lsteamclient.dll"), overwrite = true)

        // The .so side is dropped on the Wine lib dir so the loader picks
        // it up. Path resolution mirrors winlator's existing convention:
        // {imageFs.libDir}/wine/{arch}/lsteamclient.so.
        val unixSoSrc = File(stagingRoot, "$unixSide/lsteamclient.so")
        if (unixSoSrc.exists()) {
            val unixSoDest = File(imageFs.libDir, "wine/$unixSide/lsteamclient.so").apply {
                parentFile?.mkdirs()
            }
            unixSoSrc.copyTo(unixSoDest, overwrite = true)
            // De-hardcode the bionic libsteamclient.so path baked into the
            // prebuilt lsteamclient.so (see patchLsteamclientLibPath).
            patchLsteamclientLibPath(unixSoDest, context)
        }

        stagingRoot.deleteRecursively()
        wineStamp.writeText(lscArchive)
        Timber.tag(TAG).i("Wine bridge installed (variant=$lscArchive)")
        return true
    }

    /** filesDir path the patched lsteamclient.so dlopens libsteamclient.so from. */
    fun bridgeLibPath(context: Context): File =
        File(context.filesDir, "libsteamclient.so")

    /**
     * Place the Proton lsteamclient bridge — by its REAL name
     * `lsteamclient.dll` — into [container]'s own `system32` + `syswow64`,
     * and stage its unix-side `lsteamclient.so`. Per-container and
     * self-contained: re-extracts the archive each call rather than
     * relying on the global install stamp (which only ever tracked the
     * first container, leaving every other container without the DLLs —
     * the `64=false 32=false` bug).
     *
     * The DLL keeps its real name on purpose: Wine pairs a PE module
     * with its unix counterpart by base name, so `lsteamclient.dll` must
     * stay `lsteamclient.dll` to find `lsteamclient.so`. The game is
     * pointed at it via the ActiveProcess `SteamClientDll{,64}` registry
     * values, not by renaming it to `steamclient64.dll`.
     *
     * Returns true if the 64-bit bridge DLL was placed.
     */
    fun installSteamclientBridgeIntoContainer(context: Context, container: Container): Boolean {
        val archive = lsteamclientArchive(container)
        if (archive == null) {
            Timber.tag(TAG).w("bridge: no lsteamclient archive for wineVersion=%s",
                container.wineVersion)
            return false
        }
        val imageFs = ImageFs.find(context)
        val staging = File(imageFs.tmpDir, "wnsteam-bridge-stage").apply {
            deleteRecursively(); mkdirs()
        }
        val ok = TarCompressorUtils.extract(
            TarCompressorUtils.Type.ZSTD, context, "$ASSET_DIR/$archive", staging)
        if (!ok) {
            Timber.tag(TAG).e("bridge: failed to extract $archive")
            staging.deleteRecursively()
            return false
        }
        val isArm64ec = archive == LSC_ARM64EC
        val winNative = if (isArm64ec) "aarch64-windows" else "x86_64-windows"
        val unixSide = if (isArm64ec) "aarch64-unix" else "x86_64-unix"
        val system32 = File(container.rootDir, ".wine/drive_c/windows/system32")
            .apply { mkdirs() }
        val syswow64 = File(container.rootDir, ".wine/drive_c/windows/syswow64")
            .apply { mkdirs() }
        var placed64 = false
        val src64 = File(staging, "$winNative/lsteamclient.dll")
        val src32 = File(staging, "i386-windows/lsteamclient.dll")
        if (src64.exists()) {
            src64.copyTo(File(system32, "lsteamclient.dll"), overwrite = true)
            placed64 = true
        }
        if (src32.exists()) {
            src32.copyTo(File(syswow64, "lsteamclient.dll"), overwrite = true)
        }
        val unixSo = File(staging, "$unixSide/lsteamclient.so")
        if (unixSo.exists()) {
            val dest = File(imageFs.libDir, "wine/$unixSide/lsteamclient.so")
                .apply { parentFile?.mkdirs() }
            unixSo.copyTo(dest, overwrite = true)
            // The prebuilt lsteamclient.so has the libsteamclient.so path
            // hardcoded to GameNative's package — patch it to ours, here,
            // so the deployed bridge is always patched regardless of the
            // order install()/installBionicRuntime run in.
            patchLsteamclientLibPath(dest, context)
        }
        staging.deleteRecursively()
        Timber.tag(TAG).i("bridge: lsteamclient.dll → %s (64=%b src32=%b)",
            system32.absolutePath, placed64, src32.exists())
        return placed64
    }

    /**
     * Steam Launcher — extract Valve's REAL Windows Steam binaries (steamclient64.dll,
     * Steam.dll, Steam2.dll, tier0_s64.dll, vstdlib_s64.dll, and their 32-bit
     * variants) into the container's wine prefix at
     * `C:\Program Files (x86)\Steam\`. These are the binaries our
     * wn-steam-launcher.exe LoadLibrarys to host Valve's Steam client
     * IN-PROCESS with the game — eliminating the cross-process bridge that
     * blocks Steam Networking Sockets / SDR / P2P callbacks under our
     * current Bionic Steam mode.
     *
     * Source asset: `assets/wnsteam/bionic/valve-steam-x86_64.tzst`
     * (~16 MB compressed; ~50 MB unpacked).
     *
     * Idempotent — checks for a stamp file in the Steam dir.
     *
     * Returns true iff all required binaries landed in the prefix.
     */
    fun installPlanWValveSteam(context: Context, container: Container): Boolean {
        val steamDir = File(container.rootDir, ".wine/drive_c/Program Files (x86)/Steam")
        // If the Bionic-mode setup left the Steam dir as a symlink to the
        // shared steam-client-store, replace it with a real directory so
        // our copies don't follow the link and pollute the shared store.
        // (Normally installBionicSteamPathOverlay does this; Steam Launcher skips
        // the overlay so we have to do it here.)
        val steamPath = steamDir.toPath()
        if (java.nio.file.Files.isSymbolicLink(steamPath)) {
            try {
                java.nio.file.Files.delete(steamPath)
                Timber.tag(TAG).i("planW: replaced Steam-dir symlink with real dir at %s",
                    steamDir.absolutePath)
            } catch (e: Exception) {
                Timber.tag(TAG).e(e, "planW: failed to delete Steam-dir symlink, copies may pollute shared store")
            }
        }
        steamDir.mkdirs()
        // Cache extracted Valve binaries in filesDir/wnsteam-planw-cache/
        // so we only pay the ~16 MB zstd unpack once. The
        // `installBionicSteamPathOverlay` step that runs BEFORE us each
        // launch always rewrites `steamclient64.dll` to the bridge, so we
        // MUST copy from the cache into the Steam dir on every Steam Launcher
        // launch — a stamp-file shortcut would leave the bridge in place.
        val cache = File(context.filesDir, "wnsteam-planw-cache")
        val cacheStamp = File(cache, ".planw-valve-steam.stamp")
        if (!cacheStamp.exists()) {
            cache.deleteRecursively(); cache.mkdirs()
            val ok = TarCompressorUtils.extract(
                TarCompressorUtils.Type.ZSTD, context,
                "$ASSET_DIR/bionic/$VALVE_STEAM_X64", cache)
            if (!ok) {
                Timber.tag(TAG).e("planW: failed to extract %s into cache", VALVE_STEAM_X64)
                cache.deleteRecursively()
                return false
            }
            try { cacheStamp.writeText("ok\n") } catch (_: Exception) {}
            Timber.tag(TAG).i("planW: cached Valve Steam DLLs at %s (%d entries)",
                cache.absolutePath, cache.listFiles()?.size ?: 0)
        }
        var copied = 0
        cache.listFiles()?.forEach { src ->
            if (!src.isFile || src.name.startsWith(".")) return@forEach
            val dst = File(steamDir, src.name)
            try {
                src.copyTo(dst, overwrite = true)
                copied++
            } catch (e: Exception) {
                Timber.tag(TAG).e(e, "planW: copy %s -> %s failed", src.name, dst.absolutePath)
            }
        }
        Timber.tag(TAG).i("planW: staged %d Valve Steam DLLs into %s (from cache)",
            copied, steamDir.absolutePath)

        // steamclient64.dll imports tier0_s64.dll + vstdlib_s64.dll. Whoever
        // loads steamclient64.dll in the game process — our bridge, Valve's
        // own steam_api, or the game's SteamStub DRM stub — resolves those
        // deps on the DEFAULT DLL search path, NOT the Steam dir, so they
        // must also live in system32. Without this, steamclient64.dll's
        // tier0 imports stub out (Wine pins the stubs in ntdll) and
        // process_attach page-faults — the game fails to boot.
        val system32 = File(container.rootDir, ".wine/drive_c/windows/system32")
            .apply { mkdirs() }
        val syswow64 = File(container.rootDir, ".wine/drive_c/windows/syswow64")
        var depCopied = 0
        for (dep in arrayOf("tier0_s64.dll", "vstdlib_s64.dll")) {
            val src = File(steamDir, dep)
            if (src.isFile) {
                try {
                    src.copyTo(File(system32, dep), overwrite = true); depCopied++
                } catch (e: Exception) {
                    Timber.tag(TAG).e(e, "planW: copy %s -> system32 failed", dep)
                }
            }
        }
        if (syswow64.isDirectory) {
            for (dep in arrayOf("tier0_s.dll", "vstdlib_s.dll")) {
                val src = File(steamDir, dep)
                if (src.isFile) {
                    try { src.copyTo(File(syswow64, dep), overwrite = true) }
                    catch (e: Exception) {
                        Timber.tag(TAG).e(e, "planW: copy %s -> syswow64 failed", dep)
                    }
                }
            }
        }
        Timber.tag(TAG).i("planW: staged %d tier0/vstdlib dep(s) into system32", depCopied)

        return copied >= 5  // steamclient64 + Steam + Steam2 + tier0 + vstdlib (at minimum)
    }

    /**
     * Steam Launcher — install our wn-steam-launcher.exe (the in-process Steam host
     * built from `app/src/main/cpp/wn-steam-launcher/`) into the container's
     * Steam dir. Replaces the wn-steam-helper.exe path for Steam Launcher launches.
     */
    fun installPlanWLauncher(context: Context, container: Container): Boolean {
        val steamDir = File(container.rootDir, ".wine/drive_c/Program Files (x86)/Steam")
        // Same symlink-defense as installPlanWValveSteam — if Steam Launcher is the
        // first to touch the Steam dir on this launch, it may still be a
        // symlink to the shared store.
        val steamPath = steamDir.toPath()
        if (java.nio.file.Files.isSymbolicLink(steamPath)) {
            try { java.nio.file.Files.delete(steamPath) } catch (_: Exception) {}
        }
        steamDir.mkdirs()
        val dst = File(steamDir, "wn-steam-launcher.exe")
        if (dst.exists()) { try { dst.delete() } catch (_: Exception) {} }
        val ok = try {
            context.assets.open("$ASSET_DIR/bionic/wn-steam-launcher.exe").use { input ->
                dst.outputStream().use { output -> input.copyTo(output) }
            }
            Timber.tag(TAG).i("planW: installed wn-steam-launcher.exe (%d bytes) at %s",
                dst.length(), dst.absolutePath)
            true
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "planW: failed to install wn-steam-launcher.exe")
            false
        }
        // Copy the CA bundle into the Steam dir so Valve's steamclient64.dll
        // can TLS-verify its CM connection. Valve's bionic libsteamclient.so
        // needs STEAM_SSL_CERT_FILE (see steam_bootstrap.cpp); the Windows
        // steamclient64.dll under Wine has no populated cert store either,
        // so we stage the same bundle at a Windows-accessible path and
        // XServerDisplayActivity points STEAM_SSL_CERT_FILE at it.
        try {
            val caSrc = File(context.filesDir, "wnsteam_cacert.pem")
            if (caSrc.exists() && caSrc.length() > 0) {
                val caDst = File(steamDir, "wnsteam_cacert.pem")
                caSrc.copyTo(caDst, overwrite = true)
                Timber.tag(TAG).i("planW: staged CA bundle (%d bytes) at %s",
                    caDst.length(), caDst.absolutePath)
            } else {
                Timber.tag(TAG).w("planW: %s missing — STEAM_SSL_CERT_FILE will be unset, "
                    + "launcher CM logon may fail TLS", caSrc.absolutePath)
            }
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "planW: CA bundle stage failed")
        }
        return ok
    }

    /**
     * Stage a copy of the bionic `libsteamclient.so` at [bridgeLibPath]
     * (`<filesDir>/libsteamclient.so`). The Wine process runs as our app
     * uid and can read filesDir; this short path is what the patched
     * lsteamclient.so dlopens. Idempotent — skips when up to date.
     */
    private fun stageBridgeLibsteamclient(context: Context, imageFs: ImageFs): Boolean {
        val src = File(imageFs.rootDir, "usr/lib/libsteamclient.so")
        if (!src.exists()) {
            Timber.tag(TAG).w("stageBridge: %s missing — bridge lib not staged", src.absolutePath)
            return false
        }
        val dest = bridgeLibPath(context)
        if (dest.exists() && dest.length() == src.length()) return true
        return try {
            src.copyTo(dest, overwrite = true)
            Timber.tag(TAG).i("stageBridge: libsteamclient.so → %s (%d bytes)",
                dest.absolutePath, dest.length())
            true
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "stageBridge: copy failed")
            false
        }
    }

    /**
     * GameNative's prebuilt `lsteamclient.so` (the Wine-unix bridge) has the
     * path of the bionic `libsteamclient.so` HARDCODED — a Ghidra decompile
     * of `steamclient_init` shows a no-format-specifier
     * `snprintf(buf, 0x1000, "/data/data/app.gamenative/files/imagefs/usr/lib/libsteamclient.so")`.
     * There is no env override. For our app — and every product flavor —
     * that `app.gamenative` path does not exist, so the bridge would fail
     * with "unable to load CreateInterface".
     *
     * We patch the `.rodata` string in place to point at [bridgeLibPath],
     * resolved from the real runtime package dir — correct for every flavor
     * (`com.winnative.cmod` / `com.ludashi.benchmark` / `com.tencent.ig`)
     * and never hardcoded in our source. The replacement is NUL-padded to
     * the original 65-byte field and must fit within it (all our flavor
     * paths are ~49-56 bytes — comfortably within budget).
     */
    private fun patchLsteamclientLibPath(soFile: File, context: Context) {
        val marker = "/data/data/app.gamenative/files/imagefs/usr/lib/libsteamclient.so"
        val markerBytes = marker.toByteArray(Charsets.US_ASCII)
        val targetBytes = bridgeLibPath(context).absolutePath.toByteArray(Charsets.US_ASCII)
        if (targetBytes.size > markerBytes.size) {
            Timber.tag(TAG).e("lsteamclient patch: target path %d bytes exceeds %d-byte field",
                targetBytes.size, markerBytes.size)
            return
        }
        val bytes = try {
            soFile.readBytes()
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "lsteamclient patch: read failed")
            return
        }
        var patched = 0
        var i = 0
        while (i <= bytes.size - markerBytes.size) {
            var match = true
            for (j in markerBytes.indices) {
                if (bytes[i + j] != markerBytes[j]) { match = false; break }
            }
            if (match) {
                for (j in markerBytes.indices) {
                    bytes[i + j] = if (j < targetBytes.size) targetBytes[j] else 0
                }
                patched++
                i += markerBytes.size
            } else {
                i++
            }
        }
        if (patched == 0) {
            Timber.tag(TAG).w("lsteamclient patch: marker path not found in %s — "
                + "upstream asset may have changed; bridge will fail to find libsteamclient.so",
                soFile.name)
            return
        }
        try {
            soFile.writeBytes(bytes)
            Timber.tag(TAG).i("lsteamclient patch: redirected %d path occurrence(s) in %s → %s",
                patched, soFile.name, String(targetBytes, Charsets.US_ASCII))
        } catch (e: Exception) {
            Timber.tag(TAG).e(e, "lsteamclient patch: write failed")
        }
    }

    /**
     * Wipe stamps + installed files so the next [install] re-extracts.
     * Useful when the container's wine version changes (caller is expected
     * to detect this).
     */
    fun reset(context: Context) {
        val imageFs = ImageFs.find(context)
        listOf(
            File(imageFs.libDir, ".wnsteam-androidarm64.stamp"),
            File(imageFs.libDir, ".wnsteam-$LSC_ARM64EC.stamp"),
            File(imageFs.libDir, ".wnsteam-$LSC_X86_64.stamp"),
        ).forEach { if (it.exists()) it.delete() }
    }

    /**
     * Pick the lsteamclient.dll variant that matches the prefix's PE arch.
     *
     * Container.wineVersion is the PROTON build name, e.g.
     * `Proton-9.0-arm64ec-0` — describes the native ABI Wine itself runs
     * on, NOT necessarily the PE arch inside the prefix. A
     * `Proton-9.0-arm64ec-0` container with `wineprefixArch=x86_64` runs
     * x86_64 PE games + DLLs in `system32` (verified empirically:
     * `system32/kernel32.dll` is `MS PE32+ executable (DLL) x86-64`).
     *
     * Earlier code keyed off `wineVersion` and shipped the aarch64 bridge
     * (PE32+ Aarch64, 56 MB) into an x86_64 prefix; Wine's PE loader then
     * refused to load it for x86_64 callers and Forest's stock
     * `steam_api64.dll` silently failed `LoadLibrary("steamclient64.dll")`
     * → game showed "STEAM NOT INITIALIZED" with `/proc/maps` containing
     * zero steam-related mappings (verified 2026-05-20).
     *
     * Now: consult the actual wineprefix arch in `extraData`. Falls back
     * to wineVersion sniffing if extraData is missing — preserves prior
     * behaviour for older containers that haven't been re-saved.
     */
    /**
     * Replace the game's stock `steam_api.dll` / `steam_api64.dll` with the
     * GameNative-style steampipe bridge shipped under `assets/wnsteam/
     * steampipe/`. The original DLL is preserved alongside as `.orig`.
     *
     * Why: Valve's stock `steam_api64.dll` for older Steamworks SDK builds
     * (e.g. The Forest at 211 KB, 2018-vintage) short-circuits inside
     * `SteamAPI_Init` before reaching `LoadLibrary("steamclient64.dll")`.
     * Wine `+module/+loaddll` trace shows zero `steamclient64.dll` load
     * attempts in 15K+ lines of debug — our whole Bionic bridge chain
     * (steam_api64 → steamclient64.dll → lsteamclient.dll →
     * libsteamclient.so) is dormant.
     *
     * The GameNative steampipe replacement (7.3 MB PE, exports the full
     * `SteamAPI_*` + flat-C surface) DOES reach the LoadLibrary path,
     * letting our existing bridge engage. Game's Mono / CSteamworks call
     * SteamAPI_Init → steampipe bridge → LoadLibrary("steamclient64.dll")
     * → our system32 bridge → dlopen libsteamclient.so → seed_state_
     * from_env_once populates state → BLoggedOn returns true.
     *
     * Walks the game install tree (max-depth 10) so DLLs under
     * `<game>/<engine_data>/Plugins/` (e.g. Unity's `TheForest_Data/
     * Plugins/steam_api64.dll`) are also swapped. Skips entries whose
     * `.orig` already exists with our bridge stamp — idempotent across
     * launches.
     *
     * Returns the number of DLLs swapped. Zero if the game dir doesn't
     * exist or has no steam_api*.dll files.
     */
    fun installSteampipeBridgeIntoApp(context: Context, gameInstallDir: File): Int {
        if (!gameInstallDir.isDirectory) {
            Timber.tag(TAG).w("steampipe: game dir missing: ${gameInstallDir.absolutePath}")
            return 0
        }
        var swapped = 0
        gameInstallDir.walkTopDown().maxDepth(10).forEach { f ->
            if (!f.isFile) return@forEach
            val n = f.name.lowercase()
            if (n != "steam_api.dll" && n != "steam_api64.dll") return@forEach
            val is64 = n == "steam_api64.dll"
            val asset = if (is64) STEAMPIPE_API64 else STEAMPIPE_API32
            val orig = File(f.parentFile, f.name + ".orig")
            try {
                // Preserve the genuine DLL on first sighting (orig doesn't
                // exist yet, so the live file IS the original Valve binary).
                // Once .orig is captured we never overwrite it — a subsequent
                // re-swap leaves .orig untouched.
                if (!orig.exists()) {
                    f.copyTo(orig, overwrite = false)
                }
                // Always rewrite the live DLL from our asset. Rewriting on
                // every launch is cheap (few hundred KB) and naturally picks
                // up a new bridge build without needing a version marker —
                // APK-compressed assets can't be openFd'd to compare lengths
                // cheaply, and a stamp file would have to track asset
                // identity anyway.
                java.io.FileOutputStream(f).use { out ->
                    context.assets.open(asset).use { it.copyTo(out) }
                }
                swapped++
                Timber.tag(TAG).i("steampipe: swapped ${f.path} (orig backed up as ${orig.name})")

                // Stage the gbe_fork backend alongside the bridge under
                // the name `original_steam_api64.dll`. The bridge PE
                // export-forwards ~1200 calls to this module — without
                // it, the Windows loader fails forwarder resolution at
                // first GetProcAddress and the game can't init Steam.
                // Only stage the 64-bit variant for now; 32-bit support
                // can come later if any game asks.
                if (is64) {
                    val backend = File(f.parentFile, "original_steam_api64.dll")
                    try {
                        java.io.FileOutputStream(backend).use { out ->
                            context.assets.open(STEAMPIPE_ORIGINAL_API64).use { it.copyTo(out) }
                        }
                        Timber.tag(TAG).i("steampipe: staged ${backend.path} as forwarder backend")
                    } catch (e: Exception) {
                        Timber.tag(TAG).e(e, "steampipe: backend stage failed for ${backend.path}")
                    }
                }
            } catch (e: Exception) {
                Timber.tag(TAG).e(e, "steampipe: swap failed for ${f.path}")
            }
        }
        Timber.tag(TAG).i("steampipe: $swapped steam_api*.dll(s) swapped under ${gameInstallDir.absolutePath}")
        return swapped
    }

    private fun lsteamclientArchive(container: Container): String? {
        val prefixArch = container.getExtra("wineprefixArch", "").lowercase()
        if (prefixArch.isNotBlank()) {
            return when {
                prefixArch.contains("arm64ec") || prefixArch.contains("aarch64") -> LSC_ARM64EC
                prefixArch.contains("x86_64") || prefixArch.contains("win64") -> LSC_X86_64
                else -> null
            }
        }
        return when {
            container.wineVersion?.contains("arm64ec", ignoreCase = true) == true -> LSC_ARM64EC
            container.wineVersion?.contains("x86_64",  ignoreCase = true) == true -> LSC_X86_64
            else -> null
        }
    }
}
