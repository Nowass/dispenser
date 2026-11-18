package cz.nowass.dispenser.crypto

/** Handshake purposes of docs/BLE_PROTOCOL.md 5.1 (`purpose`). */
enum class Purpose(val wire: Int) {
    SESSION(1),
    CLAIM(2),
    ADD(3),
    RECOVERY(4);

    /** SESSION uses Noise_IK; CLAIM, ADD and RECOVERY use Noise_XXpsk3 (protocol section 11). */
    val usesPsk: Boolean get() = this != SESSION
}

/** A phone static X25519 key pair; the private part is secret. */
class StaticKeyPair(val privateKey: SecretBytes, publicKey: ByteArray) : AutoCloseable {
    private val pub = publicKey.copyOf()
    val publicKey: ByteArray get() = pub.copyOf()
    override fun close() = privateKey.close()
    override fun toString(): String = "StaticKeyPair(**)"
}

/**
 * The Noise provider of the Android client (docs/ANDROID_BLE_DESIGN.md 2.2, 3.1). The phone is
 * always the initiator. No cryptographic logic in Kotlin: everything is the native library.
 */
interface NoiseProvider {
    /** libsodium's randombytes implementation in use (diagnostics). */
    val rngName: String

    /** PSK for CLAIM / ADD / RECOVERY (approved C1); [inviteToken] only for ADD. */
    fun derivePsk(purpose: Purpose, setupCode: SecretBytes, inviteToken: SecretBytes?): SecretBytes

    fun generateStaticKeyPair(): StaticKeyPair
    fun publicKey(privateKey: SecretBytes): ByteArray

    /**
     * New initiator: IK (SESSION) needs [remoteStatic] (the pinned device key) and no PSK;
     * XXpsk3 needs [psk] and no remote key.
     */
    fun newInitiator(purpose: Purpose, prologue: ByteArray, localStatic: SecretBytes, remoteStatic: ByteArray?, psk: SecretBytes?): HandshakeInitiator
}

/** One Noise handshake (native HandshakeState). Not thread-safe across callers; closed exactly once. */
interface HandshakeInitiator : AutoCloseable {
    fun writeMessage(payload: ByteArray = EMPTY): ByteArray
    fun readMessage(message: ByteArray): ByteArray
    val isFinished: Boolean
    fun remoteStatic(): ByteArray
    fun handshakeHash(): ByteArray
    /** Consumes the handshake: the handshake state is freed, the transport ciphers returned. */
    fun split(): TransportCiphers
}

/** The two transport CipherStates of a session; the 8-byte frame header is the AD. */
interface TransportCiphers : AutoCloseable {
    fun encrypt(header: ByteArray, plaintext: ByteArray): ByteArray
    /** Throws [NoiseException] (authentication failure) for a modified, replayed or reordered frame. */
    fun decrypt(header: ByteArray, ciphertext: ByteArray): ByteArray
}

internal val EMPTY = ByteArray(0)

/** [NoiseProvider] over `libdspnoise`. */
class NativeNoiseProvider : NoiseProvider {
    init {
        NativeNoise.ensureLoaded()
    }

    override val rngName: String get() = NativeNoise.rngName()

    override fun derivePsk(purpose: Purpose, setupCode: SecretBytes, inviteToken: SecretBytes?): SecretBytes {
        require(purpose.usesPsk) { "SESSION (IK) uses no PSK" }
        require((purpose == Purpose.ADD) == (inviteToken != null)) { "an invitation token belongs to ADD only" }
        val out = setupCode.read { s ->
            if (inviteToken == null) NativeNoise.psk(purpose.wire, s, null)
            else inviteToken.read { t -> NativeNoise.psk(purpose.wire, s, t) }
        }
        return SecretBytes.wrap(out)
    }

    override fun generateStaticKeyPair(): StaticKeyPair {
        val kp = NativeNoise.keypair()
        try {
            return StaticKeyPair(SecretBytes.wrap(kp.copyOfRange(0, 32)), kp.copyOfRange(32, 64))
        } finally {
            kp.fill(0)
        }
    }

    override fun publicKey(privateKey: SecretBytes): ByteArray = privateKey.read { NativeNoise.publicKey(it) }

    override fun newInitiator(purpose: Purpose, prologue: ByteArray, localStatic: SecretBytes, remoteStatic: ByteArray?, psk: SecretBytes?): HandshakeInitiator {
        if (purpose.usesPsk) {
            require(psk != null && remoteStatic == null) { "XXpsk3: a PSK and no pre-known remote key" }
        } else {
            require(psk == null && remoteStatic != null) { "IK: the pinned device key and no PSK" }
        }
        val pattern = if (purpose.usesPsk) NativeNoise.PATTERN_XXPSK3 else NativeNoise.PATTERN_IK
        val handle = localStatic.read { k ->
            if (psk == null) NativeNoise.hsNew(pattern, prologue, k, remoteStatic, null)
            else psk.read { p -> NativeNoise.hsNew(pattern, prologue, k, remoteStatic, p) }
        }
        return NativeHandshake(handle)
    }
}

/** Wraps a native handshake handle (also used by tests with handles from the test library). */
internal class NativeHandshake(handle: Long) : HandshakeInitiator {
    private var h: Long = handle

    private fun live(): Long = synchronized(this) { check(h != 0L) { "handshake closed" }; h }

    override fun writeMessage(payload: ByteArray): ByteArray = NativeNoise.hsWrite(live(), payload)
    override fun readMessage(message: ByteArray): ByteArray = NativeNoise.hsRead(live(), message)
    override val isFinished: Boolean get() = NativeNoise.hsFinished(live())
    override fun remoteStatic(): ByteArray = NativeNoise.hsRemoteStatic(live())
    override fun handshakeHash(): ByteArray = NativeNoise.hsHash(live())

    override fun split(): TransportCiphers {
        val cs = NativeNoise.hsSplit(live())
        close()
        return NativeCiphers(cs)
    }

    override fun close() {
        synchronized(this) {
            if (h != 0L) {
                NativeNoise.hsFree(h)
                h = 0L
            }
        }
    }
}

internal class NativeCiphers(handle: Long) : TransportCiphers {
    private var c: Long = handle

    private fun live(): Long = synchronized(this) { check(c != 0L) { "transport ciphers closed" }; c }

    override fun encrypt(header: ByteArray, plaintext: ByteArray): ByteArray = synchronized(this) { NativeNoise.csEncrypt(live(), header, plaintext) }
    override fun decrypt(header: ByteArray, ciphertext: ByteArray): ByteArray = synchronized(this) { NativeNoise.csDecrypt(live(), header, ciphertext) }

    override fun close() {
        synchronized(this) {
            if (c != 0L) {
                NativeNoise.csFree(c)
                c = 0L
            }
        }
    }
}
