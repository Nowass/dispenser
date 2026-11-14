package cz.nowass.dispenser.data

import cz.nowass.dispenser.domain.DeviceStatus
import cz.nowass.dispenser.domain.DispenserEvent
import cz.nowass.dispenser.domain.ReservoirEstimate
import cz.nowass.dispenser.domain.Schedule
import kotlinx.coroutines.flow.Flow

/**
 * Single boundary between UI/ViewModels and dispenser data.
 *
 * Implemented now by [MockDispenserRepository] (DEMO); a future
 * `BleDispenserRepository` will implement the same contract.
 *
 * A1.1 exposes read-only streams. Mutations (schedule CRUD with expected
 * revision, simulated time, refill) are added in A1.2/A1.3.
 */
interface DispenserRepository {
    fun observeDevice(): Flow<DeviceStatus>
    fun observeSchedules(): Flow<List<Schedule>>
    fun observeReservoir(): Flow<ReservoirEstimate>
    fun observeEvents(): Flow<List<DispenserEvent>>
}
