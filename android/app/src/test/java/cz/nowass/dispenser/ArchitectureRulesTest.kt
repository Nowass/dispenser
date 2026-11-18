package cz.nowass.dispenser

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Layering rules of docs/ANDROID_BLE_DESIGN.md 2.4, checked on the main sources:
 *  - the UI never touches Bluetooth, the native library or cryptographic primitives;
 *  - only the crypto package declares native (`external`) functions;
 *  - android.bluetooth is imported only by the GATT transport package (device/transport, M1.10e.2);
 *  - the crypto package never logs or prints (no channel through which a secret could leak).
 */
class ArchitectureRulesTest {
    private val root = File(checkNotNull(System.getProperty("dsp.source.dir")), "cz/nowass/dispenser")
    private val sources = root.walkTopDown().filter { it.isFile && it.extension == "kt" }.toList()
    private fun rel(f: File) = f.relativeTo(root).path

    @Test
    fun sources_found() {
        assertTrue(sources.size > 50)
        assertTrue(sources.any { rel(it).startsWith("crypto/") })
    }

    @Test
    fun ui_does_not_reach_bluetooth_or_crypto() {
        val bad = sources.filter { rel(it).startsWith("ui/") }.filter { f ->
            f.readLines().any { l -> l.startsWith("import android.bluetooth") || l.startsWith("import cz.nowass.dispenser.crypto") ||
                l.startsWith("import cz.nowass.dispenser.device.transport") || l.startsWith("import cz.nowass.dispenser.device.session") }
        }
        assertTrue("UI imports Bluetooth/crypto: ${bad.map(::rel)}", bad.isEmpty())
    }

    @Test
    fun native_methods_only_in_crypto() {
        val bad = sources.filter { f -> !rel(f).startsWith("crypto/") && f.readText().contains(Regex("\\bexternal\\s+fun\\b")) }
        assertTrue("native methods outside crypto/: ${bad.map(::rel)}", bad.isEmpty())
    }

    @Test
    fun bluetooth_only_in_the_transport_package() {
        val bad = sources.filter { f -> !rel(f).startsWith("device/transport/") && f.readLines().any { it.startsWith("import android.bluetooth") } }
        assertTrue("android.bluetooth outside device/transport: ${bad.map(::rel)}", bad.isEmpty())
    }

    @Test
    fun crypto_never_logs() {
        val forbidden = listOf("android.util.Log", "Log.d(", "Log.i(", "Log.w(", "Log.e(", "Log.v(", "println(", "printStackTrace", "System.out", "System.err")
        val bad = sources.filter { rel(it).startsWith("crypto/") }.filter { f -> forbidden.any { f.readText().contains(it) } }
        assertTrue("logging in crypto/: ${bad.map(::rel)}", bad.isEmpty())
    }
}
