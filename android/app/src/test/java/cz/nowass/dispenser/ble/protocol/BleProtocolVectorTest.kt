package cz.nowass.dispenser.ble.protocol

import cz.nowass.dispenser.ble.protocol.BleProtocol.Field
import cz.nowass.dispenser.ble.protocol.BleProtocol.Kind
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File

/**
 * Runs the shared BLE protocol vectors (`test-vectors/ble_protocol_v1.txt`), which the firmware
 * codec (`firmware/test/host/test_bleproto.c`) runs as well, plus property tests (canonical
 * re-encoding of every accepted mutated frame, rejection of every body truncation, random-input
 * fuzzing of the decoder and the reassembler).
 */
class BleProtocolVectorTest {

    private fun vectorFile(name: String): File {
        var dir: File? = File(System.getProperty("user.dir")!!).absoluteFile
        while (dir != null) {
            val f = File(dir, "test-vectors/$name")
            if (f.isFile) return f
            dir = dir.parentFile
        }
        error("test-vectors/$name not found above ${System.getProperty("user.dir")}")
    }

    private class Rec(val record: String, val keys: List<String>, val values: Map<String, String>) {
        fun get(k: String): String = values[k] ?: error("missing $k")
    }

    private fun parse(line: String): Rec {
        val tokens = line.trim().split(' ').filter { it.isNotEmpty() }
        val keys = mutableListOf<String>()
        val values = LinkedHashMap<String, String>()
        for (t in tokens.drop(1)) {
            val eq = t.indexOf('=')
            if (eq < 0) continue
            keys += t.substring(0, eq)
            values[t.substring(0, eq)] = t.substring(eq + 1)
        }
        return Rec(tokens[0], keys, values)
    }

    private fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 0xFF) }

    // ---- Canonical field text (same form as the vectors) ---------------------------------------

    @Suppress("UNCHECKED_CAST")
    private fun text(fields: List<Field>, values: Map<String, Any>, prefix: String, out: MutableList<String>) {
        for (fd in fields) {
            val key = prefix + fd.name
            val v = values[fd.name]
            when (val k = fd.kind) {
                is Kind.Reserved -> {}
                is Kind.Record -> text(k.fields, v as Map<String, Any>, "$key.", out)
                is Kind.ListOf -> {
                    val items = v as List<Map<String, Any>>
                    out += "$key.n=${items.size}"
                    items.forEachIndexed { i, item -> text(k.fields, item, "$key.$i.", out) }
                }
                is Kind.Bytes, Kind.Blob, is Kind.Str -> out += "$key=x:${(v as ByteArray).hex()}"
                Kind.Bool -> out += "$key=${if (v == true) 1 else 0}"
                else -> out += "$key=$v"
            }
        }
    }

    private fun fromText(fields: List<Field>, r: Rec, prefix: String): Map<String, Any> {
        val out = LinkedHashMap<String, Any>()
        for (fd in fields) {
            val key = prefix + fd.name
            when (val k = fd.kind) {
                is Kind.Reserved -> {}
                is Kind.Record -> out[fd.name] = fromText(k.fields, r, "$key.")
                is Kind.ListOf -> {
                    val n = r.get("$key.n").toInt()
                    out[fd.name] = List(n) { i -> fromText(k.fields, r, "$key.$i.") }
                }
                is Kind.Bytes, Kind.Blob, is Kind.Str -> out[fd.name] = hex(r.get(key).removePrefix("x:"))
                Kind.Bool -> out[fd.name] = r.get(key) == "1"
                Kind.U64 -> out[fd.name] = r.get(key).toULong()
                is Kind.Date -> {
                    val (y, m, d) = r.get(key).split('-').map { it.toInt() }
                    out[fd.name] = BleDate(y, m, d)
                }
                else -> out[fd.name] = r.get(key).toLong()
            }
        }
        return out
    }

    private fun flagsOf(kind: String) = when (kind) {
        "rsp" -> BleProtocol.F_RESPONSE
        "err" -> BleProtocol.F_RESPONSE or BleProtocol.F_ERROR
        "evt" -> BleProtocol.F_EVENT
        else -> 0
    }

    private fun result(block: () -> Unit): String = try {
        block()
        "ok"
    } catch (e: BleCodecException) {
        e.result.wire
    }

    private val validFrames = mutableListOf<Pair<ByteArray, Boolean>>() // frame, has blob body

    @Test
    fun sharedVectors() {
        val counts = sortedMapOf<String, Int>()
        vectorFile("ble_protocol_v1.txt").readLines().forEachIndexed { i, raw ->
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) return@forEachIndexed
            val r = parse(line)
            val where = "line ${i + 1} (${r.values["name"]})"
            when (r.record) {
                "msg" -> runMsg(r, where)
                "enc" -> {
                    val f = BleProtocol.decode(hex(r.get("frame")))
                    assertNull(where, f.fields)
                    assertTrue(where, f.header.flags and BleProtocol.F_ENCRYPTED != 0)
                }
                "bad" -> assertEquals(where, r.get("expect"), result { BleProtocol.decode(hex(r.get("frame"))) })
                "compat" -> assertEquals(where, if (r.get("accept") == "1") "ok" else "trailing",
                    result { BleProtocol.decode(hex(r.get("frame")), r.get("my_minor").toInt()) })
                "negotiate" -> {
                    val got = BleProtocol.negotiate(r.get("dev_min").toInt(), r.get("dev_max").toInt(),
                        r.get("cli_min").toInt(), r.get("cli_max").toInt())
                    assertEquals(where, r.get("expect"), got?.toString() ?: "none")
                }
                "frag" -> {
                    val frame = hex(r.get("frame"))
                    val want = r.get("fragments").split(',')
                    val got = BleProtocol.fragment(frame, r.get("mtu").toInt())
                    assertEquals(where, want, got.map { it.hex() })
                    val ra = BleProtocol.Reassembler()
                    var out: ByteArray? = null
                    got.forEachIndexed { k, frag -> out = ra.push(frag, k * 10L) }
                    assertArrayEquals(where, frame, out)
                }
                "reasm_bad", "reasm_ok" -> {
                    val frags = r.get("fragments").split(',').map { hex(it) }
                    val ms = r.get("ms").split(',').map { it.toLong() }
                    val ra = BleProtocol.Reassembler()
                    var out: ByteArray? = null
                    val res = result { frags.forEachIndexed { k, f -> out = ra.push(f, ms.getOrElse(k) { 0L }) } }
                    if (r.record == "reasm_ok") {
                        assertEquals(where, "ok", res)
                        assertArrayEquals(where, hex(r.get("frame")), out)
                    } else {
                        assertEquals(where, r.get("expect"), res)
                    }
                }
                "info" -> {
                    val rec = hex(r.get("record"))
                    val info = BleProtocol.decodeInfo(rec)
                    assertEquals(where, BleProtocol.Info(r.get("proto_min").toInt(), r.get("proto_max").toInt(),
                        r.get("claim_state").toInt(), r.get("flags").toInt(), r.get("caps").toLong(),
                        r.get("fw_major").toInt(), r.get("fw_minor").toInt(), r.get("fw_patch").toInt(),
                        r.get("device_tag").toInt()), info)
                    assertArrayEquals(where, rec, BleProtocol.encodeInfo(info))
                }
                "info_bad" -> assertEquals(where, r.get("expect"), result { BleProtocol.decodeInfo(hex(r.get("record"))) })
                "advert" -> {
                    val d = hex(r.get("data"))
                    val a = BleProtocol.decodeAdvert(d)
                    assertEquals(where, BleProtocol.Advert(r.get("proto_major").toInt(), r.get("flags").toInt(),
                        r.get("device_tag").toInt()), a)
                    assertArrayEquals(where, d, BleProtocol.encodeAdvert(a))
                }
                "advert_bad" -> assertEquals(where, r.get("expect"), result { BleProtocol.decodeAdvert(hex(r.get("data"))) })
                else -> fail("$where: unknown record ${r.record}")
            }
            counts[r.record] = (counts[r.record] ?: 0) + 1
        }
        println("vectors: $counts")
        assertTrue((counts["msg"] ?: 0) >= 59 && (counts["bad"] ?: 0) >= 56 && (counts["frag"] ?: 0) >= 6)
        propertyChecks()
    }

    private fun runMsg(r: Rec, where: String) {
        val frame = hex(r.get("frame"))
        val decoded = BleProtocol.decode(frame)
        val type = r.get("type").removePrefix("0x").toInt(16)
        val flags = flagsOf(r.get("kind"))
        assertEquals(where, type, decoded.header.type)
        assertEquals(where, flags, decoded.header.flags)
        assertEquals(where, r.get("txn").toInt(), decoded.header.txn)
        val layout = BleProtocol.layoutFor(type, flags)!!
        val got = mutableListOf<String>().also { text(layout, decoded.fields!!, "", it) }
        val want = r.keys.filter { it !in setOf("name", "kind", "type", "txn", "frame") }.map { "$it=${r.get(it)}" }
        assertEquals(where, want, got)
        val encoded = BleProtocol.encode(type, flags, r.get("txn").toInt(), fromText(layout, r, ""))
        assertEquals(where, frame.hex(), encoded.hex())
        validFrames += frame to (type == 0x02)
    }

    private fun canonical(frame: ByteArray): Boolean {
        val f = try {
            BleProtocol.decode(frame)
        } catch (e: BleCodecException) {
            return false
        }
        if (f.fields == null) return false
        val again = BleProtocol.encode(f.header.type, f.header.flags, f.header.txn, f.fields!!, f.header.minor)
        assertEquals("canonical re-encoding", frame.hex(), again.hex())
        return true
    }

    private fun propertyChecks() {
        // Every body truncation is rejected (blob bodies accept any length; skip them).
        var truncations = 0
        for ((frame, blob) in validFrames) {
            if (blob || frame.size == BleProtocol.HEADER_LEN) continue
            for (cut in BleProtocol.HEADER_LEN until frame.size) {
                val t = frame.copyOf(cut)
                val n = cut - BleProtocol.HEADER_LEN
                t[6] = n.toByte()
                t[7] = (n shr 8).toByte()
                assertTrue("truncation accepted", result { BleProtocol.decode(t) } != "ok")
                truncations++
            }
        }
        // Bit flips: every accepted mutation re-encodes canonically.
        var flips = 0
        var accepted = 0
        for ((frame, _) in validFrames) {
            for (pos in frame.indices) for (bit in 0 until 8) {
                val m = frame.copyOf()
                m[pos] = (m[pos].toInt() xor (1 shl bit)).toByte()
                if (canonical(m)) accepted++
                flips++
            }
        }
        // Random inputs and fragment streams never crash (only BleCodecException).
        var seed = 0x1234567
        fun rnd(): Int {
            seed = seed xor (seed shl 13); seed = seed xor (seed ushr 17); seed = seed xor (seed shl 5)
            return seed and 0x7FFFFFFF
        }
        repeat(50_000) {
            val n = rnd() % (BleProtocol.FRAME_MAX + 32)
            val b = ByteArray(n) { rnd().toByte() }
            if (n >= 8 && rnd() % 2 == 0) {
                b[0] = 1; b[1] = 0; b[3] = (b[3].toInt() and 0x07).toByte()
                b[6] = (n - 8).toByte(); b[7] = ((n - 8) shr 8).toByte()
            }
            canonical(b)
        }
        val ra = BleProtocol.Reassembler()
        var now = 0L
        repeat(50_000) {
            val b = ByteArray(rnd() % 40) { rnd().toByte() }
            now += rnd() % 700
            result { ra.push(b, now) }
        }
        println("properties: $truncations truncations rejected, $flips bit flips ($accepted accepted, canonical)")
        assertTrue(truncations >= 1000)
    }

    @Test
    fun encoderRejectsInvalidValues() {
        assertEquals("bad_enum", result { BleProtocol.encode(0x43, 0, 1, mapOf("readiness" to 3L)) })
        assertEquals("bad_string", result { BleProtocol.encode(0x31, 0, 1, mapOf("id" to "a b".toByteArray())) })
        assertEquals("not_a_request", result { BleProtocol.encode(0x82, 0, 1, emptyMap()) })
        assertEquals("too_large", result {
            BleProtocol.encode(0x02, 0, 1, mapOf("purpose" to 1L, "msg_index" to 0L, "noise" to ByteArray(495)))
        })
        val max = BleProtocol.encode(0x02, 0, 1, mapOf("purpose" to 1L, "msg_index" to 0L, "noise" to ByteArray(494)))
        assertEquals(BleProtocol.HEADER_LEN + BleProtocol.PLAIN_BODY_MAX, max.size)
        assertEquals(BleProtocol.SecurityClass.PLAIN, BleProtocol.TYPES[0x01]!!.cls)
        assertEquals(BleProtocol.SecurityClass.ENCRYPTED, BleProtocol.TYPES[0x40]!!.cls)
        assertEquals(BleProtocol.SecurityClass.EITHER, BleProtocol.TYPES[0x7E]!!.cls)
    }
}
