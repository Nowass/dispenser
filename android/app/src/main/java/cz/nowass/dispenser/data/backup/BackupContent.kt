package cz.nowass.dispenser.data.backup

import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.simulation.HistoryEvent
import cz.nowass.dispenser.domain.simulation.SimulationState
import java.time.Instant

/**
 * Validated content of a configuration backup (format `dispenser-backup`, see
 * docs/BACKUP_FORMAT.md). Built only from domain objects; never from Room entities.
 *
 * Device synchronization state is intentionally not part of a backup: restored schedules
 * are always stored as PENDING ("Uloženo v telefonu").
 */
data class BackupContent(
    val exportedAt: Instant,
    val applicationVersion: String,
    /** Custom aquarium name; null = default name. */
    val displayName: String?,
    val schedules: List<Schedule>,
    /** Present only when the user chose "Zahrnout simulaci a historii". All data simulated. */
    val simulation: SimulationBackup?,
)

/** Simulated data only (origin SIMULATION); never real device activity. */
data class SimulationBackup(
    /** Null when no simulation run was active at export time (history may still exist). */
    val state: SimulationState?,
    val history: List<HistoryEvent>,
)

/** Why a backup file was rejected. [path] names the offending JSON location, e.g. `schedules[2].localTime`. */
data class BackupError(val kind: Kind, val path: String? = null, val detail: String? = null) {
    enum class Kind {
        EMPTY,
        TOO_LARGE,
        NOT_JSON,
        NOT_A_BACKUP,
        UNSUPPORTED_VERSION,
        MISSING_FIELD,
        UNKNOWN_FIELD,
        WRONG_TYPE,
        INVALID_VALUE,
        DUPLICATE_ID,
        /** Schedule violates domain rules ([cz.nowass.dispenser.domain.ScheduleValidator]). */
        INVALID_SCHEDULE,
        /** Simulation state and history do not fit together. */
        INCONSISTENT_SIMULATION,
        TOO_MANY_ITEMS,
        /** Imported schedule id equals an unreadable record kept in the phone (R4). */
        ID_COLLISION_UNREADABLE,
        READ_FAILED,
    }
}
