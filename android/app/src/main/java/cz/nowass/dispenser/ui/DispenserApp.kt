package cz.nowass.dispenser.ui

import androidx.annotation.StringRes
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.navigation.NavType
import androidx.navigation.navArgument
import cz.nowass.dispenser.ui.calendar.CalendarRoute
import cz.nowass.dispenser.ui.calendar.CalendarViewModel
import cz.nowass.dispenser.ui.schedule.EditorResult
import cz.nowass.dispenser.ui.schedule.ScheduleEditorRoute
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.launch
import cz.nowass.dispenser.ui.schedule.ScheduleEditorViewModel
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.lifecycle.Lifecycle
import java.time.LocalDate
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.SimulationController
import cz.nowass.dispenser.data.AppSettingsRepository
import cz.nowass.dispenser.data.AppVersion
import cz.nowass.dispenser.ui.settings.AppSettingsViewModel
import cz.nowass.dispenser.data.about.AssetTextSource
import cz.nowass.dispenser.data.update.UpdateChecker
import cz.nowass.dispenser.ui.about.AboutRoute
import cz.nowass.dispenser.ui.about.AboutViewModel
import cz.nowass.dispenser.ui.about.ChangelogRoute
import cz.nowass.dispenser.ui.about.LicenseTextRoute
import cz.nowass.dispenser.ui.about.LicensesRoute
import android.net.Uri
import cz.nowass.dispenser.ui.history.HistoryRoute
import cz.nowass.dispenser.ui.history.HistoryViewModel
import cz.nowass.dispenser.ui.reservoir.ReservoirRoute
import cz.nowass.dispenser.ui.reservoir.ReservoirViewModel
import cz.nowass.dispenser.ui.settings.SettingsRoute
import cz.nowass.dispenser.ui.settings.SimulationSettingsViewModel
import cz.nowass.dispenser.ui.settings.simulatedClockText
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.ui.components.AppHeader
import cz.nowass.dispenser.ui.components.DispenserIcons
import cz.nowass.dispenser.ui.components.NavigationDock
import cz.nowass.dispenser.ui.components.NavigationDockItem
import cz.nowass.dispenser.ui.dashboard.DashboardRoute
import cz.nowass.dispenser.ui.dashboard.DashboardViewModel

enum class TopLevelDestination(
    val route: String,
    @param:StringRes val labelRes: Int,
    val icon: ImageVector,
) {
    DASHBOARD("dashboard", R.string.nav_dashboard, Icons.Filled.Home),
    CALENDAR("calendar", R.string.nav_calendar, Icons.Filled.DateRange),
    HISTORY("history", R.string.nav_history, DispenserIcons.History),
    SETTINGS("settings", R.string.nav_settings, Icons.Filled.Settings),
    ;

    val testTag: String get() = "nav_$route"
}

/** Base route of the schedule editor (`schedule?scheduleId=…&date=…`). */
const val SCHEDULE_ROUTE = "schedule"

/**
 * Main destination hosting the four top-level screens as swipeable pages
 * (`main?tab={tab}&date={date}`); `tab` is a [TopLevelDestination.route] such as
 * `calendar`, `date` an ISO date preselected in the calendar.
 */
const val MAIN_ROUTE = "main"
const val ARG_TAB = "tab"
const val MAIN_PAGER_TEST_TAG = "main_pager"

/** Reservoir detail (full-screen route, like the schedule editor). */
const val RESERVOIR_ROUTE = "reservoir"

/** About and its sub-screens (full-screen routes). */
const val ABOUT_ROUTE = "about"
const val ABOUT_CHANGELOG_ROUTE = "about_changelog"
const val ABOUT_LICENSES_ROUTE = "about_licenses"
const val ABOUT_LICENSE_TEXT_ROUTE = "about_license_text"

/** Backup and restore (full-screen route, A1.4c.1). */
const val BACKUP_ROUTE = "backup"

/** Onboarding: pairing (CLAIM) or recovery (full-screen route, M1.10e.4b). */
const val ONBOARDING_ROUTE = "onboarding"

/**
 * Device administration for the shell (M1.10e.4b): pairing entry, mode switch, "Forget device". The
 * actions run in the process scope (a mode change rebuilds the UI that triggered them).
 */
class DeviceAdminUi(
    val bleSupported: Boolean,
    /** In DEMO: a device with a confirmed enrollment that can be switched back to. */
    val selectedDeviceInDemo: kotlinx.coroutines.flow.Flow<String?>,
    val onboardingFactory: (cz.nowass.dispenser.data.onboarding.OnboardingPurpose) -> androidx.lifecycle.ViewModelProvider.Factory,
    val switchToDevice: (String) -> Unit,
    val switchToDemo: () -> Unit,
    val forget: () -> Unit,
)

/**
 * Detail routes that draw their own [cz.nowass.dispenser.ui.components.DetailTopBar]: the shell
 * then hides its tab header and navigation dock (otherwise two headers would be stacked).
 */
internal fun isFullScreenRoute(route: String?): Boolean {
    val base = route?.substringBefore('?') ?: return false
    return base in setOf(SCHEDULE_ROUTE, RESERVOIR_ROUTE, BACKUP_ROUTE, ONBOARDING_ROUTE) || base.startsWith(ABOUT_ROUTE)
}

private const val ARG_TITLE = "title"
private const val ARG_PATH = "path"

@Composable
fun DispenserApp(
    repository: DispenserRepository,
    /** DEMO-only simulation controls; null in DEVICE mode (the Simulace card is not shown). */
    simulation: SimulationController?,
    appSettings: AppSettingsRepository,
    appVersion: AppVersion,
    isDemo: Boolean,
    updateChecker: UpdateChecker,
    assets: AssetTextSource,
    sourceCodeUrl: String?,
    /** Backup screen dependencies; null hides the entry (previews, tests without storage). */
    backupViewModelFactory: androidx.lifecycle.ViewModelProvider.Factory? = null,
    lowStockPolicy: cz.nowass.dispenser.domain.simulation.LowStockPolicy =
        simulation?.lowStockPolicy ?: cz.nowass.dispenser.domain.simulation.LowStockPolicy(),
    /** Pairing, mode switch and "Forget device"; null keeps the earlier placeholder (previews, tests). */
    deviceAdmin: DeviceAdminUi? = null,
) {
    val navController = rememberNavController()
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = backStackEntry?.destination

    // Single source of truth for the active top-level screen: the pager state. The dock, the
    // header and swipe gestures all read/write it. Hoisted here (saveable) so it survives
    // visits to the schedule editor and configuration changes.
    val pagerState = rememberPagerState { TopLevelDestination.entries.size }
    val current = TopLevelDestination.entries[pagerState.currentPage]
    val scope = rememberCoroutineScope()

    fun showPage(dest: TopLevelDestination) {
        scope.launch { pagerState.animateScrollToPage(dest.ordinal) }
    }

    fun openEditor(scheduleId: String? = null, date: LocalDate? = null) {
        val query = buildList {
            if (scheduleId != null) add("${ScheduleEditorViewModel.ARG_SCHEDULE_ID}=$scheduleId")
            if (date != null) add("${ScheduleEditorViewModel.ARG_DATE}=$date")
        }.joinToString("&")
        navController.navigate(if (query.isEmpty()) SCHEDULE_ROUTE else "$SCHEDULE_ROUTE?$query") {
            launchSingleTop = true
        }
    }

    // Confirmation after a successful editor write ("Uloženo v telefonu" — never a device sync claim).
    val snackbarHostState = remember { SnackbarHostState() }
    // DEVICE mode: saved = read back from the device; uncertain = verified at the next connection (no retry).
    val savedMessage = stringResource(if (isDemo) R.string.editor_saved_snackbar else R.string.editor_saved_device_snackbar)
    val uncertainMessage = stringResource(R.string.editor_uncertain_snackbar)
    val deletedMessage = stringResource(R.string.editor_deleted_snackbar)
    val appLicenseTitle = stringResource(R.string.about_app_license_title)

    // Editor, reservoir, About and backup are full-screen tasks with their own top bar (incl. DEMO badge).
    val inEditor = isFullScreenRoute(currentDestination?.route)

    // A1.4a: dashboard header shows the user's aquarium name (default "Moje akvárium").
    val nameFlow = remember(appSettings) { appSettings.displayName }
    val customName by nameFlow.collectAsStateWithLifecycle(initialValue = null)

    // D1: one visible simulation clock while the simulation runs.
    val deviceFlow = remember(repository) { repository.observeDevice() }
    val device by deviceFlow.collectAsStateWithLifecycle(initialValue = null)
    val simulationClockText = device?.takeIf { it.simulationActive }?.let {
        stringResource(R.string.header_simulated_time, simulatedClockText(it.now.atZone(it.zone)))
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        snackbarHost = {
            SnackbarHost(snackbarHostState) { data ->
                Snackbar(
                    snackbarData = data,
                    containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                    contentColor = MaterialTheme.colorScheme.onSurface,
                )
            }
        },
        topBar = {
            if (inEditor) return@Scaffold
            // DEMO badge is persistent on every screen: all data in A1 is simulated.
            AppHeader(
                brand = stringResource(R.string.app_brand),
                title = if (current == TopLevelDestination.DASHBOARD) {
                    customName ?: stringResource(R.string.dashboard_title)
                } else {
                    stringResource(current.labelRes)
                },
                showDemoBadge = isDemo,
                simulationClock = simulationClockText,
                connection = if (isDemo) null else device?.connection ?: cz.nowass.dispenser.domain.ConnectionState.DISCONNECTED,
            )
        },
        bottomBar = {
            if (inEditor) return@Scaffold
            // Floating icon-only dock (48 dp + margins + system inset); always visible.
            NavigationDock {
                TopLevelDestination.entries.forEach { dest ->
                    NavigationDockItem(
                        modifier = Modifier.testTag(dest.testTag),
                        selected = dest == current,
                        onClick = { showPage(dest) },
                        icon = dest.icon,
                        contentDescription = stringResource(dest.labelRes),
                    )
                }
            }
        },
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = "$MAIN_ROUTE?$ARG_TAB={$ARG_TAB}&${CalendarViewModel.ARG_DATE}={${CalendarViewModel.ARG_DATE}}",
            // Consume the applied insets so nested imePadding() only adds the keyboard excess.
            modifier = Modifier.padding(padding).consumeWindowInsets(padding),
        ) {
            composable(
                route = "$MAIN_ROUTE?$ARG_TAB={$ARG_TAB}&${CalendarViewModel.ARG_DATE}={${CalendarViewModel.ARG_DATE}}",
                arguments = listOf(
                    navArgument(ARG_TAB) { type = NavType.StringType; nullable = true; defaultValue = null },
                    navArgument(CalendarViewModel.ARG_DATE) { type = NavType.StringType; nullable = true; defaultValue = null },
                ),
            ) { entry ->
                MainPager(
                    pagerState = pagerState,
                    repository = repository,
                    simulation = simulation,
                    appSettings = appSettings,
                    appVersion = appVersion,
                    onOpenReservoir = { navController.navigate(RESERVOIR_ROUTE) { launchSingleTop = true } },
                    onOpenAbout = { navController.navigate(ABOUT_ROUTE) { launchSingleTop = true } },
                    onOpenBackup = backupViewModelFactory?.let { { navController.navigate(BACKUP_ROUTE) { launchSingleTop = true } } },
                    deviceAdmin = deviceAdmin?.let { admin ->
                        val selected by remember(admin) { admin.selectedDeviceInDemo }.collectAsStateWithLifecycle(initialValue = null)
                        cz.nowass.dispenser.ui.settings.SettingsDeviceAdmin(
                            bleSupported = admin.bleSupported,
                            selectedDeviceInDemo = selected.takeIf { isDemo },
                            onConnect = { navController.navigate("$ONBOARDING_ROUTE?purpose=CLAIM") { launchSingleTop = true } },
                            onRecover = { navController.navigate("$ONBOARDING_ROUTE?purpose=RECOVERY") { launchSingleTop = true } },
                            onSwitchToDevice = { selected?.let(admin.switchToDevice) },
                            onSwitchToDemo = admin.switchToDemo,
                            onForget = admin.forget,
                        )
                    },
                    initialTab = entry.arguments?.getString(ARG_TAB),
                    onCreateSchedule = { date -> openEditor(date = date) },
                    onEditSchedule = { id -> openEditor(scheduleId = id) },
                )
            }
            composable(ABOUT_ROUTE) {
                AboutRoute(
                    viewModel = viewModel(factory = AboutViewModel.factory(updateChecker, appVersion, sourceCodeUrl)),
                    isDemo = isDemo,
                    onBack = { navController.popBackStack() },
                    onOpenChangelog = { navController.navigate(ABOUT_CHANGELOG_ROUTE) { launchSingleTop = true } },
                    onOpenLicenses = { navController.navigate(ABOUT_LICENSES_ROUTE) { launchSingleTop = true } },
                    onOpenAppLicense = {
                        navController.navigate(
                            "$ABOUT_LICENSE_TEXT_ROUTE?$ARG_TITLE=${Uri.encode(appLicenseTitle)}&$ARG_PATH=${Uri.encode(cz.nowass.dispenser.ui.about.APP_LICENSE_TEXT_PATH)}",
                        ) { launchSingleTop = true }
                    },
                )
            }
            if (deviceAdmin != null) {
                composable(
                    route = "$ONBOARDING_ROUTE?purpose={purpose}",
                    arguments = listOf(navArgument("purpose") { type = NavType.StringType; defaultValue = "CLAIM" }),
                ) { entry ->
                    val purpose = cz.nowass.dispenser.data.onboarding.OnboardingPurpose.entries
                        .firstOrNull { it.name == entry.arguments?.getString("purpose") } ?: cz.nowass.dispenser.data.onboarding.OnboardingPurpose.CLAIM
                    cz.nowass.dispenser.ui.onboarding.OnboardingRoute(
                        viewModel = viewModel(factory = deviceAdmin.onboardingFactory(purpose)),
                        isDemo = isDemo,
                        onClose = { navController.popBackStack() },
                    )
                }
            }
            if (backupViewModelFactory != null) {
                composable(BACKUP_ROUTE) {
                    cz.nowass.dispenser.ui.backup.BackupRoute(
                        viewModel = viewModel(factory = backupViewModelFactory),
                        isDemo = isDemo,
                        onBack = { navController.popBackStack() },
                    )
                }
            }
            composable(ABOUT_CHANGELOG_ROUTE) {
                ChangelogRoute(assets, appVersion.name, isDemo, onBack = { navController.popBackStack() })
            }
            composable(ABOUT_LICENSES_ROUTE) {
                LicensesRoute(assets, isDemo, onBack = { navController.popBackStack() }) { title, path ->
                    navController.navigate("$ABOUT_LICENSE_TEXT_ROUTE?$ARG_TITLE=${Uri.encode(title)}&$ARG_PATH=${Uri.encode(path)}")
                }
            }
            composable(
                route = "$ABOUT_LICENSE_TEXT_ROUTE?$ARG_TITLE={$ARG_TITLE}&$ARG_PATH={$ARG_PATH}",
                arguments = listOf(
                    navArgument(ARG_TITLE) { type = NavType.StringType; defaultValue = "" },
                    navArgument(ARG_PATH) { type = NavType.StringType; defaultValue = "" },
                ),
            ) { entry ->
                LicenseTextRoute(
                    assets = assets,
                    title = entry.arguments?.getString(ARG_TITLE).orEmpty(),
                    path = entry.arguments?.getString(ARG_PATH).orEmpty(),
                    isDemo = isDemo,
                    onBack = { navController.popBackStack() },
                )
            }
            composable(RESERVOIR_ROUTE) {
                ReservoirRoute(
                    viewModel = viewModel(factory = ReservoirViewModel.factory(repository, lowStockPolicy)),
                    isDemo = isDemo,
                    onBack = { navController.popBackStack() },
                )
            }
            composable(
                route = "$SCHEDULE_ROUTE?${ScheduleEditorViewModel.ARG_SCHEDULE_ID}={${ScheduleEditorViewModel.ARG_SCHEDULE_ID}}" +
                    "&${ScheduleEditorViewModel.ARG_DATE}={${ScheduleEditorViewModel.ARG_DATE}}",
                arguments = listOf(
                    navArgument(ScheduleEditorViewModel.ARG_SCHEDULE_ID) {
                        type = NavType.StringType
                        nullable = true
                        defaultValue = null
                    },
                    navArgument(ScheduleEditorViewModel.ARG_DATE) {
                        type = NavType.StringType
                        nullable = true
                        defaultValue = null
                    },
                ),
            ) { entry ->
                // Not part of the pager: no tab swiping inside the editor.
                ScheduleEditorRoute(
                    viewModel = viewModel(factory = ScheduleEditorViewModel.factory(repository)),
                    isDemo = isDemo,
                    // Pop only while this entry is resumed, so a repeated close can never pop the calendar too.
                    onClose = { result ->
                        if (entry.lifecycle.currentState == Lifecycle.State.RESUMED) {
                            navController.popBackStack()
                            val message = when (result) {
                                EditorResult.SAVED -> savedMessage
                                EditorResult.DELETED -> deletedMessage
                                EditorResult.UNCERTAIN -> uncertainMessage
                                null -> null
                            }
                            if (message != null) scope.launch { snackbarHostState.showSnackbar(message) }
                        }
                    },
                )
            }
        }
    }
}

/**
 * The four top-level screens as horizontally swipeable pages (order = [TopLevelDestination]).
 *
 * - All pages stay composed (beyondViewportPageCount), and their ViewModels are scoped to the
 *   main back-stack entry, so switching tabs never recreates screens or reloads data, and
 *   calendar selection/month and scroll positions are preserved.
 * - Vertical scrolling inside pages is unaffected; horizontally scrollable children consume
 *   their own drags first (nested scrolling).
 * - Back from a non-dashboard page returns to Přehled (same as the previous tab back stack);
 *   back on Přehled leaves the app.
 */
@Composable
private fun MainPager(
    pagerState: PagerState,
    repository: DispenserRepository,
    simulation: SimulationController?,
    appSettings: AppSettingsRepository,
    appVersion: AppVersion,
    onOpenReservoir: () -> Unit,
    onOpenAbout: () -> Unit,
    onOpenBackup: (() -> Unit)?,
    deviceAdmin: cz.nowass.dispenser.ui.settings.SettingsDeviceAdmin?,
    initialTab: String?,
    onCreateSchedule: (LocalDate) -> Unit,
    onEditSchedule: (String) -> Unit,
) {
    val scope = rememberCoroutineScope()
    val dashboardViewModel: DashboardViewModel = viewModel(factory = DashboardViewModel.factory(repository))
    // Reads the route's `date` argument (if any) as its initial selection.
    val calendarViewModel: CalendarViewModel = viewModel(factory = CalendarViewModel.factory(repository))

    // Honour an explicit `tab` argument once per entry (not again after rotation/process death).
    var initialTabApplied by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(initialTab) {
        if (!initialTabApplied) {
            TopLevelDestination.entries.firstOrNull { it.route == initialTab }?.let { pagerState.scrollToPage(it.ordinal) }
            initialTabApplied = true
        }
    }

    BackHandler(enabled = pagerState.currentPage != TopLevelDestination.DASHBOARD.ordinal) {
        scope.launch { pagerState.animateScrollToPage(TopLevelDestination.DASHBOARD.ordinal) }
    }

    HorizontalPager(
        state = pagerState,
        beyondViewportPageCount = TopLevelDestination.entries.size - 1,
        key = { TopLevelDestination.entries[it].route },
        modifier = Modifier.fillMaxSize().testTag(MAIN_PAGER_TEST_TAG),
    ) { page ->
        when (TopLevelDestination.entries[page]) {
            TopLevelDestination.DASHBOARD -> DashboardRoute(
                viewModel = dashboardViewModel,
                onOpenCalendar = { date ->
                    // Dashboard day tap selects that date; the header link keeps the last selection.
                    if (date != null) calendarViewModel.selectDate(date)
                    scope.launch { pagerState.animateScrollToPage(TopLevelDestination.CALENDAR.ordinal) }
                },
                onOpenReservoir = onOpenReservoir,
            )
            TopLevelDestination.CALENDAR -> CalendarRoute(
                viewModel = calendarViewModel,
                onCreate = onCreateSchedule,
                onEdit = onEditSchedule,
            )
            TopLevelDestination.HISTORY -> HistoryRoute(viewModel(factory = HistoryViewModel.factory(repository)))
            TopLevelDestination.SETTINGS -> SettingsRoute(
                // DEMO only: in DEVICE mode no simulation ViewModel exists at all.
                simulationViewModel = simulation?.let { viewModel(factory = SimulationSettingsViewModel.factory(it, repository)) },
                appViewModel = viewModel(factory = AppSettingsViewModel.factory(appSettings, repository, appVersion)),
                onOpenReservoir = onOpenReservoir,
                onOpenAbout = onOpenAbout,
                onOpenBackup = onOpenBackup,
                deviceAdmin = deviceAdmin,
            )
        }
    }
}

// ---- Previews: whole shell incl. floating navigation dock at compact and normal widths ----

@androidx.compose.ui.tooling.preview.Preview(name = "Shell 320 dp", widthDp = 320, heightDp = 640, showBackground = true, backgroundColor = 0xFF111820)
@androidx.compose.ui.tooling.preview.Preview(name = "Shell 412 dp", widthDp = 412, heightDp = 915, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun DispenserAppPreview() {
    cz.nowass.dispenser.ui.theme.DispenserTheme {
        val repo = cz.nowass.dispenser.data.MockDispenserRepository()
        DispenserApp(
            repository = repo,
            simulation = repo.simulation,
            appSettings = cz.nowass.dispenser.data.InMemoryAppSettingsRepository(),
            appVersion = AppVersion("0.4.0", 4),
            isDemo = true,
            updateChecker = { _, _ -> cz.nowass.dispenser.domain.update.UpdateCheckResult.RepositoryUnavailable },
            assets = { null },
            sourceCodeUrl = null,
        )
    }
}
