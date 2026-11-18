package cz.nowass.dispenser.crypto.credentials

import cz.nowass.dispenser.crypto.NoiseProvider
import cz.nowass.dispenser.crypto.SecretBytes
import cz.nowass.dispenser.crypto.StaticKeyPair
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom

/**
 * Phone-side credential of one device association (docs/ANDROID_BLE_DESIGN.md 3.3, 11.1). The phone
 * static private key is secret; the pinned device static key is integrity-critical (protected by
 * the AEAD of the wrapped record). [manualSeqHigh] is the highest MANUAL_DOSE `seq` this phone has
 * ever prepared for the device.
 */
class DeviceCredential internal constructor(
    val deviceIdHex: String,
    val slot: Int,
    val phoneStaticPrivate: SecretBytes,
    phoneStaticPublic: ByteArray,
    deviceStaticPublic: ByteArray,
    val name: String,
    val enrolledAtUtc: Long,
    val manualSeqHigh: Long,
) : AutoCloseable {
    private val phonePub = phoneStaticPublic.copyOf()
    private val devicePub = deviceStaticPublic.copyOf()
    val phoneStaticPublic: ByteArray get() = phonePub.copyOf()
    val deviceStaticPublic: ByteArray get() = devicePub.copyOf()
    override fun close() = phoneStaticPrivate.close()
    override fun toString(): String = "DeviceCredential(device=$deviceIdHex, slot=$slot, key=**)"
}

/** Result of loading a credential. */
sealed interface CredentialState {
    data object Absent : CredentialState
    class Present(val credential: DeviceCredential) : CredentialState
    /** The Keystore key is gone or invalidated (reinstall, cleared storage, Keystore reset): re-enroll. */
    data object Lost : CredentialState
    /** The record exists but cannot be used (tampered, truncated, unknown format). Never overwritten silently. */
    data class Unreadable(val reason: Reason) : CredentialState {
        enum class Reason { TAMPERED, UNKNOWN_FORMAT, CORRUPT }
    }
}

/**
 * An enrollment in progress: a fresh Keystore key and a fresh phone static key pair (CSPRNG). Nothing
 * is persisted until [CredentialStore.commit] after the device's ENROLL_RESULT; [CredentialStore.abort]
 * deletes the key and clears the key pair.
 */
class PendingEnrollment internal constructor(val deviceIdHex: String, internal val alias: String, val keyPair: StaticKeyPair) : AutoCloseable {
    @Volatile internal var finished = false
    /** Durably staged (before XX message 3): from here on only confirm/discard may end it. */
    @Volatile var staged = false
        internal set
    override fun close() = keyPair.close()
    override fun toString(): String = "PendingEnrollment(device=$deviceIdHex, key=**)"
}

/**
 * Credential records, one file per device in an app-private no-backup directory, each wrapped by its
 * own Keystore key ([KeyWrapper]). Writes are atomic (temp file + fsync + rename). Never included in
 * system backup (disabled) or the JSON export.
 *
 * File: "DSPC" | container 1 | alias_len u8 | alias (ASCII) | wrapper blob (iv || ct || tag).
 * Wrapped record (format 1, 145 bytes): format u8 | device_id 6 | slot u8 | phone_priv 32 |
 * phone_pub 32 | device_pub 32 | name_len u8 | name 24 (zero padded) | enrolled_at i64 | manual_seq_high i64.
 * AAD = "DSP2 cred v1" || device_id || alias.
 */
class CredentialStore(
    private val dir: File,
    private val wrapper: KeyWrapper,
    private val noise: NoiseProvider,
    private val random: SecureRandom = SecureRandom(),
) {
    init {
        dir.mkdirs()
    }

    private fun file(deviceIdHex: String) = File(dir, "${normalize(deviceIdHex)}.cred")
    private fun pendingFile(deviceIdHex: String) = File(dir, "${normalize(deviceIdHex)}.pending")

    @Synchronized
    fun load(deviceIdHex: String): CredentialState = loadFile(deviceIdHex, file(deviceIdHex), pending = false)

    /**
     * A staged enrollment whose ENROLL_RESULT was never received (docs/ANDROID_BLE_DESIGN.md 11.7):
     * the device may or may not have stored it. Present (slot -1) until [confirmPending] (after a
     * successful IK SESSION with this key) or an explicit user [discardPending].
     */
    @Synchronized
    fun loadPending(deviceIdHex: String): CredentialState = loadFile(deviceIdHex, pendingFile(deviceIdHex), pending = true)

    private fun loadFile(deviceIdHex: String, f: File, pending: Boolean): CredentialState {
        if (!f.exists()) return CredentialState.Absent
        val raw = runCatching { f.readBytes() }.getOrElse { return CredentialState.Unreadable(CredentialState.Unreadable.Reason.CORRUPT) }
        val c = Container.parse(raw) ?: return CredentialState.Unreadable(CredentialState.Unreadable.Reason.UNKNOWN_FORMAT)
        if (!wrapper.hasKey(c.alias)) return CredentialState.Lost
        val plain = try {
            wrapper.decrypt(c.alias, c.blob, aad(deviceIdHex, c.alias))
        } catch (e: KeyLostException) {
            return CredentialState.Lost
        } catch (e: TamperedException) {
            return CredentialState.Unreadable(CredentialState.Unreadable.Reason.TAMPERED)
        }
        try {
            val cred = Record.decode(plain, normalize(deviceIdHex), pending) ?: return CredentialState.Unreadable(CredentialState.Unreadable.Reason.UNKNOWN_FORMAT)
            return CredentialState.Present(cred)
        } finally {
            plain.fill(0)
        }
    }

    /** Starts an enrollment (CLAIM, ADD, RECOVERY): new key alias and new phone key pair. */
    @Synchronized
    fun beginEnrollment(deviceIdHex: String): PendingEnrollment {
        val id = normalize(deviceIdHex)
        val suffix = ByteArray(8).also { random.nextBytes(it) }.joinToString("") { "%02x".format(it) }
        val alias = "dsp_cred_v1_${id}_$suffix"
        wrapper.createKey(alias)
        return try {
            PendingEnrollment(id, alias, noise.generateStaticKeyPair())
        } catch (e: Exception) {
            wrapper.deleteKey(alias)
            throw e
        }
    }

    /**
     * Durably stages the enrollment BEFORE the phone sends XX message 3 (the device commits when it
     * receives message 3, before it answers ENROLL_RESULT). If the connection or ENROLL_RESULT is
     * lost afterwards, the phone still holds the key the device may have enrolled.
     */
    @Synchronized
    fun stage(p: PendingEnrollment, deviceStaticPublic: ByteArray, name: String, startedAtUtc: Long) {
        check(!p.finished && !p.staged) { "enrollment already staged or finished" }
        require(deviceStaticPublic.size == 32)
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        require(nameBytes.size <= 24) { "name longer than 24 bytes" }
        val priv = p.keyPair.privateKey.copyBytes()
        try {
            val record = Record.encode(p.deviceIdHex, Record.SLOT_UNKNOWN, priv, p.keyPair.publicKey, deviceStaticPublic, nameBytes, startedAtUtc, 0L)
            write(pendingFile(p.deviceIdHex), p.deviceIdHex, p.alias, record)
            record.fill(0)
            p.staged = true
        } finally {
            priv.fill(0)
        }
    }

    /**
     * Persists the enrollment after ENROLL_RESULT (atomic). A previous credential of the same device is
     * replaced, and its key deleted, only after the new record is durable.
     */
    @Synchronized
    fun commit(p: PendingEnrollment, slot: Int, deviceStaticPublic: ByteArray, name: String, enrolledAtUtc: Long): DeviceCredential {
        check(!p.finished) { "enrollment already finished" }
        require(slot in 0..3 && deviceStaticPublic.size == 32)
        val nameBytes = name.toByteArray(Charsets.UTF_8)
        require(nameBytes.size <= 24) { "name longer than 24 bytes" }
        val old = Container.parse(runCatching { file(p.deviceIdHex).readBytes() }.getOrNull() ?: ByteArray(0))
        val priv = p.keyPair.privateKey.copyBytes()
        try {
            val record = Record.encode(p.deviceIdHex, slot, priv, p.keyPair.publicKey, deviceStaticPublic, nameBytes, enrolledAtUtc, 0L)
            write(file(p.deviceIdHex), p.deviceIdHex, p.alias, record)
            record.fill(0)
            p.finished = true
            if (p.staged) pendingFile(p.deviceIdHex).delete() // same key: only the staging record goes
            if (old != null && old.alias != p.alias) wrapper.deleteKey(old.alias)
            return DeviceCredential(p.deviceIdHex, slot, SecretBytes.wrap(priv.copyOf()), p.keyPair.publicKey, deviceStaticPublic, name, enrolledAtUtc, 0L)
        } finally {
            priv.fill(0)
            p.close()
        }
    }

    /**
     * Abandons an enrollment that was NOT staged (failure before XX message 3 was sent): nothing was
     * stored; the new key is deleted and the key pair cleared. A staged enrollment cannot be aborted
     * silently: [release] it and resolve it later with [confirmPending] or [discardPending].
     */
    @Synchronized
    fun abort(p: PendingEnrollment) {
        check(!p.staged || p.finished) { "staged enrollment: confirm it with IK or discard it explicitly" }
        if (!p.finished) {
            p.finished = true
            wrapper.deleteKey(p.alias)
        }
        p.close()
    }

    /** Releases the in-memory state of a staged enrollment whose ENROLL_RESULT did not arrive. */
    fun release(p: PendingEnrollment) = p.close()

    /**
     * Promotes the staged enrollment after an IK SESSION with the staged key and pinned device key
     * succeeded (proof that the device stored it); [slot] from PHONES_LIST (`is_self`). A previous
     * credential of the device is replaced, its key deleted after the new record is durable.
     */
    @Synchronized
    fun confirmPending(deviceIdHex: String, slot: Int) {
        require(slot in 0..3)
        val st = loadPending(deviceIdHex)
        check(st is CredentialState.Present) { "no usable staged enrollment" }
        val pendingAlias = Container.parse(pendingFile(deviceIdHex).readBytes())!!.alias
        val old = Container.parse(runCatching { file(deviceIdHex).readBytes() }.getOrNull() ?: ByteArray(0))
        st.credential.use { c ->
            val priv = c.phoneStaticPrivate.copyBytes()
            try {
                val record = Record.encode(c.deviceIdHex, slot, priv, c.phoneStaticPublic, c.deviceStaticPublic,
                    c.name.toByteArray(Charsets.UTF_8), c.enrolledAtUtc, 0L)
                write(file(deviceIdHex), deviceIdHex, pendingAlias, record)
                record.fill(0)
            } finally {
                priv.fill(0)
            }
        }
        pendingFile(deviceIdHex).delete()
        if (old != null && old.alias != pendingAlias) wrapper.deleteKey(old.alias)
    }

    /**
     * Discards a staged enrollment — only on an explicit user decision after the IK confirmation
     * failed (the device did not store it, or it was removed). The confirmed credential, if any, stays.
     */
    @Synchronized
    fun discardPending(deviceIdHex: String) {
        val pf = pendingFile(deviceIdHex)
        val c = Container.parse(runCatching { pf.readBytes() }.getOrNull() ?: ByteArray(0))
        val confirmed = Container.parse(runCatching { file(deviceIdHex).readBytes() }.getOrNull() ?: ByteArray(0))
        pf.delete()
        if (c != null && c.alias != confirmed?.alias) wrapper.deleteKey(c.alias)
    }

    /**
     * The next MANUAL_DOSE `seq` = max(high + 1, [floor]) — written durably BEFORE it is returned, so a
     * sent request can never reuse a number after a crash (docs/ANDROID_BLE_DESIGN.md 11.4).
     */
    @Synchronized
    fun nextManualSeq(deviceIdHex: String, floor: Long): Long {
        require(floor >= 1)
        val st = load(deviceIdHex)
        check(st is CredentialState.Present) { "no usable credential" }
        st.credential.use { c ->
            check(c.manualSeqHigh < Long.MAX_VALUE) { "seq exhausted" }
            val seq = maxOf(c.manualSeqHigh + 1, floor)
            val raw = file(deviceIdHex).readBytes()
            val alias = Container.parse(raw)!!.alias
            val priv = c.phoneStaticPrivate.copyBytes()
            try {
                val record = Record.encode(c.deviceIdHex, c.slot, priv, c.phoneStaticPublic, c.deviceStaticPublic,
                    c.name.toByteArray(Charsets.UTF_8), c.enrolledAtUtc, seq)
                write(file(c.deviceIdHex), c.deviceIdHex, alias, record)
                record.fill(0)
            } finally {
                priv.fill(0)
            }
            return seq
        }
    }

    /** Forgets the device: credential and staging files and their Keystore keys. */
    @Synchronized
    fun delete(deviceIdHex: String) {
        for (f in listOf(file(deviceIdHex), pendingFile(deviceIdHex))) {
            val c = Container.parse(runCatching { f.readBytes() }.getOrNull() ?: ByteArray(0))
            f.delete()
            if (c != null) wrapper.deleteKey(c.alias)
        }
    }

    fun securityLevel(deviceIdHex: String): KeyWrapper.SecurityLevel {
        val c = Container.parse(runCatching { file(deviceIdHex).readBytes() }.getOrNull() ?: return KeyWrapper.SecurityLevel.UNKNOWN)
        return if (c == null) KeyWrapper.SecurityLevel.UNKNOWN else wrapper.securityLevel(c.alias)
    }

    private fun write(target: File, deviceIdHex: String, alias: String, record: ByteArray) {
        val blob = wrapper.encrypt(alias, record, aad(deviceIdHex, alias))
        val bytes = Container.encode(alias, blob)
        val tmp = File(dir, target.name + ".tmp")
        FileOutputStream(tmp).use { out ->
            out.write(bytes)
            out.fd.sync()
        }
        check(tmp.renameTo(target)) { "credential rename failed" }
    }

    private fun aad(deviceIdHex: String, alias: String): ByteArray =
        "DSP2 cred v1".toByteArray(Charsets.US_ASCII) + hexToBytes(normalize(deviceIdHex)) + alias.toByteArray(Charsets.US_ASCII)

    private class Container(val alias: String, val blob: ByteArray) {
        companion object {
            private val MAGIC = "DSPC".toByteArray(Charsets.US_ASCII)
            fun encode(alias: String, blob: ByteArray): ByteArray {
                val a = alias.toByteArray(Charsets.US_ASCII)
                require(a.size in 1..255)
                return MAGIC + byteArrayOf(1, a.size.toByte()) + a + blob
            }

            fun parse(b: ByteArray): Container? {
                if (b.size < 6 || !b.copyOf(4).contentEquals(MAGIC) || b[4].toInt() != 1) return null
                val n = b[5].toInt() and 0xFF
                if (n == 0 || b.size < 6 + n + 28) return null
                val alias = String(b, 6, n, Charsets.US_ASCII)
                if (!alias.startsWith("dsp_cred_v1_")) return null
                return Container(alias, b.copyOfRange(6 + n, b.size))
            }
        }
    }

    private object Record {
        const val LEN = 145
        const val SLOT_UNKNOWN = 0xFF // staged: the device's slot is not known yet

        fun encode(id: String, slot: Int, priv: ByteArray, pub: ByteArray, devicePub: ByteArray, name: ByteArray, enrolledAt: Long, seqHigh: Long): ByteArray {
            val b = ByteBuffer.allocate(LEN).order(ByteOrder.LITTLE_ENDIAN)
            b.put(1).put(hexToBytes(id)).put(slot.toByte()).put(priv).put(pub).put(devicePub)
            b.put(name.size.toByte()).put(name).put(ByteArray(24 - name.size))
            b.putLong(enrolledAt).putLong(seqHigh)
            return b.array()
        }

        fun decode(p: ByteArray, id: String, pending: Boolean): DeviceCredential? {
            if (p.size != LEN || p[0].toInt() != 1) return null
            val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
            b.get()
            val did = ByteArray(6).also { b.get(it) }
            if (!did.contentEquals(hexToBytes(id))) return null
            val rawSlot = b.get().toInt() and 0xFF
            val slot = if (pending && rawSlot == SLOT_UNKNOWN) -1 else rawSlot
            val priv = ByteArray(32).also { b.get(it) }
            val pub = ByteArray(32).also { b.get(it) }
            val dpub = ByteArray(32).also { b.get(it) }
            val nl = b.get().toInt() and 0xFF
            if (nl > 24 || (slot !in 0..3 && !(pending && slot == -1))) {
                priv.fill(0)
                return null
            }
            val name = ByteArray(24).also { b.get(it) }
            val enrolled = b.long
            val seq = b.long
            return DeviceCredential(id, slot, SecretBytes.wrap(priv), pub, dpub, String(name, 0, nl, Charsets.UTF_8), enrolled, seq)
        }
    }

    private companion object {
        fun normalize(id: String): String {
            val s = id.lowercase()
            require(s.length == 12 && s.all { it in '0'..'9' || it in 'a'..'f' }) { "device ID: 12 hex digits" }
            return s
        }

        fun hexToBytes(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }
}
