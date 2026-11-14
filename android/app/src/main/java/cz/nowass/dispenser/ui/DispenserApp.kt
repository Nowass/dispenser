package cz.nowass.dispenser.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.ui.components.AppHeader
import cz.nowass.dispenser.ui.dashboard.DashboardRoute
import cz.nowass.dispenser.ui.dashboard.DashboardViewModel

enum class TopLevelDestination(
    val route: String,
    @param:StringRes val labelRes: Int,
    val icon: ImageVector,
) {
    DASHBOARD("dashboard", R.string.nav_dashboard, Icons.Filled.Home),
    CALENDAR("calendar", R.string.nav_calendar, Icons.Filled.DateRange),
    HISTORY("history", R.string.nav_history, Icons.AutoMirrored.Filled.List),
    SETTINGS("settings", R.string.nav_settings, Icons.Filled.Settings),
    ;

    val testTag: String get() = "nav_$route"
}

@Composable
fun DispenserApp(repository: DispenserRepository, isDemo: Boolean) {
    val navController = rememberNavController()
    val backStackEntry by navController.currentBackStackEntryAsState()
    val currentDestination = backStackEntry?.destination
    val current = TopLevelDestination.entries.firstOrNull { dest ->
        currentDestination?.hierarchy?.any { it.route == dest.route } == true
    } ?: TopLevelDestination.DASHBOARD

    fun navigateTo(dest: TopLevelDestination) {
        navController.navigate(dest.route) {
            popUpTo(navController.graph.findStartDestination().id) { saveState = true }
            launchSingleTop = true
            restoreState = true
        }
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
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
            NavigationBar(
                containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
                tonalElevation = 0.dp,
            ) {
                val itemColors = NavigationBarItemDefaults.colors(
                    selectedIconColor = MaterialTheme.colorScheme.primary,
                    selectedTextColor = MaterialTheme.colorScheme.primary,
                    indicatorColor = MaterialTheme.colorScheme.primaryContainer,
                    unselectedIconColor = MaterialTheme.colorScheme.onSurfaceVariant,
                    unselectedTextColor = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                TopLevelDestination.entries.forEach { dest ->
                    NavigationBarItem(
                        modifier = Modifier.testTag(dest.testTag),
                        selected = dest == current,
                        onClick = { navigateTo(dest) },
                        // Label text provides the accessible name.
                        icon = { Icon(dest.icon, contentDescription = null) },
                        label = { Text(stringResource(dest.labelRes), maxLines = 1) },
                        colors = itemColors,
                    )
                }
            }
        },
    ) { padding ->
        NavHost(
            navController = navController,
            startDestination = TopLevelDestination.DASHBOARD.route,
            modifier = Modifier.padding(padding),
        ) {
            composable(TopLevelDestination.DASHBOARD.route) {
                DashboardRoute(
                    viewModel = viewModel(factory = DashboardViewModel.factory(repository)),
                    onOpenCalendar = { navigateTo(TopLevelDestination.CALENDAR) },
                )
            }
            composable(TopLevelDestination.CALENDAR.route) { CalendarScreen() }
            composable(TopLevelDestination.HISTORY.route) { HistoryScreen() }
            composable(TopLevelDestination.SETTINGS.route) { SettingsScreen() }
        }
    }
}
