package cz.nowass.dispenser.data.local

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.MockSeed
import cz.nowass.dispenser.data.ScheduleRepositoryContract
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import org.junit.After
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Clock

/** The repository contract, executed against a real Room database (Robolectric, JVM). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class RoomScheduleRepositoryTest : ScheduleRepositoryContract() {

    private val databases = mutableListOf<DispenserDatabase>()

    override fun newRepository(ids: List<String>): LocalDispenserRepository {
        val db = Room.inMemoryDatabaseBuilder(ApplicationProvider.getApplicationContext<Context>(), DispenserDatabase::class.java).build()
        databases += db
        val clock = Clock.fixed(MockSeed.START, DEFAULT_ZONE)
        val it = ids.iterator()
        return LocalDispenserRepository(RoomScheduleStore(db.scheduleDao(), clock), clock, idGenerator = { it.next() }, clockTick = null)
    }

    @After
    fun closeDatabases() = databases.forEach { it.close() }
}
