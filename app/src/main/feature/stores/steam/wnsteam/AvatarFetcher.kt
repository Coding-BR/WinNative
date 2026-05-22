package com.winlator.cmod.feature.stores.steam.wnsteam

import android.graphics.BitmapFactory
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import timber.log.Timber
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap

/**
 * Steam-avatar fetcher. Given an SHA-1 avatar hash (the bytes Steam
 * carries in `CMsgClientPersonaState`), fetches the JPG from the
 * akamai CDN, decodes to ARGB via [BitmapFactory], converts to RGBA8,
 * and pushes through [WnLibSteamClient.nativePushFriendAvatar] so
 * games' ISteamFriends.GetSmallFriendAvatar → ISteamUtils.GetImageRGBA
 * round-trip resolves real pixels.
 *
 * **CDN URL pattern** (Steam's current public scheme):
 * - small  (32x32):  `https://avatars.akamai.steamstatic.com/{hash}.jpg`
 * - medium (64x64):  `…/{hash}_medium.jpg`
 * - full   (184x184): `…/{hash}_full.jpg`
 *
 * In-flight dedup is keyed by `{hashHex}:{tier}` — two consecutive
 * persona pushes for the same friend with the same hash fire exactly
 * one HTTP request. Completed fetches are NOT cached here because
 * libsteamclient.so already retains the bytes via image_registry; a
 * re-push only happens when the hash actually changes (the C++ side
 * de-duplicates via byte-compare).
 *
 * All work runs on [Dispatchers.IO] under a [SupervisorJob] so a
 * fetch failure doesn't poison sibling fetches. Errors are logged at
 * WARN — there's no retry yet; the next persona push for the same
 * friend will re-enqueue if it carries a hash.
 */
object AvatarFetcher {
    private const val TAG = "AvatarFetcher"
    private const val CONNECT_TIMEOUT_MS = 8_000
    private const val READ_TIMEOUT_MS    = 12_000

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val inFlight = ConcurrentHashMap<String, Job>()

    /**
     * Enqueue tier-0/1/2 fetches in parallel. Steam's friends overlay
     * typically renders medium (64x64) in the list view AND large
     * (184x184) in the profile popup, so prefetching all three avoids
     * a visible gap when the user opens a friend's profile after
     * seeing them in the list. The fetcher's dedup keys on hash:tier
     * so the three requests run independently and overlap.
     *
     * No-ops on malformed [hashHex] (same validation as [enqueue]).
     */
    fun enqueueAllTiers(steamId: Long, hashHex: String) {
        enqueue(steamId, hashHex, tier = 0)
        enqueue(steamId, hashHex, tier = 1)
        enqueue(steamId, hashHex, tier = 2)
    }

    /**
     * Enqueue a fetch for [steamId] using [hashHex] (lowercase hex of
     * the avatar SHA-1). [tier] is 0/1/2 = small/medium/large.
     *
     * No-ops when [hashHex] is empty/malformed or an in-flight fetch
     * for the same hash+tier already exists.
     */
    fun enqueue(steamId: Long, hashHex: String, tier: Int = 0) {
        if (steamId == 0L || tier !in 0..2) return
        if (hashHex.isEmpty() || hashHex.length % 2 != 0) return
        if (!hashHex.all { it in '0'..'9' || it in 'a'..'f' }) return
        val key = "$hashHex:$tier"
        if (inFlight.containsKey(key)) return
        val job = scope.launch {
            try {
                val img = fetchAndDecode(hashHex, tier)
                if (img == null) {
                    Timber.tag(TAG).w("fetchAndDecode returned null hash=$hashHex tier=$tier")
                    return@launch
                }
                val handle = WnLibSteamClient.nativePushFriendAvatar(
                    steamId, tier, img.width, img.height, img.rgba)
                Timber.tag(TAG).i(
                    "pushed avatar sid=%d tier=%d %dx%d → handle=%d",
                    steamId, tier, img.width, img.height, handle)
            } catch (t: Throwable) {
                Timber.tag(TAG).w(t, "fetch failed sid=$steamId hash=$hashHex tier=$tier")
            } finally {
                inFlight.remove(key)
            }
        }
        inFlight[key] = job
    }

    /**
     * Synchronously fetch + decode + convert. Public for the diagnostic
     * op's verification path — production callers should use [enqueue].
     */
    @Throws(Exception::class)
    fun fetchAndDecode(hashHex: String, tier: Int): RgbaImage? {
        val suffix = when (tier) {
            0 -> ""
            1 -> "_medium"
            2 -> "_full"
            else -> return null
        }
        val url = URL("https://avatars.akamai.steamstatic.com/${hashHex}${suffix}.jpg")
        val conn = (url.openConnection() as HttpURLConnection).apply {
            connectTimeout = CONNECT_TIMEOUT_MS
            readTimeout    = READ_TIMEOUT_MS
            instanceFollowRedirects = true
            requestMethod  = "GET"
        }
        val bytes = try {
            if (conn.responseCode !in 200..299) {
                Timber.tag(TAG).w("HTTP %d for %s", conn.responseCode, url)
                return null
            }
            conn.inputStream.use { it.readBytes() }
        } finally {
            conn.disconnect()
        }
        val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return null
        val w = bmp.width
        val h = bmp.height
        // Sanity: Steam avatar tiers are 32/64/184 but we don't pin —
        // some legacy avatars are 96 or non-standard. Cap at a hard
        // upper bound to keep buffer allocation safe.
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            Timber.tag(TAG).w("rejecting bitmap %dx%d (out of bounds)", w, h)
            return null
        }
        val argb = IntArray(w * h)
        bmp.getPixels(argb, 0, w, 0, 0, w, h)
        bmp.recycle()
        // Bitmap.getPixels gives 0xAARRGGBB; the SDK wants RGBA bytes.
        val rgba = ByteArray(w * h * 4)
        for (i in 0 until w * h) {
            val px = argb[i]
            rgba[i * 4 + 0] = ((px shr 16) and 0xFF).toByte() // R
            rgba[i * 4 + 1] = ((px shr 8)  and 0xFF).toByte() // G
            rgba[i * 4 + 2] = ( px         and 0xFF).toByte() // B
            rgba[i * 4 + 3] = ((px ushr 24) and 0xFF).toByte() // A
        }
        return RgbaImage(w, h, rgba)
    }

    /** Decoded RGBA8 image — what nativePushFriendAvatar consumes. */
    data class RgbaImage(val width: Int, val height: Int, val rgba: ByteArray)
}
