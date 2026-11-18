package cz.nowass.dispenser.crypto

import cz.nowass.dispenser.crypto.TestNative.hex
import cz.nowass.dispenser.crypto.TestNative.toHex
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** test-vectors/qr_formats_v1.txt: setup labels and DSP2I1 invitations (docs/ANDROID_BLE_DESIGN.md 7.1). */
class QrFormatsVectorTest {
    private val records = TestNative.records(TestNative.vectorFile("qr_formats_v1.txt"))
    private fun of(rec: String) = records.filter { it.first == rec }.map { it.second }

    @Test
    fun setup_labels() {
        for (kv in of("label_ok")) {
            val r = SetupLabel.parse(kv.getValue("label")) as SetupLabel.Result.Ok
            r.label.use { l ->
                assertEquals(kv["device_id"], l.deviceIdHex)
                l.setupCode.read { assertArrayEquals(hex(kv.getValue("code")), it) }
            }
            assertEquals(kv["label"], SetupLabel.format(kv.getValue("device_id"), hex(kv.getValue("code"))))
        }
        for (kv in of("label_entry")) {
            val r = SetupLabel.parse(kv.getValue("input").replace('_', ' '))
            assertTrue("${kv["name"]}: $r", r is SetupLabel.Result.Ok)
            (r as SetupLabel.Result.Ok).label.use { l ->
                assertEquals(kv["device_id"], l.deviceIdHex)
                l.setupCode.read { assertArrayEquals(kv["name"], hex(kv.getValue("code")), it) }
            }
        }
        for (kv in of("label_bad")) {
            val input = kv.getValue("input").let { if (it == "-") "" else it }
            assertEquals(kv["name"], SetupLabel.Result.Invalid(SetupLabel.Problem.valueOf(kv.getValue("reason").uppercase())), SetupLabel.parse(input))
        }
        assertEquals(4, of("label_ok").size)
        assertEquals(13, of("label_bad").size)
    }

    @Test
    fun invitations() {
        for (kv in of("invite_ok")) {
            val r = Invitation.parse(kv.getValue("qr")) as Invitation.Result.Ok
            r.invitation.use { inv ->
                assertArrayEquals(hex(kv.getValue("device_id")), inv.deviceId)
                assertArrayEquals(hex(kv.getValue("device_static_pub")), inv.deviceStaticPub)
                inv.inviteToken.read { assertArrayEquals(hex(kv.getValue("invite_token")), it) }
                assertEquals(kv.getValue("expires_utc").toLong(), inv.expiresUtc)
                assertEquals(kv.getValue("created_utc").toLong(), inv.createdUtc)
                assertEquals(kv["device_id_hash"]!!.removePrefix("x:"), inv.expectedDeviceIdHash.toHex())
                // the owner phone produces exactly the same text
                val again = Invitation.encode(inv.deviceId, inv.deviceStaticPub, inv.inviteToken, inv.createdUtc, (inv.expiresUtc - inv.createdUtc).toInt())
                assertEquals(kv["qr"], again)
            }
        }
        for (kv in of("invite_bad")) {
            assertEquals(kv["name"], Invitation.Result.Invalid(Invitation.Problem.valueOf(kv.getValue("reason").uppercase())), Invitation.parse(kv.getValue("input")))
        }
        for (kv in of("invite_time")) {
            val created = kv.getValue("created_utc").toLong()
            val expires = kv.getValue("expires_utc").toLong()
            val expect = kv.getValue("expect")
            if (expect == "invalid") {
                assertTrue(expires - created > Invitation.MAX_LIFETIME_S || created > expires)
                continue
            }
            val inv = Invitation(byteArrayOf(0, 0, 0, 0, 0, 1), ByteArray(32) { 1 }, SecretBytes.wrap(ByteArray(16)), expires, created)
            assertEquals(kv["name"], Invitation.Timing.valueOf(expect.uppercase()), inv.timing(kv.getValue("now_utc").toLong()))
            inv.close()
        }
        assertEquals(3, of("invite_ok").size)
        assertEquals(15, of("invite_bad").size)
    }

    /** The reference vector printed in docs/ANDROID_BLE_DESIGN.md 7.1 B. */
    @Test
    fun design_reference_vector() {
        val qr = of("invite_ok").first { it["name"] == "reference" }.getValue("qr")
        assertEquals("DSP2I1:06GB3GPKWKTJ08924CJ2A9H750MJMASC5MQ2YC1H68SK8D9P6WW3JEHV7GYKWFXTSM7TH1WYMKEH0K215Y9Y39YB324ZTTP0GVYPM73NTS8RP69ZW0F4AV7JEYV6188", qr)
    }
}
