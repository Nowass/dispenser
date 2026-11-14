package cz.nowass.dispenser.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import cz.nowass.dispenser.R
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.ui.theme.Spacing

/** A1.1 placeholder; history is implemented in A1.3. */
@Composable
fun HistoryScreen(modifier: Modifier = Modifier) = ComingNext(modifier)

@Composable
private fun SurfaceCard(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
    ) { content() }
}

@Composable
private fun ComingNext(modifier: Modifier) {
    Column(modifier.fillMaxSize().padding(horizontal = Spacing.m, vertical = Spacing.s)) {
        SurfaceCard {
            Text(
                stringResource(R.string.placeholder_coming_next),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(Spacing.m),
            )
        }
    }
}

/** Static settings for A1.1: demo mode, schedule timezone, disabled device connection. */
@Composable
fun SettingsScreen(modifier: Modifier = Modifier) {
    val itemColors = ListItemDefaults.colors(
        containerColor = MaterialTheme.colorScheme.surface,
        supportingColor = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Column(
        modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Spacing.m, vertical = Spacing.s),
    ) {
        SurfaceCard {
            Column {
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_mode)) },
                    supportingContent = { Text(stringResource(R.string.settings_mode_demo)) },
                    colors = itemColors,
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_timezone)) },
                    supportingContent = { Text(DEFAULT_ZONE.id) },
                    colors = itemColors,
                )
                HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = { Text(stringResource(R.string.settings_device)) },
                    supportingContent = {
                        Column {
                            Text(stringResource(R.string.settings_device_coming_later))
                            OutlinedButton(
                                onClick = {},
                                enabled = false,
                                modifier = Modifier.padding(top = Spacing.s),
                            ) { Text(stringResource(R.string.settings_device_connect)) }
                        }
                    },
                    colors = itemColors,
                )
            }
        }
    }
}
