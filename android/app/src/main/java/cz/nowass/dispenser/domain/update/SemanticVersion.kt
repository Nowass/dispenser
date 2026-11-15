package cz.nowass.dispenser.domain.update

/**
 * Semantic version (SemVer 2.0.0). Ordering follows the spec: numeric major/minor/patch,
 * a pre-release sorts before the release, pre-release identifiers compare numerically when
 * numeric and lexically otherwise; build metadata is ignored. Never compared as plain strings.
 */
data class SemanticVersion(
    val major: Long,
    val minor: Long,
    val patch: Long,
    val preRelease: List<String> = emptyList(),
    val buildMetadata: String? = null,
) : Comparable<SemanticVersion> {

    val isPreRelease: Boolean get() = preRelease.isNotEmpty()

    override fun compareTo(other: SemanticVersion): Int {
        compareValuesBy(this, other, { it.major }, { it.minor }, { it.patch }).let { if (it != 0) return it }
        if (preRelease.isEmpty() || other.preRelease.isEmpty()) {
            return when {
                preRelease.isEmpty() && other.preRelease.isEmpty() -> 0
                preRelease.isEmpty() -> 1 // release > pre-release
                else -> -1
            }
        }
        for (i in 0 until minOf(preRelease.size, other.preRelease.size)) {
            val c = compareIdentifier(preRelease[i], other.preRelease[i])
            if (c != 0) return c
        }
        return preRelease.size.compareTo(other.preRelease.size)
    }

    /** Equal precedence (build metadata ignored). */
    fun samePrecedence(other: SemanticVersion): Boolean = compareTo(other) == 0

    override fun toString(): String = buildString {
        append("$major.$minor.$patch")
        if (preRelease.isNotEmpty()) append('-').append(preRelease.joinToString("."))
        if (buildMetadata != null) append('+').append(buildMetadata)
    }

    companion object {
        private val NUMERIC = Regex("0|[1-9]\\d*")
        private val PATTERN = Regex(
            "^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)" +
                "(?:-((?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\\.(?:0|[1-9]\\d*|\\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?" +
                "(?:\\+([0-9a-zA-Z-]+(?:\\.[0-9a-zA-Z-]+)*))?$",
        )

        /** Parses "1.2.3", "1.2.3-beta.1", "1.2.3+build"; an optional leading "v"/"V" (git tags) is accepted. */
        fun parse(text: String): SemanticVersion? {
            val m = PATTERN.matchEntire(text.trim().removePrefix("v").removePrefix("V")) ?: return null
            val (maj, min, pat) = Triple(m.groupValues[1], m.groupValues[2], m.groupValues[3])
            return SemanticVersion(
                major = maj.toLongOrNull() ?: return null,
                minor = min.toLongOrNull() ?: return null,
                patch = pat.toLongOrNull() ?: return null,
                preRelease = m.groupValues[4].takeIf { it.isNotEmpty() }?.split('.') ?: emptyList(),
                buildMetadata = m.groupValues[5].takeIf { it.isNotEmpty() },
            )
        }

        private fun compareIdentifier(a: String, b: String): Int {
            val an = NUMERIC.matches(a); val bn = NUMERIC.matches(b)
            return when {
                an && bn -> a.toBigInteger().compareTo(b.toBigInteger())
                an -> -1 // numeric identifiers have lower precedence
                bn -> 1
                else -> a.compareTo(b)
            }
        }
    }
}
