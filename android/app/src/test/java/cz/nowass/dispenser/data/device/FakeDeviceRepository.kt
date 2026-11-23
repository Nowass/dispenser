package cz.nowass.dispenser.data.device

import cz.nowass.dispenser.device.ops.BatteryRecord
import cz.nowass.dispenser.device.ops.ClockTrust
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DiagnosticsRecord
import cz.nowass.dispenser.device.ops.InviteRecord
import cz.nowass.dispenser.device.ops.ManualDoseRecord
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.PhoneRecord
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.ops.StatusRecord
import cz.nowass.dispenser.device.ops.TimeDecisionRecord
import cz.nowass.dispenser.device.session.DeviceEvent
import cz.nowass.dispenser.device.session.SessionState
import cz.nowass.dispenser.domain.ScheduleDraft
import cz.nowass.dispenser.domain.toSchedule
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.flowOf

/**
 * Scripted [DeviceRepository] for the DEVICE-mode adapter and ViewModel tests: freshness and outcomes
 * are set directly, so every mapping (Live / Cached / Unknown, Uncertain / NotSent / …) is exercised
 * deterministically. The real repository is covered against the firmware (DeviceRepositoryFirmwareTest,
 * DeviceUiRepositoryFirmwareTest).
 */
class FakeDeviceRepository(override val deviceId: String = "a0b1c2d3e4f5") : DeviceRepository {
    override val connection = MutableStateFlow<SessionState>(SessionState.Idle)
    override val events: SharedFlow<DeviceEvent> = MutableSharedFlow()

    val statusF = MutableStateFlow<Fresh<StatusRecord>>(Fresh.Unknown)
    val schedulesF = MutableStateFlow<Fresh<DeviceSchedules>>(Fresh.Unknown)
    val batteryF = MutableStateFlow<Fresh<BatteryRecord>>(Fresh.Unknown)

    /** Next outcome of a schedule write / refill. */
    var writeOutcome: OpResult<ScheduleWrite> = OpResult.NotSent("not connected")
    var refillOutcome: OpResult<StatusRecord> = OpResult.NotSent("not connected")
    val calls = mutableListOf<String>()

    override fun status(): Flow<Fresh<StatusRecord>> = statusF
    override fun schedules(): Flow<Fresh<DeviceSchedules>> = schedulesF
    override fun battery(): Flow<Fresh<BatteryRecord>> = batteryF
    /** Scripted battery refresh (the card tap); counts calls and in-flight overlaps. */
    var batteryRefresh: suspend () -> OpResult<BatteryRecord> = { OpResult.NotSent("not connected") }
    var batteryRefreshCalls = 0
    var batteryRefreshInFlight = 0
    var batteryRefreshOverlaps = 0
    override suspend fun refreshBattery(): OpResult<BatteryRecord> {
        batteryRefreshCalls++
        if (++batteryRefreshInFlight > 1) batteryRefreshOverlaps++
        try {
            return batteryRefresh()
        } finally {
            batteryRefreshInFlight--
        }
    }
    val configF = MutableStateFlow<Fresh<ConfigRecord>>(Fresh.Unknown)
    override fun config(): Flow<Fresh<ConfigRecord>> = configF
    override fun diagnostics(): Flow<Fresh<DiagnosticsRecord>> = flowOf(Fresh.Unknown)
    override fun phones(): Flow<Fresh<List<PhoneRecord>>> = flowOf(Fresh.Unknown)
    val pendingF = MutableStateFlow<List<PendingOp>>(emptyList())
    override fun pendingOperations(): Flow<List<PendingOp>> = pendingF

    var connectOutcome: ConnectOutcome = ConnectOutcome.NoAddress
    var connectThrows: Exception? = null
    var disconnects = 0
    val disconnectModes = mutableListOf<Boolean>()
    override suspend fun connect(): ConnectOutcome { connectThrows?.let { throw it }; return connectOutcome }
    override suspend fun disconnect(graceful: Boolean) { disconnects++; disconnectModes += graceful }
    /** Runs inside createInvite before it answers (e.g. suspends while the caller is cancelled). */
    var inviteHook: suspend () -> Unit = {}
    var inviteOutcome: OpResult<InviteRecord> = OpResult.NotSent("not connected")
    var cancelOutcome: OpResult<Unit> = OpResult.NotSent("not connected")
    var phonesOutcome: OpResult<List<PhoneRecord>> = OpResult.NotSent("not connected")
    var removeOutcome: OpResult<Unit> = OpResult.NotSent("not connected")

    override suspend fun createSchedule(draft: ScheduleDraft): OpResult<ScheduleWrite> { calls += "create"; return writeOutcome }
    override suspend fun updateSchedule(id: String, expectedRevision: Long, draft: ScheduleDraft): OpResult<ScheduleWrite> {
        calls += "update $id $expectedRevision enabled=${draft.enabled}"; return writeOutcome
    }
    override suspend fun deleteSchedule(id: String, expectedRevision: Long): OpResult<ScheduleWrite> { calls += "delete $id"; return writeOutcome }
    override suspend fun refill(remainingUl: Long, capacityUl: Long?): OpResult<StatusRecord> {
        calls += if (capacityUl == null) "refill $remainingUl" else "refill $remainingUl capacity $capacityUl"; return refillOutcome
    }

    private fun no(): Nothing = throw UnsupportedOperationException("not used by the shared-screen adapter")
    // bench pump controller (debug tools): scripted outcomes, recorded calls
    var statusOutcome: OpResult<StatusRecord>? = null
    var configOutcome: OpResult<ConfigRecord>? = null
    var setConfigOutcome: OpResult<Long> = OpResult.NotSent("not connected")
    var readinessOutcome: OpResult<StatusRecord> = OpResult.NotSent("not connected")
    var lastConfig: ConfigRecord? = null
    override suspend fun refreshStatus(): OpResult<StatusRecord> = statusOutcome ?: no()
    override suspend fun refreshDiagnostics(): OpResult<DiagnosticsRecord> = no()
    override suspend fun ackRejections(): OpResult<Unit> = no()
    var refreshConfigCalls = 0
    override suspend fun refreshConfig(): OpResult<ConfigRecord> { refreshConfigCalls++; return configOutcome ?: OpResult.NotSent("not connected") }
    override suspend fun setConfig(expectedRevision: Long, mask: Int, values: ConfigRecord): OpResult<Long> {
        calls += "setConfig $expectedRevision 0x${mask.toString(16)}"; lastConfig = values; return setConfigOutcome
    }
    override suspend fun refreshSchedules(): OpResult<DeviceSchedules> = no()
    override suspend fun manualDose(volumeUl: Long): OpResult<ManualDoseRecord> = no()
    override suspend fun resendManualDose(journalId: Long): OpResult<ManualDoseRecord> = no()
    override suspend fun discardManualDose(journalId: Long): OpResult<Unit> = no()
    override suspend fun acknowledge(journalId: Long): OpResult<Unit> = no()
    override suspend fun setReadiness(readiness: Readiness): OpResult<StatusRecord> { calls += "readiness $readiness"; return readinessOutcome }
    override suspend fun setTime(): OpResult<TimeDecisionRecord> = no()
    override suspend fun refreshPhones(): OpResult<List<PhoneRecord>> = phonesOutcome
    override suspend fun removePhone(slot: Int): OpResult<Unit> { calls += "remove $slot"; return removeOutcome }
    override suspend fun createInvite(nameHint: String): OpResult<InviteRecord> { calls += "invite $nameHint"; inviteHook(); return inviteOutcome }
    override suspend fun cancelInvite(): OpResult<Unit> { calls += "cancel"; return cancelOutcome }
    override suspend fun cancelRecovery(): OpResult<Unit> = no()

    // firmware update: scripted outcomes, recorded requests
    var updateStatusOutcome: OpResult<cz.nowass.dispenser.device.ops.UpdateStatusRecord> = OpResult.NotSent("not connected")
    var startUpdateOutcome: OpResult<cz.nowass.dispenser.device.ops.UpdateVerdictRecord> = OpResult.NotSent("not connected")
    /** Runs inside startUpdate before it answers (e.g. suspends to test single flight). */
    var startUpdateHook: suspend () -> Unit = {}
    val startUpdateRequests = mutableListOf<cz.nowass.dispenser.device.ops.StartUpdateRequest>()
    var updateStatusCalls = 0
    override suspend fun getUpdateStatus(): OpResult<cz.nowass.dispenser.device.ops.UpdateStatusRecord> { updateStatusCalls++; return updateStatusOutcome }
    override suspend fun startUpdate(request: cz.nowass.dispenser.device.ops.StartUpdateRequest): OpResult<cz.nowass.dispenser.device.ops.UpdateVerdictRecord> {
        startUpdateRequests += request; startUpdateHook(); return startUpdateOutcome
    }

    /** Phase C: the device's history; paged like the firmware (seq < before, newest first). */
    var historyLog: List<cz.nowass.dispenser.device.ops.HistoryEventRecord> = emptyList()
    var historyFaults = false
    /** Non-null: returned instead of paging [historyLog] (refusals, lost answers …). */
    var historyOutcome: OpResult<cz.nowass.dispenser.device.ops.HistoryPageRecord>? = null
    override suspend fun history(beforeSeq: Long, max: Int): OpResult<cz.nowass.dispenser.device.ops.HistoryPageRecord> {
        calls += "history $beforeSeq $max"
        historyOutcome?.let { return it }
        val all = historyLog.sortedByDescending { it.seq }
        val page = all.filter { beforeSeq == 0L || it.seq < beforeSeq }.take(max)
        return OpResult.Ok(cz.nowass.dispenser.device.ops.HistoryPageRecord(all.firstOrNull()?.seq ?: 0, all.lastOrNull()?.seq ?: 0, historyFaults, page))
    }

    fun authenticated(generation: Long = 7) {
        connection.value = SessionState.Authenticated(deviceId, generation, slot = 0)
    }

    companion object {
        fun status(capacityUl: Long = 200_000, remainingUl: Long = 150_000, battery: Int? = 80) = StatusRecord(
            readiness = Readiness.READY, dosingFlags = 0, gates = 0, pumpProblems = 0,
            capacityUl = capacityUl, remainingUl = remainingUl, lowPercent = 20, estimateUncertain = false,
            clockTrust = ClockTrust.SYNCED, clockReason = 0, deviceUtc = 1_795_000_000, nextDueAt = null,
            pendingNotes = 0, highestSeverity = 0, storageOk = true, sessionSecondsLeft = 600,
            batteryPercent = battery, manualFloor = 1,
        )

        fun schedule(draft: ScheduleDraft, id: String, revision: Long): DeviceSchedule {
            val record = (DeviceScheduleMapping.toRecord(draft, id, revision) as DeviceScheduleMapping.ToRecord.Ok).record
            return DeviceSchedule(draft.toSchedule(id, revision.toInt()), record)
        }
    }
}
