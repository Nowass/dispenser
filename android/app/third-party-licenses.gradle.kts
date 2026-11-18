/*
 * Third-party license notices generated from the ACTUAL resolved release runtime classpath (A1.4b, D3).
 *
 * - Every module's POM is read; licenses missing there are taken from the nearest parent POM
 *   (resolved recursively), recording where they came from.
 * - BOM/platform modules (packaging "pom") ship no code and are excluded.
 * - A module whose license cannot be determined FAILS the task: unknown licenses are never
 *   assumed to be Apache-2.0.
 *
 * Resolving POMs (and parents) dynamically is not configuration-cache compatible, so this is an
 * explicit task writing a committed, reviewable asset; `checkThirdPartyLicenses` verifies it is
 * up to date with the current dependencies.
 *
 *   ./gradlew :app:generateThirdPartyLicenses   (after dependency changes)
 *   ./gradlew :app:checkThirdPartyLicenses      (verification)
 */
import org.gradle.api.artifacts.component.ModuleComponentIdentifier
import org.w3c.dom.Element
import javax.xml.parsers.DocumentBuilderFactory

data class PomLicense(val name: String, val url: String?)
data class Pom(val name: String?, val url: String?, val packaging: String?, val licenses: List<PomLicense>, val parent: String?)

val licensesAsset = layout.projectDirectory.file("src/main/assets/licenses/third_party_licenses.json").asFile

fun resolvePom(coordinates: String): File? = runCatching {
    configurations.detachedConfiguration(dependencies.create("$coordinates@pom")).apply { isTransitive = false }.singleFile
}.getOrNull()

fun Element.children(tag: String): List<Element> =
    (0 until childNodes.length).map { childNodes.item(it) }.filterIsInstance<Element>().filter { it.tagName == tag }

fun Element.text(tag: String): String? = children(tag).firstOrNull()?.textContent?.trim()?.takeIf { it.isNotEmpty() }

fun parsePom(file: File): Pom {
    val factory = DocumentBuilderFactory.newInstance().apply {
        isNamespaceAware = false
        setFeature("http://apache.org/xml/features/disallow-doctype-decl", true)
    }
    val root = factory.newDocumentBuilder().parse(file).documentElement
    val licenses = root.children("licenses").flatMap { it.children("license") }
        .map { PomLicense(it.text("name") ?: "", it.text("url")) }
        .filter { it.name.isNotEmpty() || it.url != null }
    val parent = root.children("parent").firstOrNull()?.let {
        val g = it.text("groupId"); val a = it.text("artifactId"); val v = it.text("version")
        if (g != null && a != null && v != null) "$g:$a:$v" else null
    }
    return Pom(root.text("name"), root.text("url"), root.text("packaging"), licenses, parent)
}

/** Known license identification (SPDX). Unrecognized names are kept verbatim, never guessed. */
fun spdxOf(l: PomLicense): String? {
    val n = l.name.lowercase(); val u = (l.url ?: "").lowercase()
    return when {
        n == "apache-2.0" || ("apache" in n && "2.0" in n) || "apache.org/licenses/license-2.0" in u -> "Apache-2.0"
        n == "mit" || n == "the mit license" || n == "mit license" || "opensource.org/licenses/mit" in u -> "MIT"
        n == "bsd-3-clause" || ("bsd" in n && ("3-clause" in n || "new" in n)) || "opensource.org/licenses/bsd-3-clause" in u -> "BSD-3-Clause"
        else -> null
    }
}

/** Generic full texts bundled under assets/licenses/texts/<SPDX>.txt (official sources). */
val genericLicenseTexts = setOf("Apache-2.0")

/** Licenses whose terms require reproducing a specific copyright notice per library. */
val attributionRequired = setOf("BSD-3-Clause", "MIT", "ISC")

/**
 * Native code compiled into the APK (M1.10e.1, libdspnoise) that is not a Gradle dependency: the
 * pinned noise-c and libsodium sources (firmware/third_party/noise-c/noise_c_deps.cmake). Their
 * upstream LICENSE files are bundled verbatim as notices.
 */
data class NativeComponent(val artifact: String, val version: String, val name: String, val url: String, val notice: String, val spdx: List<String>)
val nativeComponents = listOf(
    NativeComponent("noise-c", "0.1.31 (62c2fdca7a874ea898761147bb062ca1cde9f36d)", "Noise-C (ESPHome fork)",
        "https://github.com/esphome/noise-c", "notices/noise-c.txt", listOf("MIT")),
    NativeComponent("libsodium", "1.10021.12 (95c5e60d991b3f735619da8a307951c0483ef567; libsodium 1.0.21)", "libsodium (ESPHome packaging)",
        "https://github.com/esphome-libs/libsodium", "notices/libsodium.txt", listOf("ISC", "MIT")),
)

/**
 * Per-library notices (assets/licenses/notices/...), verbatim from the artifact and its upstream
 * project. Required for every library under an attribution-requiring license.
 */
val libraryNotices = mapOf(
    "androidx.datastore:datastore-preferences-external-protobuf" to "notices/datastore-preferences-external-protobuf.txt",
)

fun String.json(): String = buildString {
    append('"')
    for (c in this@json) when (c) {
        '"' -> append("\\\""); '\\' -> append("\\\\"); '\n' -> append("\\n"); '\r' -> append("\\r"); '\t' -> append("\\t")
        else -> if (c < ' ') append(String.format("\\u%04x", c.code)) else append(c)
    }
    append('"')
}

fun buildLicenseReport(): String {
    val modules = configurations.getByName("releaseRuntimeClasspath").incoming.resolutionResult.allComponents
        .mapNotNull { it.id as? ModuleComponentIdentifier }
        .distinctBy { "${it.group}:${it.module}:${it.version}" }
        .sortedWith(compareBy({ it.group }, { it.module }))
    val unknown = mutableListOf<String>()
    val entries = mutableListOf<String>()
    for (m in modules) {
        val coordinates = "${m.group}:${m.module}:${m.version}"
        val pomFile = resolvePom(coordinates) ?: run { unknown += "$coordinates (POM not resolvable)"; continue }
        val pom = parsePom(pomFile)
        if (pom.packaging == "pom") continue // BOM / platform: no shipped code
        var licenses = pom.licenses
        var source = "pom"
        var parent = pom.parent
        var depth = 0
        while (licenses.isEmpty() && parent != null && depth++ < 10) {
            val parentPom = resolvePom(parent)?.let(::parsePom) ?: break
            if (parentPom.licenses.isNotEmpty()) { licenses = parentPom.licenses; source = "parent $parent" }
            parent = parentPom.parent
        }
        if (licenses.isEmpty()) { unknown += coordinates; continue }
        val key = "${m.group}:${m.module}"
        val notice = libraryNotices[key]
        val spdxIds = licenses.map(::spdxOf)
        if (spdxIds.any { it == null || (it !in genericLicenseTexts && it !in attributionRequired) } && notice == null) {
            unknown += "$coordinates (license '${licenses.joinToString { it.name }}' has no bundled text or notice)"
            continue
        }
        if (spdxIds.any { it in attributionRequired } && notice == null) {
            unknown += "$coordinates (${spdxIds.joinToString()} requires a per-library notice)"
            continue
        }
        if (notice != null && !layout.projectDirectory.file("src/main/assets/licenses/$notice").asFile.exists()) {
            unknown += "$coordinates (notice file $notice missing)"
            continue
        }
        val licJson = licenses.joinToString(",") { l ->
            """{"spdx":${spdxOf(l)?.json() ?: "null"},"name":${l.name.json()},"url":${l.url?.json() ?: "null"}}"""
        }
        entries += """  {"group":${m.group.json()},"artifact":${m.module.json()},"version":${m.version.json()},""" +
            """"name":${(pom.name ?: m.module).json()},"url":${pom.url?.json() ?: "null"},"licenseSource":${source.json()},"notice":${notice?.json() ?: "null"},"licenses":[$licJson]}"""
    }
    for (n in nativeComponents) {
        if (!layout.projectDirectory.file("src/main/assets/licenses/${n.notice}").asFile.exists()) {
            unknown += "native ${n.artifact} (notice file ${n.notice} missing)"
            continue
        }
        val licJson = n.spdx.joinToString(",") { """{"spdx":${it.json()},"name":${it.json()},"url":null}""" }
        entries += """  {"group":"native","artifact":${n.artifact.json()},"version":${n.version.json()},""" +
            """"name":${n.name.json()},"url":${n.url.json()},"licenseSource":"upstream LICENSE file","notice":${n.notice.json()},"licenses":[$licJson]}"""
    }
    if (unknown.isNotEmpty()) {
        throw GradleException("License could not be determined for:\n  " + unknown.joinToString("\n  ") +
            "\nResolve manually; unknown licenses are never assumed.")
    }
    return "[\n" + entries.joinToString(",\n") + "\n]\n"
}

tasks.register("generateThirdPartyLicenses") {
    group = "licensing"
    description = "Writes src/main/assets/licenses/third_party_licenses.json from the resolved release runtime classpath."
    notCompatibleWithConfigurationCache("Resolves POM and parent-POM metadata dynamically.")
    doLast {
        licensesAsset.parentFile.mkdirs()
        licensesAsset.writeText(buildLicenseReport())
        logger.lifecycle("Wrote ${licensesAsset.relativeTo(rootDir)}")
    }
}

tasks.register("checkThirdPartyLicenses") {
    group = "verification"
    description = "Fails if the committed third-party license asset is missing or stale."
    notCompatibleWithConfigurationCache("Resolves POM and parent-POM metadata dynamically.")
    doLast {
        val expected = buildLicenseReport()
        if (!licensesAsset.exists() || licensesAsset.readText() != expected) {
            throw GradleException("third_party_licenses.json is stale. Run ./gradlew :app:generateThirdPartyLicenses")
        }
        logger.lifecycle("Third-party license asset is up to date.")
    }
}
