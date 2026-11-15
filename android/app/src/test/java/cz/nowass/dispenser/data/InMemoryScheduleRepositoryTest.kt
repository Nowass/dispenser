package cz.nowass.dispenser.data

import cz.nowass.dispenser.data.MockSeed.Companion.START
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import java.time.Clock

class InMemoryScheduleRepositoryTest : ScheduleRepositoryContract() {
    override fun newRepository(ids: List<String>): LocalDispenserRepository {
        val it = ids.iterator()
        return LocalDispenserRepository(
            store = InMemoryScheduleStore(),
            clock = Clock.fixed(START, DEFAULT_ZONE),
            idGenerator = { it.next() },
            clockTick = null,
        )
    }
}
