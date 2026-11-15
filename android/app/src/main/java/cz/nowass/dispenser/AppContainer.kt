package cz.nowass.dispenser

import android.app.Application
import android.content.Context
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.SimulatedDevice
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import java.time.Clock

/** Manual dependency container; lives as long as the process. */
class AppContainer(context: Context) {
    /** Device values (battery, reservoir) are still simulated, so the DEMO badge stays. */
    val isDemo: Boolean = true

    /** Phone clock in the schedule timezone; tests and previews inject fixed clocks instead. */
    val clock: Clock = Clock.system(DEFAULT_ZONE)

    private val database: DispenserDatabase by lazy { DispenserDatabase.build(context) }

    /** Schedules persisted on the phone (Room); a fresh install starts with no schedules. */
    val repository: DispenserRepository by lazy {
        LocalDispenserRepository(
            store = RoomScheduleStore(database.scheduleDao(), clock),
            clock = clock,
            simulatedDevice = SimulatedDevice.demo(),
        )
    }
}

class DispenserApplication : Application() {
    val container: AppContainer by lazy { AppContainer(this) }
}
