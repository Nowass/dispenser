package cz.nowass.dispenser.crypto

import cz.nowass.dispenser.crypto.TestNative.hex
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Noise CONFORMANCE: the official vectors shipped with the pinned noise-c (cacophony.txt,
 * noise-c-basic.txt) for Noise_XXpsk3_25519_ChaChaPoly_SHA256 and Noise_IK_25519_ChaChaPoly_SHA256,
 * through the JNI binding (host build). Kept separate from the Dispenser framing vectors.
 */
class OfficialNoiseVectorsTest {
    init {
        TestNative.load()
    }

    @Test
    fun xxpsk3_and_ik_official_vectors() {
        var xx = 0
        var ik = 0
        for (file in listOf("cacophony.txt", "noise-c-basic.txt")) {
            val root = Json.parseToJsonElement(TestNative.noiseVectorFile(file).readText()).jsonObject
            for (v in root.getValue("vectors").jsonArray.map { it.jsonObject }) {
                val name = (v["protocol_name"] ?: v["name"])?.jsonPrimitive?.content
                when (name) {
                    "Noise_XXpsk3_25519_ChaChaPoly_SHA256" -> { run(v, true); xx++ }
                    "Noise_IK_25519_ChaChaPoly_SHA256" -> { run(v, false); ik++ }
                }
            }
        }
        assertTrue("XXpsk3 $xx, IK $ik", xx >= 1 && ik >= 2) // 0 skipped
    }

    private fun s(v: JsonObject, k: String) = v[k]?.jsonPrimitive?.content?.let { hex(it) }
    private fun psk(v: JsonObject, k: String) = v[k]?.jsonArray?.firstOrNull()?.jsonPrimitive?.content?.let { hex(it) }

    private fun run(v: JsonObject, xx: Boolean) {
        val i = NativeNoiseTestHooks.handshake(xx, true, s(v, "init_prologue")!!, s(v, "init_static")!!, s(v, "init_remote_static"), psk(v, "init_psks"), s(v, "init_ephemeral"))
        val r = NativeNoiseTestHooks.handshake(xx, false, s(v, "resp_prologue")!!, s(v, "resp_static")!!, null, psk(v, "resp_psks"), s(v, "resp_ephemeral"))
        var ic: TransportCiphers? = null
        var rc: TransportCiphers? = null
        v.getValue("messages").jsonArray.forEachIndexed { idx, m ->
            val payload = hex(m.jsonObject.getValue("payload").jsonPrimitive.content)
            val ct = hex(m.jsonObject.getValue("ciphertext").jsonPrimitive.content)
            val fromInit = idx % 2 == 0
            if (ic == null) {
                val (w, rd) = if (fromInit) i to r else r to i
                assertArrayEquals(ct, w.writeMessage(payload))
                assertArrayEquals(payload, rd.readMessage(ct))
                if (i.isFinished) {
                    s(v, "handshake_hash")?.let { assertArrayEquals(it, i.handshakeHash()); assertArrayEquals(it, r.handshakeHash()) }
                    ic = i.split()
                    rc = r.split()
                }
            } else {
                val (w, rd) = if (fromInit) ic!! to rc!! else rc!! to ic!!
                assertArrayEquals(ct, w.encrypt(ByteArray(0), payload))
                assertArrayEquals(payload, rd.decrypt(ByteArray(0), ct))
            }
        }
        assertTrue(ic != null)
        ic?.close()
        rc?.close()
    }
}
