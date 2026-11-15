package cz.nowass.dispenser.data.local.simulation

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.AppContainer
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.simulation.SimulationEngine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class RoomSimulationStoreTest {

    private val context: Context = ApplicationProvider.getApplicationContext()
    private val name = "simulation-test.db"
    private val open = mutableListOf<SimulationDatabase>()
    private val engine = SimulationEngine()
    private val t0 = Instant.parse("2026-11-20T06:00:00Z")

    @Before fun clean() { context.deleteDatabase(name); context.deleteDatabase(DispenserDatabase.FILE_NAME); context.deleteDatabase(SimulationDatabase.FILE_NAME) }
    @After fun close() { open.forEach { it.close() }; clean() }

    private fun store() = RoomSimulationStore(SimulationDatabase.build(context, name).also { open += it }.simulationDao(), log = {})

    private val schedule = cz.nowass.dispenser.domain.Schedule(
        "m", 1, "Ranní", true, 10.0, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), cz.nowass.dispenser.domain.Recurrence.Daily(),
    )

    @Test
    fun stateAndHistory_roundTrip_andSurviveReopen() = runBlocking {
        val s1 = store()
        val started = engine.start("run", t0, 500_000, 25_000).let { it as cz.nowass.dispenser.domain.simulation.SimulationResult.Ok }
        s1.commit(started.state, started.events)
        val advanced = engine.advanceDays(started.state, 3, listOf(schedule)) as cz.nowass.dispenser.domain.simulation.SimulationResult.Ok
        s1.commit(advanced.state, advanced.events)
        open.forEach { it.close() }; open.clear()

        val s2 = store()
        assertEquals(advanced.state, s2.currentState())
        assertEquals(advanced.state, s2.observeState().first())
        assertEquals(started.events + advanced.events, s2.observeHistory().first())
    }

    @Test
    fun duplicateEventId_abortsTheWholeCommit() = runBlocking {
        val s = store()
        val started = engine.start("run", t0, 500_000, 400_000) as cz.nowass.dispenser.domain.simulation.SimulationResult.Ok
        val r = engine.advanceDays(started.state, 1, listOf(schedule)) as cz.nowass.dispenser.domain.simulation.SimulationResult.Ok
        s.commit(r.state, r.events)
        try {
            s.commit(r.state.copy(nextSeq = 99), r.events) // same immutable event ids again
            fail("expected constraint violation")
        } catch (_: Exception) {
        }
        assertEquals(r.state, s.currentState()) // state not partially updated
        assertEquals(r.events, s.observeHistory().first())
    }

    @Test
    fun clear_removesOnlySimulationData() = runBlocking {
        val s = store()
        val started = engine.start("run", t0, 500_000, 400_000) as cz.nowass.dispenser.domain.simulation.SimulationResult.Ok
        s.commit(started.state, started.events)
        s.clear()
        assertNull(s.currentState())
        assertTrue(s.observeHistory().first().isEmpty())
    }

    @Test
    fun appWiring_usesSeparateDatabases_resetKeepsSchedules() = runBlocking {
        val container = AppContainer(context)
        val repo = container.repository as LocalDispenserRepository
        repo.createSchedule(ScheduleDraft("Ranní", 10.0, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), RecurrenceKind.DAILY))
        container.simulation.start(t0, 500_000, 400_000)
        container.simulation.advanceDays(5)
        assertTrue(repo.observeHistory().first().isNotEmpty())

        container.simulation.reset()
        assertEquals(1, repo.observeSchedules().first().size) // user schedules untouched
        assertTrue(repo.observeHistory().first().isEmpty())
        assertTrue(context.getDatabasePath(DispenserDatabase.FILE_NAME).exists())
        assertTrue(context.getDatabasePath(SimulationDatabase.FILE_NAME).exists())
    }
}
