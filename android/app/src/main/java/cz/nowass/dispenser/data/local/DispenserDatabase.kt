package cz.nowass.dispenser.data.local

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

/**
 * Local app database (schedules only in A1.2c).
 *
 * Schema changes must ship explicit migrations; destructive migration fallbacks are
 * never configured, because they would silently delete user schedules. A fresh
 * installation starts with an empty database (no demo schedules are inserted).
 */
@Database(entities = [ScheduleEntity::class], version = 1, exportSchema = true)
abstract class DispenserDatabase : RoomDatabase() {
    abstract fun scheduleDao(): ScheduleDao

    companion object {
        const val FILE_NAME = "dispenser.db"

        fun build(context: Context, name: String = FILE_NAME): DispenserDatabase =
            Room.databaseBuilder(context.applicationContext, DispenserDatabase::class.java, name).build()
    }
}
