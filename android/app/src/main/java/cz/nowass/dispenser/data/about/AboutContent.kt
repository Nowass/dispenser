package cz.nowass.dispenser.data.about

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull

/** Reads bundled text assets (CHANGELOG.md, license data and texts). */
fun interface AssetTextSource {
    /** Contents of [path], or null if absent. */
    fun read(path: String): String?
}

// ---- Changelog (CHANGELOG.md, Keep a Changelog subset) ----

data class ChangelogGroup(val title: String, val items: List<String>)

data class ChangelogSection(
    /** Heading text without brackets, e.g. "Nevydáno – vývojové milníky" or "0.5.0 – 2027-01-10". */
    val title: String,
    /** Version inside the brackets ("Nevydáno" or "0.5.0"). */
    val label: String,
    val isUnreleased: Boolean,
    val groups: List<ChangelogGroup>,
)

data class Changelog(val intro: List<String>, val sections: List<ChangelogSection>) {
    /** Section for the installed version if released, else the unreleased section. */
    fun relevantFor(versionName: String): ChangelogSection? =
        sections.firstOrNull { !it.isUnreleased && it.label == versionName } ?: sections.firstOrNull { it.isUnreleased }
}

object ChangelogParser {
    private val SECTION = Regex("^##\\s+\\[([^]]+)](.*)$")
    private val LINK = Regex("\\[([^]]+)]\\([^)]+\\)")

    fun parse(markdown: String): Changelog {
        val intro = mutableListOf<String>()
        val sections = mutableListOf<ChangelogSection>()
        var label: String? = null
        var title = ""
        val groups = mutableListOf<ChangelogGroup>()
        var groupTitle: String? = null
        var items = mutableListOf<String>()

        fun flushGroup() {
            if (groupTitle != null) groups += ChangelogGroup(groupTitle!!, items.toList())
            groupTitle = null; items = mutableListOf()
        }
        fun flushSection() {
            flushGroup()
            label?.let { sections += ChangelogSection(title, it, it.equals("Nevydáno", true) || it.equals("Unreleased", true), groups.toList()) }
            groups.clear(); label = null
        }

        for (raw in markdown.lines()) {
            val line = raw.trimEnd()
            val section = SECTION.matchEntire(line)
            when {
                section != null -> {
                    flushSection()
                    label = section.groupValues[1].trim()
                    title = (label + section.groupValues[2]).trim()
                }
                line.startsWith("### ") && label != null -> { flushGroup(); groupTitle = line.removePrefix("### ").trim() }
                line.startsWith("- ") && groupTitle != null -> items += clean(line.removePrefix("- "))
                line.startsWith("# ") -> Unit
                label == null && line.isNotBlank() -> intro += clean(line)
            }
        }
        flushSection()
        return Changelog(intro, sections)
    }

    /** Markdown links -> their text; drop emphasis markers. */
    private fun clean(text: String): String = LINK.replace(text) { it.groupValues[1] }.replace("**", "").trim()
}

// ---- Third-party licenses (generated asset) ----

data class LicenseRef(val spdx: String?, val name: String, val url: String?)

data class ThirdPartyLibrary(
    val group: String,
    val artifact: String,
    val version: String,
    val name: String,
    val url: String?,
    val licenses: List<LicenseRef>,
    /** Per-library notice asset path (licenses/...), if any. */
    val noticePath: String?,
)

object LicenseCatalog {
    const val DATA_PATH = "licenses/third_party_licenses.json"

    /** Asset path of a generic license text, e.g. licenses/texts/Apache-2.0.txt. */
    fun textPath(spdx: String) = "licenses/texts/$spdx.txt"

    fun parse(json: String): List<ThirdPartyLibrary>? {
        val root = runCatching { Json.parseToJsonElement(json) }.getOrNull() as? JsonArray ?: return null
        return root.map { e ->
            val o = e as? JsonObject ?: return null
            fun s(k: String) = (o[k] as? JsonPrimitive)?.contentOrNull
            ThirdPartyLibrary(
                group = s("group") ?: return null,
                artifact = s("artifact") ?: return null,
                version = s("version") ?: return null,
                name = s("name") ?: s("artifact")!!,
                url = s("url"),
                licenses = (o["licenses"] as? JsonArray).orEmpty().mapNotNull { l ->
                    val lo = l as? JsonObject ?: return@mapNotNull null
                    LicenseRef(
                        spdx = (lo["spdx"] as? JsonPrimitive)?.contentOrNull,
                        name = (lo["name"] as? JsonPrimitive)?.contentOrNull ?: return@mapNotNull null,
                        url = (lo["url"] as? JsonPrimitive)?.contentOrNull,
                    )
                },
                noticePath = s("notice")?.let { "licenses/$it" },
            )
        }
    }

    /** Only bundled license texts/notices may be opened (no arbitrary asset paths). */
    fun isAllowedTextPath(path: String): Boolean =
        !path.contains("..") && (path.startsWith("licenses/texts/") || path.startsWith("licenses/notices/")) && path.endsWith(".txt")
}
