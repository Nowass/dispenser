package cz.nowass.dispenser.crypto

import java.io.File

/** Loads the host test build of libdspnoise (built by Gradle's buildHostNativeTest) — never skipped. */
object TestNative {
    fun load(): NativeNoiseProvider {
        val path = checkNotNull(System.getProperty("dsp.native.hostlib")) {
            "dsp.native.hostlib not set: run the tests through Gradle (buildHostNativeTest)"
        }
        check(File(path).isFile) { "host native test library missing: $path" }
        NativeNoise.libraryLoader = { System.load(path) }
        val p = NativeNoiseProvider()
        check(NativeNoiseTestHooks.isTestBuild())
        return p
    }

    fun vectorFile(name: String): File = File(checkNotNull(System.getProperty("dsp.vectors.dir")), name).also {
        check(it.isFile) { "missing vector file $it" }
    }

    fun noiseVectorFile(name: String): File = File(checkNotNull(System.getProperty("dsp.noise.vectors.dir")), name).also {
        check(it.isFile) { "missing official vector file $it" }
    }

    fun hex(s: String): ByteArray {
        val t = s.removePrefix("x:")
        if (t == "-") return ByteArray(0)
        require(t.length % 2 == 0)
        return ByteArray(t.length / 2) { t.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }

    fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }

    /** "rec k=v k=v" records of a vector file. */
    fun records(file: File): List<Pair<String, Map<String, String>>> =
        file.readLines().filter { it.isNotBlank() && !it.startsWith("#") }.map { line ->
            val parts = line.trim().split(Regex("\\s+"))
            parts[0] to parts.drop(1).associate { t -> t.substringBefore('=') to t.substringAfter('=') }
        }
}
