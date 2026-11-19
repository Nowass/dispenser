package cz.nowass.dispenser

import android.app.Application
import android.content.Context
import android.util.Log
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
import cz.nowass.dispenser.data.backup.BackupService
import cz.nowass.dispenser.data.backup.ContentResolverDocumentIo
import cz.nowass.dispenser.data.backup.DirectoryRestoreFiles
import cz.nowass.dispenser.data.backup.DocumentIo
import cz.nowass.dispenser.data.backup.StorageState
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.data.local.simulation.RoomSimulationStore
import cz.nowass.dispenser.data.local.simulation.SimulationDatabase
import cz.nowass.dispenser.data.device.CredentialSeqAllocator
import cz.nowass.dispenser.data.device.DeviceDispenserRepository
import cz.nowass.dispenser.data.device.DeviceUiRepository
import cz.nowass.dispenser.data.device.UnavailablePhoneTimeSource
import cz.nowass.dispenser.data.device.local.DeviceDatabase
import cz.nowass.dispenser.data.device.local.RoomDeviceCacheStore
import cz.nowass.dispenser.data.device.local.RoomPendingOpsJournal
import cz.nowass.dispenser.data.mode.AppModeController
import cz.nowass.dispenser.data.mode.DataStoreAppModeStore
import cz.nowass.dispenser.data.mode.ResolvedMode
import cz.nowass.dispenser.crypto.NativeNoiseProvider
import cz.nowass.dispenser.crypto.credentials.AndroidKeystoreKeyWrapper
import cz.nowass.dispenser.crypto.credentials.CredentialStore
import cz.nowass.dispenser.device.ops.ManagerSessionPort
import cz.nowass.dispenser.device.session.DeviceSessionManager
import cz.nowass.dispenser.device.transport.AndroidBluetoothPrerequisites
import cz.nowass.dispenser.device.transport.AndroidGattPortFactory
import cz.nowass.dispenser.device.transport.GattTransportFactory
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import android.content.pm.PackageManager
import cz.nowass.dispenser.data.onboarding.ForgetDevice
import cz.nowass.dispenser.data.onboarding.InviteController
import cz.nowass.dispenser.data.onboarding.PhonesController
import cz.nowass.dispenser.ui.devices.InviteViewModel
import cz.nowass.dispenser.ui.devices.PhonesViewModel
import cz.nowass.dispenser.data.onboarding.ForgetResult
import cz.nowass.dispenser.data.onboarding.ForgetSessions
import cz.nowass.dispenser.data.device.CredentialHealth
import cz.nowass.dispenser.ui.settings.ForgetNotice
import cz.nowass.dispenser.data.onboarding.OnboardingController
import cz.nowass.dispenser.device.transport.AndroidDeviceAssociator
import cz.nowass.dispenser.ui.DeviceAdminUi
import cz.nowass.dispenser.ui.onboarding.OnboardingViewModel
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import cz.nowass.dispenser.domain.DeviceAccess
import kotlinx.coroutines.flow.MutableStateFlow
import cz.nowass.dispenser.domain.simulation.LowStockPolicy
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import java.io.File
import java.time.Clock

/**
 * What the UI shows in one mode (docs/ANDROID_BLE_DESIGN.md 2.3, E1). Built for exactly one
 * [ResolvedMode]; DEMO and DEVICE sources are never combined.
 */
class UiSources(
    val mode: ResolvedMode,
    val repository: DispenserRepository,
    /** DEMO-only simulation controls; null in DEVICE mode. */
    val simulation: SimulationController?,
    val lowStockPolicy: LowStockPolicy,
) {
    val isDemo: Boolean get() = mode == ResolvedMode.Demo
}

/** Manual dependency container; lives as long as the process. */
class AppContainer(private val context: Context) {
    /** Phone clock in the schedule timezone; tests and previews inject fixed clocks instead. */
    val clock: Clock = Clock.system(DEFAULT_ZONE)

    /** User schedules (never touched by the simulation). A fresh install starts empty. */
    private val database: DispenserDatabase by lazy { DispenserDatabase.build(context) }

    /** Separate simulation database: state + history of simulated runs only. */
    private val simulationDatabase: SimulationDatabase by lazy { SimulationDatabase.build(context) }

    private val scheduleStore: RoomScheduleStore by lazy { RoomScheduleStore(database.scheduleDao(), clock) }
    private val simulationStore: RoomSimulationStore by lazy { RoomSimulationStore(simulationDatabase.simulationDao()) }

    private val localRepository: LocalDispenserRepository by lazy {
        LocalDispenserRepository(store = scheduleStore, clock = clock, simulationStore = simulationStore)
    }

    /** Presentation preferences (display name) in their own DataStore file; no DB involvement. */
    private val settingsRepository: AppSettingsRepository by lazy {
        DataStoreAppSettingsRepository(
            PreferenceDataStoreFactory.create { context.applicationContext.preferencesDataStoreFile(DataStoreAppSettingsRepository.FILE_NAME) },
        )
    }

    /**
     * Configuration backup / restore (A1.4c.1). Owns the startup gate: while a pending restore is
     * being rolled back (or recovery failed), [repository], [simulation] and [appSettings] refuse access.
     */
    val backup: BackupService by lazy {
        BackupService(
            schedules = scheduleStore,
            simulation = simulationStore,
            settings = settingsRepository,
            files = DirectoryRestoreFiles(File(context.filesDir, DirectoryRestoreFiles.DIRECTORY)),
            access = localRepository,
            clock = clock,
            appVersion = { "${appVersion.name} (${appVersion.code})" },
            log = { Log.w("Backup", it) },
        )
    }

    /** System document picker access for backup files (no storage permission). */
    val documents: DocumentIo by lazy { ContentResolverDocumentIo(context.applicationContext.contentResolver) }

    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /** Runs startup recovery once per process (no-op without a restore journal). */
    fun startStorageRecovery() {
        if (backup.state.value !is StorageState.Ready) appScope.launch { backup.recoverAtStartup() }
    }

    private fun requireStorageReady() =
        check(backup.state.value is StorageState.Ready) { "Local storage not ready (pending restore recovery)" }

    val repository: DispenserRepository get() = localRepository.also { requireStorageReady() }

    /** DEMO-only simulation controls (Settings); not part of the device contract. */
    val simulation: SimulationController get() = localRepository.simulation.also { requireStorageReady() }

    val appSettings: AppSettingsRepository get() = settingsRepository.also { requireStorageReady() }

    // ---- App mode (M1.10e.4a) ------------------------------------------------------------------

    /** Credential records (never backed up; docs/ANDROID_BLE_DESIGN.md 11.1–11.2). */
    private val credentialDir: File by lazy { credentialDirectory(context) }

    /** The mode selection and associated addresses, in their own DataStore file (not part of the JSON backup or a restore). */
    private val modeStore: DataStoreAppModeStore by lazy {
        DataStoreAppModeStore(PreferenceDataStoreFactory.create { context.applicationContext.preferencesDataStoreFile(DataStoreAppModeStore.FILE_NAME) })
    }

    val modes: AppModeController by lazy {
        AppModeController(
            store = modeStore,
            enrollments = { id -> CredentialStore.hasConfirmedRecord(credentialDir, id) },
            scope = appScope,
        )
    }

    /** Associated addresses (CDM), kept current for the repository's synchronous locator. */
    private val addresses: StateFlow<Map<String, String>> by lazy {
        modeStore.addresses.stateIn(appScope, SharingStarted.Eagerly, emptyMap())
    }

    /** Resolves the persisted mode once per process (file checks only). */
    fun startModeResolution() {
        if (modes.mode.value == null) modes.start()
    }

    /** The UI's data sources for [mode]. DEMO requires ready local storage (pending restore recovery). */
    fun sources(mode: ResolvedMode): UiSources = when (mode) {
        ResolvedMode.Demo -> localRepository.simulation.let { sim ->
            UiSources(mode, repository, simulation, sim.lowStockPolicy)
        }
        is ResolvedMode.Device -> UiSources(mode, deviceUi(mode.deviceId), simulation = null, lowStockPolicy = LowStockPolicy())
    }

    // ---- Device stack: built only when DEVICE mode is shown -----------------------------------

    private val noise: NativeNoiseProvider by lazy { NativeNoiseProvider() }
    private val prerequisites: AndroidBluetoothPrerequisites by lazy { AndroidBluetoothPrerequisites(context.applicationContext) }
    private val associator: AndroidDeviceAssociator by lazy { AndroidDeviceAssociator(context.applicationContext) }
    private val credentials: CredentialStore by lazy { CredentialStore(credentialDir, AndroidKeystoreKeyWrapper(), noise) }
    private val deviceDatabase: DeviceDatabase by lazy { DeviceDatabase.build(context) }

    /** The process-wide owner of the single live session (docs/ANDROID_BLE_DESIGN.md 4.4, E9). */
    val sessionManager: DeviceSessionManager by lazy {
        DeviceSessionManager(
            GattTransportFactory(AndroidGattPortFactory(context.applicationContext), prerequisites, appScope),
            noise,
            credentials,
            appScope,
        )
    }

    /**
     * Exactly one repository per device for the process lifetime (review of 4d7e3b2): the repository's
     * operation reservations are instance-local, so a second instance for the same device must never
     * exist. Reused across mode changes and session replacement.
     */
    private val deviceUis = PerKeyRegistry(::buildDeviceUi)

    /** Credential health per device (one per device, like the repository). */
    private val healths = PerKeyRegistry<String, CredentialHealth> { id -> CredentialHealth({ DeviceUiRepository.credentialAccess(credentials, id) }) }

    private fun deviceUi(deviceId: String): DeviceUiRepository = deviceUis.get(deviceId)

    private fun buildDeviceUi(deviceId: String): DeviceUiRepository {
        val dao = deviceDatabase.dao()
        // Credential health off the main thread: a missing key or record keeps DEVICE mode in the
        // recovery-required state (cached values and unresolved operations stay visible). Re-evaluated
        // after an enrollment/recovery and after "Forget device" (F3).
        val health = healths.get(deviceId)
        appScope.launch { health.refresh() }
        return DeviceUiRepository(
            device = DeviceDispenserRepository(
                deviceId = deviceId,
                address = { addresses.value[deviceId] }, // CDM association (locator only, never an identity)
                port = ManagerSessionPort(sessionManager),
                journal = RoomPendingOpsJournal(dao),
                cache = RoomDeviceCacheStore(dao),
                seqAllocator = CredentialSeqAllocator(credentials),
                timeSource = UnavailablePhoneTimeSource, // D6: SET_TIME stays disabled
                scope = appScope,
            ),
            clock = clock,
            access = health.state,
        )
    }

    // ---- Onboarding and device administration (M1.10e.4b) --------------------------------------

    /** A new onboarding flow (one per onboarding screen; its ViewModel zeroizes the label when cleared). */
    fun onboardingController(): OnboardingController = OnboardingController(
        prerequisites = prerequisites,
        associator = associator,
        manager = sessionManager,
        credentials = credentials,
        addresses = modeStore,
        modes = modes,
        // the credential is usable again (leaves the recovery-required state, F3), then resolution of
        // unresolved operations and a live status read on the session the enrollment opened
        afterEnrollment = { id ->
            healths.get(id).refresh()
            appScope.launch { deviceUi(id).device.connect() }
        },
    )

    private val forgetDevice: ForgetDevice by lazy {
        ForgetDevice(
            credentials, modeStore, associator, modes,
            sessions = object : ForgetSessions {
                override suspend fun <T> exclusive(deviceId: String, cleanup: suspend () -> T): T = sessionManager.forgetting(deviceId, cleanup)
                override suspend fun endGracefully(deviceId: String) = sessionManager.endFor(deviceId)
                override fun closeNow(deviceId: String) = sessionManager.closeNow(deviceId)
                override fun hasLiveSession(deviceId: String): Boolean = sessionManager.hasLiveSession(deviceId)
            },
            afterCredentialChange = { id -> healths.get(id).refresh() },
        )
    }

    /** The outcome of the last "Forget device" that needs the user's attention (failure, association left). */
    private val forgetNotice = MutableStateFlow<ForgetNotice?>(null)
    private val forgetRunning = java.util.concurrent.atomic.AtomicBoolean(false)

    private fun forget(deviceId: String) {
        if (!forgetRunning.compareAndSet(false, true)) return
        forgetNotice.value = null
        appScope.launch {
            val r = try { forgetDevice(deviceId) } catch (e: Throwable) { ForgetResult.Failed(ForgetResult.Step.CREDENTIALS) }
            forgetNotice.value = when (r) {
                is ForgetResult.Forgotten -> if (r.associationRemoved) null else ForgetNotice.ASSOCIATION_LEFT
                is ForgetResult.Failed -> when (r.step) {
                    ForgetResult.Step.SESSION -> ForgetNotice.FAILED_SESSION
                    ForgetResult.Step.CREDENTIALS -> ForgetNotice.FAILED_CREDENTIALS
                    ForgetResult.Step.ADDRESS, ForgetResult.Step.MODE -> ForgetNotice.FAILED_STORAGE
                }
            }
            forgetRunning.set(false)
        }
    }

    /** The shell's device administration for [mode]. */
    fun deviceAdmin(mode: ResolvedMode): DeviceAdminUi = DeviceAdminUi(
        bleSupported = context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE),
        selectedDeviceInDemo = modes.selectedDevice.map { id -> id?.takeIf { CredentialStore.hasConfirmedRecord(credentialDir, it) } }.flowOn(Dispatchers.IO),
        onboardingFactory = { purpose -> OnboardingViewModel.factory(purpose, ::onboardingController) },
        switchToDevice = { id -> appScope.launch { modes.enterDevice(id) } },
        switchToDemo = { appScope.launch { modes.enterDemo() } },
        forget = { (mode as? ResolvedMode.Device)?.deviceId?.let(::forget) },
        forgetNotice = forgetNotice,
        dismissForgetNotice = { forgetNotice.value = null },
        inviteFactory = (mode as? ResolvedMode.Device)?.deviceId?.let { id ->
            { InviteViewModel.factory { InviteController(deviceUi(id).device, credentials) } }
        },
        phonesFactory = (mode as? ResolvedMode.Device)?.deviceId?.let { id ->
            { PhonesViewModel.factory({ PhonesController(deviceUi(id).device) }, runAfterClose = { block -> appScope.launch { block() } }) }
        },
    )

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

/** One value per key for its lifetime, created at most once even under concurrent first access. */
class PerKeyRegistry<K, V>(private val create: (K) -> V) {
    private val values = HashMap<K, V>()

    @Synchronized
    fun get(key: K): V = values.getOrPut(key) { create(key) }
}

/** Credential records: app-private no-backup storage (never in Auto Backup or device transfer). */
fun credentialDirectory(context: Context): File = File(context.noBackupFilesDir, "credentials")

class DispenserApplication : Application() {
    val container: AppContainer by lazy { AppContainer(this) }
}
