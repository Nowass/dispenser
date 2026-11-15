package cz.nowass.dispenser

import android.app.Application
import android.content.Context
import androidx.core.content.pm.PackageInfoCompat
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.preferencesDataStoreFile
import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.data.DataStoreAppSettingsRepository
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.about.AssetTextSource
import cz.nowass.dispenser.data.update.GitHubReleasesUpdateChecker
import cz.nowass.dispenser.data.update.UpdateChecker
import cz.nowass.dispenser.data.SimulationController
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.data.local.simulation.RoomSimulationStore
import cz.nowass.dispenser.data.local.simulation.SimulationDatabase
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import java.time.Clock

/** Manual dependency container; lives as long as the process. */
class AppContainer(context: Context) {
    /** Device values (battery, reservoir, history) are still simulated, so the DEMO badge stays. */
    val isDemo: Boolean = true

    /** Phone clock in the schedule timezone; tests and previews inject fixed clocks instead. */
    val clock: Clock = Clock.system(DEFAULT_ZONE)

    /** User schedules (never touched by the simulation). A fresh install starts empty. */
    private val database: DispenserDatabase by lazy { DispenserDatabase.build(context) }

    /** Separate simulation database: state + history of simulated runs only. */
    private val simulationDatabase: SimulationDatabase by lazy { SimulationDatabase.build(context) }

    private val localRepository: LocalDispenserRepository by lazy {
        LocalDispenserRepository(
            store = RoomScheduleStore(database.scheduleDao(), clock),
            clock = clock,
            simulationStore = RoomSimulationStore(simulationDatabase.simulationDao()),
        )
    }

    val repository: DispenserRepository get() = localRepository

    /** DEMO-only simulation controls (Settings); not part of the device contract. */
    val simulation: SimulationController get() = localRepository.simulation

    /** Presentation preferences (display name) in their own DataStore file; no DB involvement. */
    val appSettings: AppSettingsRepository by lazy {
        DataStoreAppSettingsRepository(
            PreferenceDataStoreFactory.create { context.applicationContext.preferencesDataStoreFile(DataStoreAppSettingsRepository.FILE_NAME) },
        )
    }

    /** Public GitHub Releases source (owner/repo from gradle.properties via resources; no credentials). */
    private val githubOwner: String by lazy { context.getString(R.string.github_owner) }
    private val githubRepo: String by lazy { context.getString(R.string.github_repo) }

    /** Repository page, or null while the source link is disabled (repository still private, D5). */
    val sourceCodeUrl: String? by lazy {
        if (context.resources.getBoolean(R.bool.github_source_link_enabled)) "https://github.com/$githubOwner/$githubRepo" else null
    }

    /** User-initiated update check; only public release metadata is requested. */
    val updateChecker: UpdateChecker by lazy {
        GitHubReleasesUpdateChecker(githubOwner, githubRepo, userAgent = "Dispenser-Android/${appVersion.name}")
    }

    /** Bundled assets: CHANGELOG.md, license data and texts. */
    val assets: AssetTextSource by lazy {
        val am = context.applicationContext.assets
        AssetTextSource { path -> runCatching { am.open(path).bufferedReader().use { it.readText() } }.getOrNull() }
    }

    val appVersion: AppVersion by lazy {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        AppVersion(name = info.versionName ?: "?", code = PackageInfoCompat.getLongVersionCode(info))
    }
}

class DispenserApplication : Application() {
    val container: AppContainer by lazy { AppContainer(this) }
}
