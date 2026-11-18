package cz.nowass.dispenser.crypto

/**
 * Secret byte material (private keys, setup codes, invitation tokens, PSKs) — docs/ANDROID_BLE_DESIGN.md 3.3.
 *
 * - [toString] never shows the content; there is no content-based `equals`/`hashCode`.
 * - [close] overwrites the bytes with zeros; any later access fails. Use with `use { }`.
 * - Residual risk (approved, BLE_PROTOCOL_DESIGN T8): copies the JVM may have made cannot be
 *   guaranteed erased.
 */
class SecretBytes private constructor(private var bytes: ByteArray?) : AutoCloseable {

    val size: Int get() = checkOpen().size

    /** Runs [block] with the live bytes (no copy). The array must not be retained. */
    fun <R> read(block: (ByteArray) -> R): R = synchronized(this) { block(checkOpen()) }

    /** A defensive copy for APIs that need their own array; the caller must clear it. */
    fun copyBytes(): ByteArray = synchronized(this) { checkOpen().copyOf() }

    /** Constant-time comparison with another secret of the same length. */
    fun contentEqualsConstantTime(other: SecretBytes): Boolean = read { a ->
        other.read { b ->
            if (a.size != b.size) return@read false
            var diff = 0
            for (i in a.indices) diff = diff or (a[i].toInt() xor b[i].toInt())
            diff == 0
        }
    }

    val isClosed: Boolean get() = synchronized(this) { bytes == null }

    override fun close() {
        synchronized(this) {
            bytes?.fill(0)
            bytes = null
        }
    }

    override fun toString(): String = "SecretBytes(**)"

    private fun checkOpen(): ByteArray = checkNotNull(bytes) { "secret already cleared" }

    companion object {
        /** Takes ownership of [bytes]: the caller must not use or keep the array afterwards. */
        fun wrap(bytes: ByteArray): SecretBytes = SecretBytes(bytes)

        /** Copies [bytes] and clears the source array. */
        fun takeAndClear(bytes: ByteArray): SecretBytes = SecretBytes(bytes.copyOf()).also { bytes.fill(0) }
    }
}
