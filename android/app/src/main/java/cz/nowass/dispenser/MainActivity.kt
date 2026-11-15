package cz.nowass.dispenser

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import cz.nowass.dispenser.ui.DispenserApp
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
        setContent {
            DispenserTheme {
                DispenserApp(repository = container.repository, simulation = container.simulation, isDemo = container.isDemo)
            }
        }
    }
}
