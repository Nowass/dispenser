package cz.nowass.dispenser.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.padding
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
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
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

@Composable
fun DispenserApp(repository: DispenserRepository, isDemo: Boolean) {
    val navController = rememberNavController()
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = backStackEntry?.destination
    val current = TopLevelDestination.entries.firstOrNull { dest ->
        // Routes may carry optional arguments ("calendar?date={date}"); match on the base route.
        currentDestination?.hierarchy?.any { it.route?.substringBefore('?') == dest.route } == true
    } ?: TopLevelDestination.DASHBOARD

    fun navigateTo(dest: TopLevelDestination) {
        navController.navigate(dest.route) {
            popUpTo(navController.graph.findStartDestination().id) { saveState = true }
            launchSingleTop = true
            restoreState = true
        }
    }

    /** Opens the calendar at [date] (fresh selection, not the restored one); null = normal tab switch. */
    fun openCalendar(date: LocalDate?) {
        if (date == null) return navigateTo(TopLevelDestination.CALENDAR)
        navController.navigate("${TopLevelDestination.CALENDAR.route}?${CalendarViewModel.ARG_DATE}=$date") {
            popUpTo(navController.graph.findStartDestination().id) { saveState = true }
            launchSingleTop = true
            restoreState = false
        }
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
    val scope = rememberCoroutineScope()
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
                        onClick = { navigateTo(dest) },
                        icon = dest.icon,
                        contentDescription = stringResource(dest.labelRes),
                    )
                }
            }
        },
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = TopLevelDestination.DASHBOARD.route,
            // Consume the applied insets so nested imePadding() only adds the keyboard excess.
            modifier = Modifier.padding(padding).consumeWindowInsets(padding),
        ) {
            composable(TopLevelDestination.DASHBOARD.route) {
                DashboardRoute(
                    viewModel = viewModel(factory = DashboardViewModel.factory(repository)),
                    onOpenCalendar = ::openCalendar,
                )
            }
            composable(
                route = "${TopLevelDestination.CALENDAR.route}?${CalendarViewModel.ARG_DATE}={${CalendarViewModel.ARG_DATE}}",
                arguments = listOf(
                    navArgument(CalendarViewModel.ARG_DATE) {
                        type = NavType.StringType
                        nullable = true
                        defaultValue = null
                    },
                ),
            ) {
                CalendarRoute(
                    viewModel = viewModel(factory = CalendarViewModel.factory(repository)),
                    onCreate = { date -> openEditor(date = date) },
                    onEdit = { id -> openEditor(scheduleId = id) },
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
            composable(TopLevelDestination.HISTORY.route) { HistoryScreen() }
            composable(TopLevelDestination.SETTINGS.route) { SettingsScreen() }
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
