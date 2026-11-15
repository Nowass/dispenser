package cz.nowass.dispenser.data.local.simulation

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

/**
 * Simulation-only database (`simulation.db`), deliberately separate from the schedule
 * database (`dispenser.db`): simulated runs and history can never modify or reset user
 * schedules. "Reset simulace" clears these tables only. No destructive migrations.
 */
@Database(
    entities = [SimStateEntity::class, SimProcessedEntity::class, SimEventEntity::class],
    version = 1,
    exportSchema = true,
)
abstract class SimulationDatabase : RoomDatabase() {
    abstract fun simulationDao(): SimulationDao

    companion object {
        const val FILE_NAME = "simulation.db"

        fun build(context: Context, name: String = FILE_NAME): SimulationDatabase =
            Room.databaseBuilder(context.applicationContext, SimulationDatabase::class.java, name).build()
    }
}
