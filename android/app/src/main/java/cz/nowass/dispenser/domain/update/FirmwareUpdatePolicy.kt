package cz.nowass.dispenser.domain.update

/**
 * Selection of an over-the-air firmware update (docs/FIRMWARE_UPDATE.md 2-3). Pure: the release
 * list comes from the existing GitHub release check, the manifest download is passed in.
 *
 * The manifest is not a proof of origin (the device verifies the signature); these checks only make
 * sure the app offers, and binds with SHA-256, exactly the asset the release publishes.
 */

/** The firmware manifest `dispenser-firmware-X.Y.Z.json` as parsed; a field of the wrong type is null. */
data class FirmwareManifest(
    val schema: Long?,
    val project: String?,
    val hardware: String?,
    val version: String?,
    val asset: String?,
    val size: Long?,
    val sha256: String?,
    val minInstalled: String?,
    val notes: String?,
)

/** A release that carries both firmware assets of one version (manifest not yet checked). */
data class FirmwareCandidate(
    val release: ReleaseInfo,
    /** Plain `X.Y.Z` from the asset names. */
    val version: String,
    val semantic: SemanticVersion,
    val bin: ReleaseAsset,
    val manifest: ReleaseAsset,
)

/** The firmware the owner can install: everything START_UPDATE needs, plus the notes to show. */
data class FirmwareOffer(
    val version: String,
    val tag: String,
    val size: Long,
    /** 64 lowercase hex characters (the manifest's, equal to GitHub's digest when reported). */
    val sha256Hex: String,
    val notes: String?,
    val isPreRelease: Boolean,
    val releasePageUrl: String?,
)

/** Outcome of downloading one manifest. */
sealed interface ManifestFetch {
    /** Downloaded; [manifest] null when it is not a JSON object. */
    data class Fetched(val manifest: FirmwareManifest?) : ManifestFetch
    /** Not usable (URL refused, HTTP error, too large): this release is skipped. */
    data object Unavailable : ManifestFetch
    /** Network problem (offline, rate limit): the whole check stops with [result]. */
    data class Failed(val result: UpdateCheckResult) : ManifestFetch
}

sealed interface FirmwareCheckResult {
    data class Available(val offer: FirmwareOffer) : FirmwareCheckResult

    /**
     * No newer compatible firmware. [incompatibleNewer]: the highest newer version that needs a newer
     * installed firmware (`min_installed`); [rejectedNewer]: newer releases skipped for an invalid or
     * unavailable manifest.
     */
    data class UpToDate(val incompatibleNewer: String? = null, val rejectedNewer: Int = 0) : FirmwareCheckResult

    /** The device's running version is not a semantic version: nothing can be compared. */
    data object UnknownRunningVersion : FirmwareCheckResult

    /** The release list could not be read: Offline, RateLimited, RepositoryUnavailable, InvalidMetadata, HttpError. */
    data class Failed(val reason: UpdateCheckResult) : FirmwareCheckResult
}

/** Why a candidate's manifest was not accepted (tests and diagnostics). */
sealed interface ManifestCheck {
    data class Ok(val offer: FirmwareOffer) : ManifestCheck
    data class Invalid(val reason: String) : ManifestCheck
    /** Valid, but `min_installed` is newer than the running firmware (needs another update first or USB). */
    data class NeedsNewerInstalled(val minInstalled: String) : ManifestCheck
}

object FirmwareUpdatePolicy {
    const val MANIFEST_URL_PREFIX = "https://github.com/Nowass/dispenser/releases/download/"
    const val PROJECT = "dispenser"
    const val HARDWARE = "xiao-esp32c3"
    /** The OTA slot (BLE_PROTOCOL.md 10.8). */
    const val MAX_SIZE = 1_835_008L
    const val NOTES_MAX = 2_000

    private const val V = "(?:0|[1-9][0-9]{0,4})\\.(?:0|[1-9][0-9]{0,4})\\.(?:0|[1-9][0-9]{0,4})"
    private val PLAIN_VERSION = Regex("^$V$")
    private val BIN = Regex("^dispenser-firmware-($V)\\.bin$")
    private val ID = Regex("^[A-Za-z0-9._-]+$")
    private val SHA256 = Regex("^[0-9a-f]{64}$")

    /** True for a firmware asset name (`dispenser-firmware-X.Y.Z.bin` / `.json`). */
    fun isFirmwareAsset(name: String): Boolean = name.startsWith("dispenser-firmware-") && (name.endsWith(".bin") || name.endsWith(".json"))

    /** Plain `X.Y.Z`, each part without leading zeros and at most 65535 (the device's rule). */
    fun plainVersion(text: String): SemanticVersion? =
        if (PLAIN_VERSION.matches(text) && text.split('.').all { it.toLong() <= 65_535 }) SemanticVersion.parse(text) else null

    /**
     * Published releases (pre-releases only with [beta]) with both assets of the same version, a tag
     * and asset names in `[A-Za-z0-9._-]`, the manifest on the official download path and the image
     * where the device will look for it. Highest version first.
     */
    fun candidates(releases: List<ReleaseInfo>, beta: Boolean): List<FirmwareCandidate> = releases
        .filter { !it.isDraft && (beta || !it.isPreRelease) }
        .filter { it.tagName.length in 1..40 && ID.matches(it.tagName) }
        .flatMap { r ->
            r.assets.mapNotNull { bin ->
                val version = BIN.matchEntire(bin.name)?.groupValues?.get(1) ?: return@mapNotNull null
                val semantic = plainVersion(version) ?: return@mapNotNull null
                val json = r.assets.firstOrNull { it.name == "dispenser-firmware-$version.json" } ?: return@mapNotNull null
                val base = MANIFEST_URL_PREFIX + r.tagName + "/"
                if (json.downloadUrl?.startsWith(MANIFEST_URL_PREFIX) != true) return@mapNotNull null
                // the device downloads <base><tag>/dispenser-firmware-<version>.bin: it must be this asset
                if (bin.downloadUrl != base + bin.name) return@mapNotNull null
                FirmwareCandidate(r, version, semantic, bin, json)
            }
        }
        .sortedByDescending { it.semantic }

    /** docs/FIRMWARE_UPDATE.md 3 on one candidate's manifest. */
    fun check(c: FirmwareCandidate, m: FirmwareManifest?, running: SemanticVersion): ManifestCheck {
        fun bad(why: String) = ManifestCheck.Invalid(why)
        if (m == null) return bad("not a JSON object")
        if (m.schema != 1L) return bad("schema")
        if (m.project != PROJECT) return bad("project")
        if (m.hardware != HARDWARE) return bad("hardware")
        if (m.version != c.version) return bad("version")
        if (m.asset != c.bin.name) return bad("asset")
        val size = m.size ?: return bad("size")
        if (size != c.bin.size || size !in 1..MAX_SIZE) return bad("size")
        val sha = m.sha256 ?: return bad("sha256")
        if (!SHA256.matches(sha)) return bad("sha256")
        c.bin.digest?.let { if (it != "sha256:$sha") return bad("digest") }
        val min = m.minInstalled?.let(::plainVersion) ?: return bad("min_installed")
        if (min > running) return ManifestCheck.NeedsNewerInstalled(m.minInstalled)
        return ManifestCheck.Ok(
            FirmwareOffer(
                version = c.version, tag = c.release.tagName, size = size, sha256Hex = sha,
                notes = notes(m.notes) ?: notes(c.release.body), isPreRelease = c.release.isPreRelease,
                releasePageUrl = c.release.releasePageUrl,
            ),
        )
    }

    /**
     * The highest firmware newer than [runningVersion] whose `min_installed` <= it. Manifests are
     * downloaded newest first, only for newer candidates; an invalid one skips that release.
     */
    suspend fun select(
        runningVersion: String,
        releases: List<ReleaseInfo>,
        beta: Boolean,
        fetch: suspend (FirmwareCandidate) -> ManifestFetch,
    ): FirmwareCheckResult {
        val running = SemanticVersion.parse(runningVersion) ?: return FirmwareCheckResult.UnknownRunningVersion
        var incompatible: String? = null
        var rejected = 0
        for (c in candidates(releases, beta).filter { it.semantic > running }) {
            val m = when (val f = fetch(c)) {
                is ManifestFetch.Failed -> return FirmwareCheckResult.Failed(f.result)
                ManifestFetch.Unavailable -> { rejected++; continue }
                is ManifestFetch.Fetched -> f.manifest
            }
            when (val r = check(c, m, running)) {
                is ManifestCheck.Ok -> return FirmwareCheckResult.Available(r.offer)
                is ManifestCheck.NeedsNewerInstalled -> if (incompatible == null) incompatible = c.version
                is ManifestCheck.Invalid -> rejected++
            }
        }
        return FirmwareCheckResult.UpToDate(incompatible, rejected)
    }

    private fun notes(text: String?): String? {
        val t = text?.trim()?.takeIf { it.isNotEmpty() } ?: return null
        return if (t.length <= NOTES_MAX) t else t.take(NOTES_MAX).trimEnd() + "…"
    }
}
