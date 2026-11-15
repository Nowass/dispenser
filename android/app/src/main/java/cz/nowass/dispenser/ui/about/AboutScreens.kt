package cz.nowass.dispenser.ui.about

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.about.AssetTextSource
import cz.nowass.dispenser.data.about.Changelog
import cz.nowass.dispenser.data.about.ChangelogParser
import cz.nowass.dispenser.data.about.LicenseCatalog
import cz.nowass.dispenser.data.about.ThirdPartyLibrary
import cz.nowass.dispenser.data.update.GitHubReleaseParser
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.update.ReleaseChannel
import cz.nowass.dispenser.domain.update.UpdateCheckResult
import cz.nowass.dispenser.ui.Format
import cz.nowass.dispenser.ui.components.DetailTopBar
import cz.nowass.dispenser.ui.components.StatusTag
import cz.nowass.dispenser.ui.czechPluralStringResource
import cz.nowass.dispenser.ui.theme.DispenserTheme
import cz.nowass.dispenser.ui.theme.Spacing
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

const val ABOUT_CHECK_UPDATES_TEST_TAG = "about_check_updates"
const val ABOUT_WHATS_NEW_TEST_TAG = "about_whats_new"
const val ABOUT_LICENSES_TEST_TAG = "about_licenses"

@Composable
private fun SectionCard(content: @Composable ColumnScope.() -> Unit) {
    Card(
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.fillMaxWidth(),
    ) { Column(content = content) }
}

@Composable
private fun itemColors() = ListItemDefaults.colors(
    containerColor = MaterialTheme.colorScheme.surface,
    supportingColor = MaterialTheme.colorScheme.onSurfaceVariant,
)

/** Opens only official https://github.com pages (release page, repository). */
@Composable
private fun rememberGitHubOpener(): (String) -> Unit {
    val uri = LocalUriHandler.current
    return { url -> if (GitHubReleaseParser.isOfficialGitHubPage(url)) uri.openUri(url) }
}

// ---- About ----

const val ABOUT_APP_LICENSE_TEST_TAG = "about_app_license"

/** Bundled official Apache License 2.0 text: the license of this application (A1.5). */
const val APP_LICENSE_TEXT_PATH = "licenses/texts/Apache-2.0.txt"

@Composable
fun AboutRoute(
    viewModel: AboutViewModel,
    isDemo: Boolean,
    onBack: () -> Unit,
    onOpenChangelog: () -> Unit,
    onOpenLicenses: () -> Unit,
    onOpenAppLicense: () -> Unit = {},
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    AboutScreen(state, isDemo, onBack, onOpenChangelog, onOpenLicenses, viewModel::checkForUpdates, rememberGitHubOpener(), onOpenAppLicense)
}

@Composable
fun AboutScreen(
    state: AboutUiState,
    isDemo: Boolean,
    onBack: () -> Unit,
    onOpenChangelog: () -> Unit,
    onOpenLicenses: () -> Unit,
    onCheckForUpdates: () -> Unit,
    onOpenUrl: (String) -> Unit,
    /** Opens the bundled Apache License 2.0 text (the app's own license). */
    onOpenAppLicense: () -> Unit = {},
) {
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.about_title), isDemo, onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            Header(state)
            SectionCard {
                ListItem(
                    headlineContent = { Text(stringResource(R.string.about_whats_new)) },
                    colors = itemColors(),
                    modifier = Modifier.testTag(ABOUT_WHATS_NEW_TEST_TAG).clickable(onClick = onOpenChangelog),
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                UpdateItem(state, onCheckForUpdates, onOpenUrl)
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.about_licenses)) },
                    colors = itemColors(),
                    modifier = Modifier.testTag(ABOUT_LICENSES_TEST_TAG).clickable(onClick = onOpenLicenses),
                )
                state.sourceCodeUrl?.let { url ->
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.about_source)) },
                        supportingContent = { Text(url.removePrefix("https://")) },
                        colors = itemColors(),
                        modifier = Modifier.clickable { onOpenUrl(url) },
                    )
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.about_app_license)) },
                    supportingContent = { Text(stringResource(R.string.licenses_open_text)) },
                    colors = itemColors(),
                    modifier = Modifier.testTag(ABOUT_APP_LICENSE_TEST_TAG).clickable(onClick = onOpenAppLicense),
                )
            }
            Text(stringResource(R.string.about_privacy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (isDemo) Text(stringResource(R.string.about_demo_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(Spacing.m))
        }
    }
}

@Composable
private fun Header(state: AboutUiState) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.m)) {
        // Launcher artwork: adaptive-icon foreground on the brand background (decorative).
        Box(
            Modifier.size(72.dp).clip(CircleShape).background(MaterialTheme.colorScheme.background),
            contentAlignment = Alignment.Center,
        ) {
            Image(painterResource(R.drawable.ic_launcher_foreground), contentDescription = null, modifier = Modifier.size(108.dp))
        }
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(stringResource(R.string.app_name), style = MaterialTheme.typography.headlineMedium, modifier = Modifier.semantics { heading() })
            Text(stringResource(R.string.about_version, state.version.name), style = MaterialTheme.typography.bodyLarge)
            Text(stringResource(R.string.about_build, state.version.code.toInt()), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            state.channel?.let {
                Text(
                    stringResource(if (it == ReleaseChannel.BETA) R.string.about_channel_beta else R.string.about_channel_stable),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
    Text(stringResource(R.string.about_description), style = MaterialTheme.typography.bodyMedium)
}

@Composable
private fun UpdateItem(state: AboutUiState, onCheck: () -> Unit, onOpenUrl: (String) -> Unit) {
    val update = state.update
    Column(
        Modifier
            .fillMaxWidth()
            .testTag(ABOUT_CHECK_UPDATES_TEST_TAG)
            .clickable(enabled = update != UpdateUiState.Checking, onClick = onCheck)
            .padding(horizontal = Spacing.m, vertical = Spacing.m),
        verticalArrangement = Arrangement.spacedBy(Spacing.xs),
    ) {
        Text(stringResource(R.string.about_check_updates), style = MaterialTheme.typography.bodyLarge)
        val (message, url) = updateMessage(update)
        if (update == UpdateUiState.Checking) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text(stringResource(R.string.update_checking), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else if (message != null) {
            Text(message, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        }
        if (url != null) {
            FilledTonalButton(onClick = { onOpenUrl(url) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.update_open_release)) }
            Text(stringResource(R.string.update_manual_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** Message and optional release-page URL for an update state. */
@Composable
private fun updateMessage(update: UpdateUiState): Pair<String?, String?> = when (update) {
    UpdateUiState.Idle, UpdateUiState.Checking -> null to null
    UpdateUiState.UnsupportedVersion -> stringResource(R.string.update_unsupported_version) to null
    is UpdateUiState.Done -> when (val r = update.result) {
        is UpdateCheckResult.UpToDate ->
            (r.latest?.let { stringResource(R.string.update_up_to_date_latest, it.toString()) } ?: stringResource(R.string.update_up_to_date)) to null
        is UpdateCheckResult.UpdateAvailable ->
            stringResource(if (r.isPreRelease) R.string.update_available_beta else R.string.update_available, r.version.toString()) to r.releasePageUrl
        is UpdateCheckResult.NewerReleaseWithoutApk -> stringResource(R.string.update_without_apk, r.version.toString()) to r.releasePageUrl
        UpdateCheckResult.NoReleases -> stringResource(R.string.update_no_releases) to null
        UpdateCheckResult.RepositoryUnavailable -> stringResource(R.string.update_repo_unavailable) to null
        UpdateCheckResult.Offline -> stringResource(R.string.update_offline) to null
        is UpdateCheckResult.RateLimited ->
            (r.resetAt?.let { stringResource(R.string.update_rate_limited_until, Format.time(it.atZone(DEFAULT_ZONE))) } ?: stringResource(R.string.update_rate_limited)) to null
        UpdateCheckResult.InvalidMetadata -> stringResource(R.string.update_invalid) to null
        is UpdateCheckResult.HttpError -> stringResource(R.string.update_http_error, r.code) to null
    }
}

// ---- Changelog ----

@Composable
fun ChangelogRoute(assets: AssetTextSource, versionName: String, isDemo: Boolean, onBack: () -> Unit) {
    val changelog by produceState<Changelog?>(initialValue = null) {
        value = withContext(Dispatchers.IO) { assets.read("CHANGELOG.md")?.let(ChangelogParser::parse) }
    }
    ChangelogScreen(changelog, versionName, isDemo, onBack)
}

@Composable
fun ChangelogScreen(changelog: Changelog?, versionName: String, isDemo: Boolean, onBack: () -> Unit) {
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.about_whats_new), isDemo, onBack)
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.m),
        ) {
            if (changelog == null || changelog.sections.isEmpty()) {
                Text(stringResource(R.string.changelog_unavailable), color = MaterialTheme.colorScheme.onSurfaceVariant)
                return@Column
            }
            changelog.intro.forEach { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            val relevant = changelog.relevantFor(versionName)
            (listOfNotNull(relevant) + changelog.sections.filter { it != relevant }).forEach { section ->
                SectionCard {
                    Column(Modifier.padding(Spacing.m), verticalArrangement = Arrangement.spacedBy(Spacing.s)) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                            Text(section.title, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f, fill = false).semantics { heading() })
                            if (section == relevant) StatusTag(stringResource(R.string.changelog_current))
                        }
                        section.groups.forEach { group ->
                            Text(group.title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
                            group.items.forEach { item ->
                                Row(horizontalArrangement = Arrangement.spacedBy(Spacing.s)) {
                                    Text("•", color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    Text(item, style = MaterialTheme.typography.bodyMedium)
                                }
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(Spacing.m))
        }
    }
}

// ---- Licenses ----

@Composable
fun LicensesRoute(assets: AssetTextSource, isDemo: Boolean, onBack: () -> Unit, onOpenText: (title: String, path: String) -> Unit) {
    val libraries by produceState<List<ThirdPartyLibrary>?>(initialValue = null) {
        value = withContext(Dispatchers.IO) { assets.read(LicenseCatalog.DATA_PATH)?.let(LicenseCatalog::parse) }
    }
    LicensesScreen(libraries, isDemo, onBack, onOpenText)
}

/** Text asset for a library: its notice, else the generic text of its first SPDX license. */
fun ThirdPartyLibrary.textPath(): String? = noticePath ?: licenses.firstNotNullOfOrNull { it.spdx }?.let(LicenseCatalog::textPath)

@Composable
fun LicensesScreen(libraries: List<ThirdPartyLibrary>?, isDemo: Boolean, onBack: () -> Unit, onOpenText: (String, String) -> Unit) {
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(stringResource(R.string.about_licenses), isDemo, onBack)
        if (libraries == null) {
            Text(stringResource(R.string.licenses_unavailable), color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(Spacing.m))
            return@Column
        }
        val bySpdx = libraries.flatMap { lib -> lib.licenses.mapNotNull { it.spdx }.distinct().map { it to lib } }
            .groupBy({ it.first }, { it.second })
            .toSortedMap()
        LazyColumn(
            contentPadding = PaddingValues(horizontal = Spacing.m, vertical = Spacing.s),
            verticalArrangement = Arrangement.spacedBy(Spacing.s),
        ) {
            item { Text(stringResource(R.string.licenses_intro), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            item { Text(stringResource(R.string.licenses_material_icons_note), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            item { Text(stringResource(R.string.licenses_texts_title), style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = Spacing.s).semantics { heading() }) }
            items(bySpdx.entries.filter { it.key == "Apache-2.0" || it.key == "MIT" }.toList(), key = { "spdx-" + it.key }) { (spdx, libs) ->
                SectionCard {
                    ListItem(
                        headlineContent = { Text(libs.first().licenses.first { it.spdx == spdx }.name) },
                        supportingContent = { Text(spdx + " · " + czechPluralStringResource(R.plurals.licenses_library_count, libs.size, libs.size)) },
                        colors = itemColors(),
                        modifier = Modifier.clickable(onClickLabel = stringResource(R.string.licenses_open_text)) { onOpenText(spdx, LicenseCatalog.textPath(spdx)) },
                    )
                }
            }
            item {
                Text(
                    stringResource(R.string.licenses_libraries_title) + " (" + czechPluralStringResource(R.plurals.licenses_library_count, libraries.size, libraries.size) + ")",
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(top = Spacing.s).semantics { heading() },
                )
            }
            items(libraries, key = { it.group + ":" + it.artifact }) { lib ->
                val path = lib.textPath()
                SectionCard {
                    ListItem(
                        headlineContent = { Text(lib.name) },
                        supportingContent = {
                            Column {
                                Text("${lib.group}:${lib.artifact}:${lib.version}", style = MaterialTheme.typography.bodySmall)
                                Text(lib.licenses.joinToString { it.spdx ?: it.name }, style = MaterialTheme.typography.bodySmall)
                            }
                        },
                        colors = itemColors(),
                        modifier = if (path != null) {
                            Modifier.clickable(onClickLabel = stringResource(R.string.licenses_open_text)) { onOpenText(lib.name, path) }
                        } else {
                            Modifier
                        },
                    )
                }
            }
        }
    }
}

@Composable
fun LicenseTextRoute(assets: AssetTextSource, title: String, path: String, isDemo: Boolean, onBack: () -> Unit) {
    val text by produceState<String?>(initialValue = null, path) {
        value = if (LicenseCatalog.isAllowedTextPath(path)) withContext(Dispatchers.IO) { assets.read(path) } else null
    }
    Column(Modifier.fillMaxSize()) {
        DetailTopBar(title.ifBlank { stringResource(R.string.license_text_title) }, isDemo, onBack)
        Text(
            text ?: stringResource(R.string.licenses_unavailable),
            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(Spacing.m),
        )
    }
}

// ---- Previews ----

@Preview(name = "About 412", widthDp = 412, heightDp = 900, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun AboutPreview() {
    DispenserTheme {
        AboutScreen(
            AboutUiState(AppVersion("0.4.0", 4), ReleaseChannel.STABLE, sourceCodeUrl = null, update = UpdateUiState.Done(UpdateCheckResult.RepositoryUnavailable)),
            isDemo = true, onBack = {}, onOpenChangelog = {}, onOpenLicenses = {}, onCheckForUpdates = {}, onOpenUrl = {},
        )
    }
}
