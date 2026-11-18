package cz.nowass.dispenser.crypto

import cz.nowass.dispenser.crypto.TestNative.hex
import cz.nowass.dispenser.crypto.TestNative.toHex
import cz.nowass.dispenser.crypto.credentials.CredentialState
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.crypto.credentials.FakeKeyWrapper
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.JUnitCore
import java.io.ByteArrayOutputStream
import java.io.PrintStream
import java.util.Base64

/**
 * Output spy (docs/ANDROID_BLE_DESIGN.md 3.3): runs the full transcript suite, the label/invitation
 * parsers, failure paths and the credential store while capturing stdout and stderr (the crypto
 * package has no Android log calls at all: ArchitectureRulesTest), then asserts that no fixture
 * secret — setup code, token, private keys, ephemerals, PSKs — appears in hex or Base64, and that
 * no exception message or toString of a secret type contains them.
 */
class SecretLeakSpyTest {
    @get:Rule val tmp = TemporaryFolder()

    private val records = TestNative.records(TestNative.vectorFile("ble_handshake_v1.txt"))
    private val secrets: MutableList<ByteArray> = records.filter { it.first == "hs" }.flatMap { (_, kv) ->
        listOf("setup", "token", "psk", "phone_static", "phone_ephemeral", "device_static", "device_ephemeral")
            .map { hex(kv.getValue(it)) }.filter { it.isNotEmpty() }
    }.distinctBy { it.toHex() }.toMutableList()

    @Test
    fun no_secret_reaches_any_output() {
        val noise = TestNative.load()
        val buf = ByteArrayOutputStream()
        val out = System.out
        val err = System.err
        val texts = mutableListOf<String>()
        System.setOut(PrintStream(buf, true))
        System.setErr(PrintStream(buf, true))
        try {
            val result = JUnitCore.runClasses(HandshakeVectorsTest::class.java, QrFormatsVectorTest::class.java, NoiseProviderTest::class.java)
            assertTrue(result.failures.joinToString { it.message ?: "" }, result.wasSuccessful())
            // failure paths: messages and toString of secret-bearing types
            val setup = hex(records.first { it.first == "hs" && it.second["id"] == "claim_a" }.second.getValue("setup"))
            try {
                SecretBytes.wrap(setup.copyOf()).use { s -> noise.derivePsk(Purpose.SESSION, s, null) }
            } catch (e: Exception) {
                texts += e.toString()
            }
            val label = SetupLabel.parse(SetupLabel.format("a0b1c2d3e4f5", setup)) as SetupLabel.Result.Ok
            texts += label.label.toString(); texts += label.label.setupCode.toString()
            label.label.close()
            val store = CredentialStore(tmp.root, FakeKeyWrapper(), noise)
            val p = store.beginEnrollment("a0b1c2d3e4f5")
            texts += p.toString(); texts += p.keyPair.toString()
            val c = store.commit(p, 0, ByteArray(32) { 1 }, "A", 0)
            texts += c.toString()
            secrets.add(c.phoneStaticPrivate.copyBytes())
            c.close()
            texts += (store.load("a0b1c2d3e4f5") as CredentialState.Present).toString()
        } finally {
            System.setOut(out)
            System.setErr(err)
        }
        val all = (listOf(buf.toString(Charsets.UTF_8)) + texts).joinToString("\n")
        for (s in secrets) {
            for (enc in listOf(s.toHex(), s.toHex().uppercase(), Base64.getEncoder().encodeToString(s), s.copyOf(8).toHex())) {
                assertFalse("a secret appeared in output", all.contains(enc))
            }
        }
    }
}
