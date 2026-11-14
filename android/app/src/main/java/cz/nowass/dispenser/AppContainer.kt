package cz.nowass.dispenser

import android.app.Application
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.MockDispenserRepository

/** Manual dependency container; lives as long as the process. */
class AppContainer {
    /** A1 runs in DEMO mode only. A BLE-backed repository is out of scope. */
    val isDemo: Boolean = true

    val repository: DispenserRepository by lazy { MockDispenserRepository() }
}

class DispenserApplication : Application() {
    val container: AppContainer by lazy { AppContainer() }
}
