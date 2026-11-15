package cz.nowass.dispenser

import android.app.Application
import android.content.Context
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.SimulationController
import cz.nowass.dispenser.data.DispenserRepository
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.data.local.simulation.RoomSimulationStore
import cz.nowass.dispenser.data.local.simulation.SimulationDatabase
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import java.time.Clock

/** Manual dependency container; lives as long as the process. */
class AppContainer(context: Context) {
    /** Device values (battery, reservoir, history) are still simulated, so the DEMO badge stays. */
    val isDemo: Boolean = true

    /** Phone clock in the schedule timezone; tests and previews inject fixed clocks instead. */
    val clock: Clock = Clock.system(DEFAULT_ZONE)

    /** User schedules (never touched by the simulation). A fresh install starts empty. */
    private val database: DispenserDatabase by lazy { DispenserDatabase.build(context) }

    /** Separate simulation database: state + history of simulated runs only. */
    private val simulationDatabase: SimulationDatabase by lazy { SimulationDatabase.build(context) }

    private val localRepository: LocalDispenserRepository by lazy {
        LocalDispenserRepository(
            store = RoomScheduleStore(database.scheduleDao(), clock),
            clock = clock,
            simulationStore = RoomSimulationStore(simulationDatabase.simulationDao()),
        )
    }

    val repository: DispenserRepository get() = localRepository

    /** DEMO-only simulation controls (Settings); not part of the device contract. */
    val simulation: SimulationController get() = localRepository.simulation
}

class DispenserApplication : Application() {
    val container: AppContainer by lazy { AppContainer(this) }
}
