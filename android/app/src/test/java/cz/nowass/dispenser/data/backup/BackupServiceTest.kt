package cz.nowass.dispenser.data.backup

import android.content.Context
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.test.core.app.ApplicationProvider
import cz.nowass.dispenser.data.CommandResult
import cz.nowass.dispenser.data.DataStoreAppSettingsRepository
import cz.nowass.dispenser.data.LocalDispenserRepository
import cz.nowass.dispenser.data.ScheduleWriteResult
import cz.nowass.dispenser.data.local.DispenserDatabase
import cz.nowass.dispenser.data.local.RoomScheduleStore
import cz.nowass.dispenser.data.local.ScheduleEntity
import cz.nowass.dispenser.data.local.simulation.RoomSimulationStore
import cz.nowass.dispenser.data.local.simulation.SimEventEntity
import cz.nowass.dispenser.data.local.simulation.SimProcessedEntity
import cz.nowass.dispenser.data.local.simulation.SimStateEntity
import cz.nowass.dispenser.data.local.simulation.SimulationDatabase
import cz.nowass.dispenser.domain.DEFAULT_ZONE
import cz.nowass.dispenser.domain.Recurrence
import cz.nowass.dispenser.domain.RecurrenceKind
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.SyncState
import cz.nowass.dispenser.domain.simulation.HistoryEventType
import cz.nowass.dispenser.domain.toDraft
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import java.time.Clock
import java.time.DayOfWeek
import java.time.Duration
import java.time.Instant
import java.time.LocalDate
import java.time.LocalTime

/**
 * Backup service against the real persistence stack (Room `dispenser.db` + `simulation.db`,
 * DataStore, app-private files) with failure injection at every persistence boundary.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class BackupServiceTest {

    @get:Rule val tmp = TemporaryFolder()

    private val context: Context = ApplicationProvider.getApplicationContext()
    private val t0: Instant = Instant.parse("2026-11-20T05:00:00Z")
    private val clock: Clock = Clock.fixed(t0, DEFAULT_ZONE)

    data class Fingerprint(
        val schedules: List<ScheduleEntity>,
        val state: SimStateEntity?,
        val processed: List<SimProcessedEntity>,
        val events: List<SimEventEntity>,
        val displayNameRaw: String?,
    )

    /** One isolated installation: own databases, DataStore file and backup directory. */
    inner class Env(private val name: String) : AutoCloseable {
        val dir = File(tmp.root, name).apply { mkdirs() }
        private val backupDir = File(dir, "backup")
        private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        val settings = DataStoreAppSettingsRepository(PreferenceDataStoreFactory.create(scope = scope) { File(dir, "settings.preferences_pb") })
        val faults = Faults()
        private var runs = 0
        private var snapshots = 0

        init {
            context.deleteDatabase("$name.db"); context.deleteDatabase("$name-sim.db")
        }

        var db: DispenserDatabase = DispenserDatabase.build(context, "$name.db")
        var simDb: SimulationDatabase = SimulationDatabase.build(context, "$name-sim.db")

        /** App restart: databases closed and reopened (DataStore stays: one instance per file and process). */
        fun restart() {
            db.close(); simDb.close()
            db = DispenserDatabase.build(context, "$name.db")
            simDb = SimulationDatabase.build(context, "$name-sim.db")
        }

        fun scheduleStore() = RoomScheduleStore(db.scheduleDao(), clock, onInvalidRow = { _, _ -> })
        fun simulationStore() = RoomSimulationStore(simDb.simulationDao(), log = {})
        fun files() = DirectoryRestoreFiles(backupDir)

        fun repo() = LocalDispenserRepository(
            store = scheduleStore(), clock = clock, simulationStore = simulationStore(), clockTick = null,
            simulationRunIds = { "$name-run-${++runs}" },
        )

        fun service(withFaults: Boolean = true) = BackupService(
            schedules = if (withFaults) FaultyScheduleStore(scheduleStore(), faults) else scheduleStore(),
            simulation = if (withFaults) FaultySimulationStore(simulationStore(), faults) else simulationStore(),
            settings = if (withFaults) FaultySettings(settings, faults) else settings,
            files = if (withFaults) FaultyFiles(files(), faults) else files(),
            access = MutexAccess(),
            clock = clock,
            appVersion = { "0.4.0 (4)" },
            snapshotIds = { "$name-snapshot-${++snapshots}" },
        )

        fun journalExists() = files().journalExists()

        suspend fun fingerprint(): Fingerprint {
            val dao = simDb.simulationDao()
            return Fingerprint(
                db.scheduleDao().getAll(),
                dao.getState(),
                dao.getProcessed().sortedBy { it.occurrenceId },
                dao.getEvents(),
                settings.captureRaw(),
            )
        }

        /** Typical user data: two schedules (one DEVICE_APPLIED row), one unreadable row, a name, a running simulation. */
        suspend fun seed() {
            val r = repo()
            assertTrue(r.createSchedule(MORNING) is ScheduleWriteResult.Success)
            assertTrue(r.createSchedule(WEEKEND) is ScheduleWriteResult.Success)
            db.scheduleDao().insert(UNREADABLE)
            settings.setDisplayName("Původní akvárium")
            assertTrue(r.simulation.start(t0, 500_000, 400_000) is CommandResult.Done)
            assertTrue(r.simulation.advanceDays(2) is CommandResult.Done)
        }

        override fun close() {
            db.close(); simDb.close(); scope.cancel()
            context.deleteDatabase("$name.db"); context.deleteDatabase("$name-sim.db")
        }
    }

    private fun env(name: String) = Env(name)

    private val newSchedules = listOf(
        Schedule("n1", 4, "Nová ranní", true, 7.25, LocalDate.of(2026, 11, 1), LocalTime.of(9, 15), Recurrence.Daily(1)),
        Schedule("n2", 1, "Nová jednorázová", false, 0.5, LocalDate.of(2026, 12, 24), LocalTime.of(18, 0), Recurrence.Once),
    )
    private val newContent = BackupContent(Instant.parse("2026-11-14T08:00:00Z"), "0.4.0 (4)", "Nové akvárium", newSchedules, null)

    private suspend fun BackupService.plan(content: BackupContent): RestorePlan {
        val r = prepare(BackupCodec.encode(content).toByteArray())
        if (r !is PrepareResult.Ready) fail("prepare failed: $r")
        return (r as PrepareResult.Ready).plan
    }

    // ------------------------------------------------------------------ round trip & semantics

    @Test
    fun exportRestore_roundTrip_persistsAfterRestart_restoredSchedulesArePending() = runBlocking {
        env("roundtrip").use { e ->
            e.seed()
            // A row confirmed by a device (DEVICE_APPLIED) must come back as PENDING after restore.
            val applied = e.db.scheduleDao().getAll().first { it.syncState == "PENDING" }.copy(syncState = "DEVICE_APPLIED")
            e.db.scheduleDao().replaceIfRevision(applied, applied.revision)
            val before = e.repo().observeSchedules().first()

            val export = e.service().export(includeSimulation = false)
            assertEquals(1, export.unreadableSkipped)
            assertEquals(2, export.content.schedules.size)
            assertEquals("Původní akvárium", export.content.displayName)

            // Change everything, then restore the export.
            val r = e.repo()
            before.forEach { r.deleteSchedule(it.id, it.revision) }
            e.settings.setDisplayName("Jiné")
            val result = e.service().restore(e.service().plan(BackupCodec.decode(export.bytes).let { (it as BackupCodec.Decoded.Valid).content }))
            assertEquals(RestoreResult.Restored(2, SimulationEffect.DEACTIVATED), result)

            e.restart()
            val after = e.repo()
            assertEquals(before.sortedBy { it.id }, after.observeSchedules().first().sortedBy { it.id }) // ids, revisions, recurrence
            assertEquals(setOf(SyncState.PENDING), after.observeScheduleSync().first().values.toSet())
            assertEquals("Původní akvárium", e.settings.displayName.first())
            assertFalse(e.journalExists())
        }
    }

    @Test
    fun restore_replacesInsteadOfMerging_emptyBackupClearsReadableSchedules() = runBlocking {
        env("replace").use { e ->
            e.seed()
            val s = e.service()
            assertTrue(s.restore(s.plan(newContent)) is RestoreResult.Restored)
            assertEquals(listOf("n1", "n2"), e.repo().observeSchedules().first().map { it.id }.sorted())
            assertEquals(4, e.repo().observeSchedules().first().first { it.id == "n1" }.revision)

            val empty = newContent.copy(schedules = emptyList(), displayName = null)
            assertTrue(s.restore(s.plan(empty)) is RestoreResult.Restored)
            assertTrue(e.repo().observeSchedules().first().isEmpty())
            assertNull(e.settings.displayName.first())
            // The unreadable record is never touched.
            assertEquals(listOf(UNREADABLE), e.db.scheduleDao().getAll())
        }
    }

    @Test
    fun prepare_isReadOnly_cancellingConfirmationChangesNothing() = runBlocking {
        env("prepare").use { e ->
            e.seed()
            val before = e.fingerprint()
            val plan = e.service().plan(newContent)
            assertEquals(2, plan.currentScheduleCount)
            assertEquals(1, plan.currentUnreadableCount)
            assertEquals("Původní akvárium", plan.currentDisplayName)
            assertEquals(SimulationEffect.DEACTIVATED, plan.simulationEffect)
            // The user cancels the confirmation: restore() is never called.
            assertEquals(before, e.fingerprint())
            assertFalse(e.journalExists())
            assertFalse(File(e.dir, "backup/${DirectoryRestoreFiles.SNAPSHOT_FILE}").exists())
        }
    }

    @Test
    fun invalidBackup_isRejectedBeforeAnyWrite() = runBlocking {
        env("invalid").use { e ->
            e.seed()
            val before = e.fingerprint()
            val s = e.service()
            listOf("not json", "{}", BackupCodec.encode(newContent).replace("\"schemaVersion\": 1", "\"schemaVersion\": 9")).forEach {
                assertTrue(s.prepare(it.toByteArray()) is PrepareResult.Invalid)
            }
            assertEquals(before, e.fingerprint())
            assertTrue(e.faults.visited.isEmpty())
        }
    }

    // ------------------------------------------------------------------ unreadable rows (R4)

    @Test
    fun idCollisionWithUnreadableRow_isRejected_rowsUnchanged() = runBlocking {
        env("collision").use { e ->
            e.seed()
            val before = e.fingerprint()
            val colliding = newContent.copy(schedules = newSchedules + newSchedules[0].copy(id = UNREADABLE.id))
            val s = e.service()
            val prepared = s.prepare(BackupCodec.encode(colliding).toByteArray())
            assertEquals(PrepareResult.Invalid(BackupError(BackupError.Kind.ID_COLLISION_UNREADABLE, "schedules[2].id")), prepared)
            // Even a plan built before the row became unreadable is re-checked under the lock.
            val stalePlan = RestorePlan(colliding, 0, 0, null, 0, SimulationEffect.NONE)
            assertEquals(RestoreResult.NotStarted(BackupError(BackupError.Kind.ID_COLLISION_UNREADABLE, "schedules[2].id")), s.restore(stalePlan))
            assertEquals(before, e.fingerprint())
        }
    }

    // ------------------------------------------------------------------ failure injection (R1)

    private val commonPoints = listOf(
        "schedules.captureRaw.before",
        "files.writeSnapshot.before", "files.writeSnapshot.after",
        "files.writeJournal.before", "files.writeJournal.after",
        "schedules.replace.before", "schedules.replace.after",
        "settings.set.before", "settings.set.after",
        "files.deleteJournal.before", "files.deleteJournal.after",
    )

    @Test
    fun inProcessFailure_atEveryBoundary_leavesStoresExactlyUnchanged() = runBlocking {
        env("faults").use { e ->
            e.seed()
            val before = e.fingerprint()
            val cases = (commonPoints + listOf("simulation.deactivate.before", "simulation.deactivate.after")).map { it to newContent } +
                listOf("simulation.replaceAll.before", "simulation.replaceAll.after").map {
                    it to newContent.copy(simulation = SimulationBackup(null, emptyList()))
                }
            for ((point, content) in cases) {
                val s = e.service()
                val plan = s.plan(content)
                e.faults.failAt += point
                val result = s.restore(plan)
                assertTrue("$point -> $result", result is RestoreResult.RolledBack || result is RestoreResult.NotStarted)
                assertTrue("fault $point not reached", point !in e.faults.failAt)
                assertEquals("stores changed after failure at $point", before, e.fingerprint())
                assertFalse("journal left after $point", e.journalExists())
                assertEquals(StorageState.Ready(), s.state.value)
            }
        }
    }

    @Test
    fun rollbackFailure_blocksApp_startupRecoveryThenRestoresExactly() = runBlocking {
        env("rollbackfail").use { e ->
            e.seed()
            val before = e.fingerprint()
            val s = e.service()
            val plan = s.plan(newContent)
            e.faults.failAt += setOf("settings.set.before", "simulation.restoreRaw.before")
            assertEquals(RestoreResult.Blocked, s.restore(plan))
            assertEquals(StorageState.Blocked(BlockReason.ROLLBACK_FAILED), s.state.value)
            assertTrue(e.journalExists())
            // Further restores are refused while blocked.
            assertEquals(RestoreResult.NotStarted(null), s.restore(plan))

            e.restart()
            val restarted = e.service(withFaults = false)
            assertEquals(StorageState.Recovering, restarted.state.value)
            assertEquals(StorageState.Ready(RecoveryNotice.INTERRUPTED_RESTORE_ROLLED_BACK), restarted.recoverAtStartup())
            assertEquals(before, e.fingerprint())
            assertFalse(e.journalExists())
            restarted.acknowledgeNotice()
            assertEquals(StorageState.Ready(), restarted.state.value)
        }
    }

    @Test
    fun processDeath_atEveryBoundary_isRecoveredAtStartup() = runBlocking {
        val points = commonPoints + "simulation.deactivate.before" + "simulation.deactivate.after"
        points.forEachIndexed { i, point ->
            env("death$i").use { e ->
                e.seed()
                val before = e.fingerprint()
                val s = e.service()
                val plan = s.plan(newContent)
                e.faults.failAt += point
                e.faults.die = true
                try {
                    s.restore(plan)
                    fail("expected process death at $point")
                } catch (_: ProcessDeath) {
                }

                e.restart()
                val journal = e.journalExists()
                val restarted = e.service(withFaults = false)
                assertEquals(if (journal) StorageState.Recovering else StorageState.Ready(), restarted.state.value)
                val recovered = restarted.recoverAtStartup()
                assertFalse(e.journalExists())
                if (point == "files.deleteJournal.after") {
                    // Commit point passed: the restore is complete.
                    assertEquals(StorageState.Ready(), recovered)
                    assertEquals(listOf("n1", "n2"), e.repo().observeSchedules().first().map { it.id }.sorted())
                    assertEquals("Nové akvárium", e.settings.displayName.first())
                    assertEquals(listOf(UNREADABLE), e.db.scheduleDao().getAll().filter { it.id == UNREADABLE.id })
                } else {
                    assertEquals("$point", if (journal) StorageState.Ready(RecoveryNotice.INTERRUPTED_RESTORE_ROLLED_BACK) else StorageState.Ready(), recovered)
                    assertEquals("data after interruption at $point", before, e.fingerprint())
                }
            }
        }
    }

    @Test
    fun journalWithoutUsableSnapshot_blocksStartup() = runBlocking {
        env("nosnapshot").use { e ->
            e.seed()
            e.files().writeJournal(SafetySnapshotCodec.encodeJournal("missing"))
            val s = e.service(withFaults = false)
            assertEquals(StorageState.Recovering, s.state.value)
            assertEquals(StorageState.Blocked(BlockReason.SNAPSHOT_UNAVAILABLE), s.recoverAtStartup())
            assertTrue("journal is kept for a later retry", e.journalExists())

            // Mismatching snapshot id is not used either.
            e.files().writeSnapshot(SafetySnapshotCodec.encode(SafetySnapshot("other", t0, RawScheduleRows(emptyList()), RawSimulationRows.EMPTY, null)))
            assertEquals(StorageState.Blocked(BlockReason.SNAPSHOT_UNAVAILABLE), s.recoverAtStartup())
            assertEquals(2, e.repo().observeSchedules().first().size) // nothing was applied
        }
    }

    @Test
    fun safetySnapshot_preservesUnreadableRowsWithAllColumns() = runBlocking {
        env("snapshot").use { e ->
            e.seed()
            val raw = e.scheduleStore().captureRaw()
            val decoded = SafetySnapshotCodec.decode(SafetySnapshotCodec.encode(SafetySnapshot("x", t0, raw, e.simulationStore().captureRaw(), "  raw ")))
            assertEquals(raw, decoded.schedules)
            assertEquals("  raw ", decoded.displayNameRaw)
            e.scheduleStore().restoreRaw(decoded.schedules)
            assertTrue(e.db.scheduleDao().getAll().contains(UNREADABLE))
        }
    }

    // ------------------------------------------------------------------ simulation (R2)

    @Test
    fun restoreWithSimulation_replacesRun_andPreservesAtMostOnce() = runBlocking {
        val exported: ByteArray
        val runId: String
        env("simsource").use { src ->
            src.seed()
            // Clock 09:00 local: today's 08:00 dose is processed.
            assertTrue(src.repo().simulation.advanceHours(3) is CommandResult.Done)
            exported = src.service().export(includeSimulation = true).bytes
            runId = src.simulationStore().currentState()!!.runId
        }
        env("simtarget").use { e ->
            val s = e.service()
            val plan = s.plan((BackupCodec.decode(exported) as BackupCodec.Decoded.Valid).content)
            assertEquals(SimulationEffect.REPLACED, plan.simulationEffect)
            assertTrue(s.restore(plan) is RestoreResult.Restored)
            e.restart()

            val repo = e.repo()
            val state = repo.simulation.observeSimulation().first()!!
            assertEquals(runId, state.runId)
            val history = repo.observeHistory().first()
            assertTrue(history.isNotEmpty())
            assertTrue(history.all { it.origin == cz.nowass.dispenser.domain.simulation.EventOrigin.SIMULATION })

            // Today's 08:00 dose was already processed. Moving it later today must not dose again.
            val today = state.clock.atZone(DEFAULT_ZONE).toLocalDate()
            val morning = repo.observeSchedules().first().first { it.name == MORNING.name }
            assertTrue(morning.id + "@" + today in state.processedOccurrences)
            val moved = repo.updateSchedule(morning.id, morning.revision, morning.toDraft().copy(localTime = state.clock.atZone(DEFAULT_ZONE).toLocalTime().plusHours(1)))
            assertTrue(moved is ScheduleWriteResult.Success)
            assertTrue(repo.simulation.advanceHours(3) is CommandResult.Done)
            val doses = repo.observeHistory().first().filter { it.occurrenceId == "${morning.id}@$today" && it.type == HistoryEventType.DISPENSE_SUCCESS }
            assertEquals("at most once per occurrence", 1, doses.size)
        }
    }

    @Test
    fun restoreWithoutSimulation_changedSchedules_stopsRun_keepsHistory_restartContinuesSeq() = runBlocking {
        env("deactivate").use { e ->
            e.seed()
            val historyBefore = e.repo().observeHistory().first()
            assertTrue(historyBefore.isNotEmpty())
            val s = e.service()
            assertEquals(RestoreResult.Restored(2, SimulationEffect.DEACTIVATED), s.restore(s.plan(newContent)))

            val repo = e.repo()
            assertNull(repo.simulation.observeSimulation().first())
            assertEquals(historyBefore, repo.observeHistory().first())
            assertNull(repo.observeReservoir().first())

            // Explicit restart required; the new run's events follow the kept history.
            assertTrue(repo.simulation.start(t0.plus(Duration.ofDays(5)), 500_000, 50_000) is CommandResult.Done)
            val all = repo.observeHistory().first()
            assertTrue(all.size > historyBefore.size)
            assertEquals(all.size, all.map { it.id }.toSet().size)
            assertTrue(all.drop(historyBefore.size).all { it.seq > historyBefore.maxOf { h -> h.seq } })
        }
    }

    @Test
    fun unchangedIds_withModifiedRecurrence_stopTheRun() = runBlocking {
        env("modified").use { e ->
            e.seed()
            val current = e.repo().observeSchedules().first()
            val modified = current.map { if (it.name == MORNING.name) it.copy(recurrence = Recurrence.Daily(3)) else it }
            val s = e.service()
            val plan = s.plan(newContent.copy(schedules = modified))
            assertEquals(SimulationEffect.DEACTIVATED, plan.simulationEffect)
            assertTrue(s.restore(plan) is RestoreResult.Restored)
            assertNull(e.simulationStore().currentState())
            assertEquals(Recurrence.Daily(3), e.repo().observeSchedules().first().first { it.name == MORNING.name }.recurrence)
        }
    }

    @Test
    fun identicalSchedules_keepTheRun_noActiveRun_keepsHistory() = runBlocking {
        env("identical").use { e ->
            e.seed()
            val stateBefore = e.simulationStore().currentState()
            val current = e.repo().observeSchedules().first().map { it.copy(revision = it.revision + 5) }
            val s = e.service()
            val plan = s.plan(newContent.copy(schedules = current))
            assertEquals(SimulationEffect.CONTINUES, plan.simulationEffect)
            assertTrue(s.restore(plan) is RestoreResult.Restored)
            assertEquals(stateBefore, e.simulationStore().currentState())

            // Without an active run, simulation data is left as is.
            e.simulationStore().deactivate()
            val history = e.repo().observeHistory().first()
            val plan2 = s.plan(newContent)
            assertEquals(SimulationEffect.NONE, plan2.simulationEffect)
            assertTrue(s.restore(plan2) is RestoreResult.Restored)
            assertEquals(history, e.repo().observeHistory().first())
            assertNotEquals(0, history.size)
        }
    }

    @Test
    fun export_isConsistentAndSelfValidating_withSimulationMarkedSimulated() = runBlocking {
        env("export").use { e ->
            e.seed()
            val data = e.service().export(includeSimulation = true)
            val text = data.bytes.toString(Charsets.UTF_8)
            assertTrue(text.contains("\"dataOrigin\": \"SIMULATION\""))
            assertFalse(text.contains("\"DEVICE\""))
            assertFalse(text.contains(UNREADABLE.name))
            val decoded = BackupCodec.decode(data.bytes) as BackupCodec.Decoded.Valid
            assertEquals(data.content, decoded.content)
            assertEquals(e.simulationStore().currentState(), decoded.content.simulation!!.state)
            assertNull(e.service().export(includeSimulation = false).content.simulation)
        }
    }

    private companion object {
        val MORNING = ScheduleDraft("Ranní", 10.0, LocalDate.of(2026, 11, 1), LocalTime.of(8, 0), RecurrenceKind.DAILY)
        val WEEKEND = ScheduleDraft(
            "Víkend", 12.345, LocalDate.of(2026, 11, 1), LocalTime.of(10, 30), RecurrenceKind.WEEKLY,
            weekdays = setOf(DayOfWeek.SATURDAY, DayOfWeek.SUNDAY), endDate = LocalDate.of(2027, 3, 31),
        )

        /** Fails domain validation (unknown recurrence kind): must survive every operation untouched. */
        val UNREADABLE = ScheduleEntity(
            id = "corrupted-1", revision = 2, name = "Poškozený", enabled = true, amountMicrolitres = 5_000,
            startDate = "2026-11-01", localTime = "07:00", zoneId = "Europe/Prague", recurrenceKind = "BOGUS",
            repeatInterval = 1, weekdaysMask = 0, endDate = null, syncState = "PENDING", updatedAtEpochMs = 123_456_789L,
        )
    }
}
