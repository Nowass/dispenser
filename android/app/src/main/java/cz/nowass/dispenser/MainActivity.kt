package cz.nowass.dispenser

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import cz.nowass.dispenser.ui.DispenserApp
import cz.nowass.dispenser.ui.StorageGate
import cz.nowass.dispenser.ui.backup.BackupViewModel
import cz.nowass.dispenser.ui.theme.DispenserTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // App is always dark: force light system-bar icons even when the phone is in light mode.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        val container = (application as DispenserApplication).container
        // Roll back an interrupted restore (if any) before anything touches the stores (R1).
        container.startStorageRecovery()
        // Restore the persisted mode (DEVICE only with a confirmed enrollment, also while disconnected).
        container.startModeResolution()
        setContent {
            DispenserTheme {
                val storage by container.backup.state.collectAsStateWithLifecycle()
                val mode by container.modes.mode.collectAsStateWithLifecycle()
                StorageGate(
                    state = storage,
                    onRetry = container::startStorageRecovery,
                    onAcknowledgeNotice = container.backup::acknowledgeNotice,
                ) {
                    // Nothing from either source is shown before the mode is known. A mode change rebuilds
                    // the whole UI (navigation, ViewModels), so no screen ever mixes DEMO and DEVICE data.
                    val resolved = mode ?: return@StorageGate
                    key(resolved) {
                        val sources = remember(resolved) { container.sources(resolved) }
                        DispenserApp(
                            repository = sources.repository,
                            simulation = sources.simulation,
                            lowStockPolicy = sources.lowStockPolicy,
                            appSettings = container.appSettings,
                            appVersion = container.appVersion,
                            isDemo = sources.isDemo,
                            updateChecker = container.updateChecker,
                            assets = container.assets,
                            sourceCodeUrl = container.sourceCodeUrl,
                            backupViewModelFactory = BackupViewModel.factory(container.backup, container.documents, container.clock),
                        )
                    }
                }
            }
        }
    }
}
