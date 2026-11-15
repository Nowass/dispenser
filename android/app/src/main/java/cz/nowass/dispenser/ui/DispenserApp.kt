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

@Composable
fun DispenserApp(repository: DispenserRepository, isDemo: Boolean) {
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
    val savedMessage = stringResource(R.string.editor_saved_snackbar)
    val deletedMessage = stringResource(R.string.editor_deleted_snackbar)

    // The editor is a full-screen task with its own top bar (incl. DEMO badge); hide header and dock.
    val inEditor = currentDestination?.route?.substringBefore('?') == SCHEDULE_ROUTE

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
                title = stringResource(
                    if (current == TopLevelDestination.DASHBOARD) R.string.dashboard_title else current.labelRes,
                ),
                showDemoBadge = isDemo,
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
                    initialTab = entry.arguments?.getString(ARG_TAB),
                    onCreateSchedule = { date -> openEditor(date = date) },
                    onEditSchedule = { id -> openEditor(scheduleId = id) },
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
            )
            TopLevelDestination.CALENDAR -> CalendarRoute(
                viewModel = calendarViewModel,
                onCreate = onCreateSchedule,
                onEdit = onEditSchedule,
            )
            TopLevelDestination.HISTORY -> HistoryScreen()
            TopLevelDestination.SETTINGS -> SettingsScreen()
        }
    }
}

// ---- Previews: whole shell incl. floating navigation dock at compact and normal widths ----

@androidx.compose.ui.tooling.preview.Preview(name = "Shell 320 dp", widthDp = 320, heightDp = 640, showBackground = true, backgroundColor = 0xFF111820)
@androidx.compose.ui.tooling.preview.Preview(name = "Shell 412 dp", widthDp = 412, heightDp = 915, showBackground = true, backgroundColor = 0xFF111820)
@Composable
private fun DispenserAppPreview() {
    cz.nowass.dispenser.ui.theme.DispenserTheme {
        DispenserApp(repository = cz.nowass.dispenser.data.MockDispenserRepository(), isDemo = true)
    }
}
