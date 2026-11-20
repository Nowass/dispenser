package cz.nowass.dispenser.data.onboarding

import cz.nowass.dispenser.data.device.ConnectOutcome
import cz.nowass.dispenser.data.device.DeviceRepository
import cz.nowass.dispenser.data.device.DeviceScheduleMapping
import cz.nowass.dispenser.device.ops.ConfigRecord
import cz.nowass.dispenser.device.ops.DeviceErrorCode
import cz.nowass.dispenser.device.ops.LocalRefusal
import cz.nowass.dispenser.device.ops.OpResult
import cz.nowass.dispenser.device.ops.Readiness
import cz.nowass.dispenser.device.ops.StatusRecord
import cz.nowass.dispenser.domain.Volume
import kotlinx.coroutines.flow.first

/**
 * BENCH ONLY (debug builds): pump configuration and readiness of the connected device, for the
 * hardware bring-up of the GPIO pump control signal. Uses the existing SET_CONFIG and SET_READINESS
 * (journaled, verified by read-back); every firmware rule stays in force: calibration and limits are
 * accepted only in SERVICE_REQUIRED, max_runtime_ms <= the device's hardware maximum, daily >= dose,
 * READY only with every gate except the clock open. READY is never set automatically: only
 * [setReady] sends it, and only when the user asks.
 */

/** Firmware dosing gates (Status.gates, DSP_DOSING_G_*). */
enum class BenchGate(val bit: Int) {
    NOT_LOADED(0), DEGRADED(1), NOT_READY(2), BLOCKED(3), REFILL_PENDING(4), MANUAL_ACTIVE(5),
    CLOCK(6), PUMP_CONFIG(7), RESERVOIR_UNCONFIGURED(8), CREDENTIALS(9);

    companion object {
        fun of(mask: Long): List<BenchGate> = entries.filter { mask and (1L shl it.bit) != 0L }
    }
}

/** Firmware pump-configuration problems behind PUMP_CONFIG (Status.pump_problems, DSP_PUMPCFG_E_*). */
enum class BenchPumpProblem(val bit: Int) {
    FLOW(0), CALIBRATED_AT(1), MAX_DOSE(2), MAX_DAILY(3), MAX_RUNTIME(4), MIN_INTERVAL(5), HW_LIMIT(6);

    companion object {
        fun of(mask: Long): List<BenchPumpProblem> = entries.filter { mask and (1L shl it.bit) != 0L }
    }
}

/** Form fields, as typed: flow in µl/s, dose and daily limit in ml (≤ 3 decimals), runtime in ms, interval in s. */
data class BenchPumpForm(
    val flowUlPerS: String,
    val maxDoseMl: String,
    val maxDailyMl: String,
    val maxRuntimeMs: String,
    val minIntervalS: String,
    /** Explicit bench calibration: sets calibrated_at to the phone's time now. */
    val calibrateNow: Boolean,
)

enum class BenchField { FLOW, MAX_DOSE, MAX_DAILY, MAX_RUNTIME, MIN_INTERVAL }

enum class BenchFieldError { MISSING, NOT_A_NUMBER, NOT_POSITIVE, TOO_PRECISE, TOO_LARGE, ABOVE_HW_MAX, NO_HW_MAX, DAILY_BELOW_DOSE }

data class BenchSnapshot(val config: ConfigRecord, val status: StatusRecord) {
    val ready: Boolean get() = status.readiness == Readiness.READY
    val gates: List<BenchGate> get() = BenchGate.of(status.gates)
    val problems: List<BenchPumpProblem> get() = BenchPumpProblem.of(status.pumpProblems)
}

sealed interface BenchOutcome {
    /** [schedulesAboveMaxDose]: device schedules (as last read) whose amount the max dose now refuses. */
    data class Loaded(val snapshot: BenchSnapshot, val schedulesAboveMaxDose: Int = 0) : BenchOutcome
    /** Not connected / not reachable (nothing changed). */
    data object NotConnected : BenchOutcome
    /** Local validation failed: nothing sent. */
    data class Invalid(val errors: Map<BenchField, BenchFieldError>) : BenchOutcome
    /** Calibration and limits change only in SERVICE_REQUIRED (firmware rule): nothing sent. */
    data object MustBeNotReady : BenchOutcome
    /** The device refused READY: the closed gates and pump problems it reported. */
    data class ReadyRefused(val gates: List<BenchGate>, val problems: List<BenchPumpProblem>) : BenchOutcome
    /** The device refused (code); nothing changed. */
    data class Rejected(val code: Int, val detail: Long) : BenchOutcome
    /** The answer was lost: verified at the next connection; not repeated automatically. */
    data object Uncertain : BenchOutcome
    /** An earlier change of the same kind awaits verification: nothing sent. */
    data object AwaitingVerification : BenchOutcome
}

private val ML = Regex("^[0-9]+([.,][0-9]+)?$")

class BenchPumpController(private val repo: DeviceRepository, private val nowUtc: () -> Long = { System.currentTimeMillis() / 1000 }) {

    /** Connects (the existing session is reused) and reads the configuration and status live. */
    suspend fun load(): BenchOutcome {
        if (repo.connect() !is ConnectOutcome.Connected) return BenchOutcome.NotConnected
        val cfg = (repo.refreshConfig() as? OpResult.Ok)?.value ?: return BenchOutcome.NotConnected
        val st = (repo.refreshStatus() as? OpResult.Ok)?.value ?: return BenchOutcome.NotConnected
        return BenchOutcome.Loaded(BenchSnapshot(cfg, st))
    }

    /** Pure range checks against the device's hardware maximum (no device access). */
    fun validate(form: BenchPumpForm, hwMaxRuntimeMs: Long): Map<BenchField, BenchFieldError> {
        val e = mutableMapOf<BenchField, BenchFieldError>()
        fun num(f: BenchField, text: String): Long? {
            val t = text.trim()
            if (t.isEmpty()) { e[f] = BenchFieldError.MISSING; return null }
            if (!t.all { it.isDigit() }) { e[f] = BenchFieldError.NOT_A_NUMBER; return null }
            val v = t.toBigInteger()
            if (v.signum() == 0) { e[f] = BenchFieldError.NOT_POSITIVE; return null }
            if (v > DeviceScheduleMapping.U32_MAX.toBigInteger()) { e[f] = BenchFieldError.TOO_LARGE; return null }
            return v.toLong()
        }
        fun ml(f: BenchField, text: String): Long? {
            val t = text.trim()
            if (t.isEmpty()) { e[f] = BenchFieldError.MISSING; return null }
            if (!ML.matches(t)) { e[f] = BenchFieldError.NOT_A_NUMBER; return null }
            return when (val c = Volume.mlToMicrolitres(t.replace(',', '.').toDouble())) {
                is Volume.Conversion.Ok -> if (c.microlitres > DeviceScheduleMapping.U32_MAX) { e[f] = BenchFieldError.TOO_LARGE; null } else c.microlitres
                Volume.Conversion.NotPositive -> { e[f] = BenchFieldError.NOT_POSITIVE; null }
                Volume.Conversion.TooPrecise -> { e[f] = BenchFieldError.TOO_PRECISE; null }
                Volume.Conversion.NotFinite, Volume.Conversion.OutOfRange -> { e[f] = BenchFieldError.TOO_LARGE; null }
            }
        }
        num(BenchField.FLOW, form.flowUlPerS)
        val dose = ml(BenchField.MAX_DOSE, form.maxDoseMl)
        val daily = ml(BenchField.MAX_DAILY, form.maxDailyMl)
        val runtime = num(BenchField.MAX_RUNTIME, form.maxRuntimeMs)
        num(BenchField.MIN_INTERVAL, form.minIntervalS)
        if (runtime != null) {
            if (hwMaxRuntimeMs <= 0) e[BenchField.MAX_RUNTIME] = BenchFieldError.NO_HW_MAX
            else if (runtime > hwMaxRuntimeMs) e[BenchField.MAX_RUNTIME] = BenchFieldError.ABOVE_HW_MAX
        }
        if (dose != null && daily != null && daily < dose) e[BenchField.MAX_DAILY] = BenchFieldError.DAILY_BELOW_DOSE
        return e
    }

    /**
     * Writes flow, limits and interval (and calibrated_at = now only when [BenchPumpForm.calibrateNow]).
     * Refused locally while READY (the firmware would refuse) and for invalid input; then re-reads.
     */
    suspend fun save(snapshot: BenchSnapshot, form: BenchPumpForm): BenchOutcome {
        val errors = validate(form, snapshot.config.hwMaxRuntimeMs)
        if (errors.isNotEmpty()) return BenchOutcome.Invalid(errors)
        if (snapshot.status.readiness != Readiness.SERVICE_REQUIRED) return BenchOutcome.MustBeNotReady
        var mask = ConfigRecord.F_FLOW or ConfigRecord.F_MAX_DOSE or ConfigRecord.F_MAX_DAILY or
            ConfigRecord.F_MAX_RUNTIME or ConfigRecord.F_MIN_INTERVAL
        if (form.calibrateNow) mask = mask or ConfigRecord.F_CALIBRATED_AT
        val values = snapshot.config.copy(
            pumpFlowUlPerS = form.flowUlPerS.trim().toLong(),
            maxDoseUl = microlitres(form.maxDoseMl),
            maxDailyUl = microlitres(form.maxDailyMl),
            maxRuntimeMs = form.maxRuntimeMs.trim().toLong(),
            minIntervalS = form.minIntervalS.trim().toLong(),
            calibratedAt = if (form.calibrateNow) nowUtc() else snapshot.config.calibratedAt,
        )
        return when (val r = repo.setConfig(snapshot.config.revision, mask, values)) {
            is OpResult.Ok -> when (val l = load()) {
                // schedules the device keeps but now refuses at execution (amount > max dose): warn
                is BenchOutcome.Loaded -> l.copy(schedulesAboveMaxDose = repo.schedules().first().valueOrNull
                    ?.schedules?.count { it.record.amountUl > values.maxDoseUl } ?: 0)
                else -> l
            }
            else -> failure(r)
        }
    }

    private fun microlitres(ml: String): Long = (Volume.mlToMicrolitres(ml.trim().replace(',', '.').toDouble()) as Volume.Conversion.Ok).microlitres

    /** READY / NOT READY, only on the user's explicit request; a refusal reports the closed gates. */
    suspend fun setReady(ready: Boolean): BenchOutcome =
        when (val r = repo.setReadiness(if (ready) Readiness.READY else Readiness.SERVICE_REQUIRED)) {
            is OpResult.Ok -> load()
            is OpResult.Rejected ->
                if (ready && r.error.code == DeviceErrorCode.REFUSED) BenchOutcome.ReadyRefused(BenchGate.of(r.error.detail), BenchPumpProblem.of(r.error.aux))
                else BenchOutcome.Rejected(r.error.code.wire, r.error.detail)
            else -> failure(r)
        }

    private fun failure(r: OpResult<*>): BenchOutcome = when (r) {
        is OpResult.Ok -> error("not a failure")
        is OpResult.Uncertain -> BenchOutcome.Uncertain
        is OpResult.NotSent -> BenchOutcome.NotConnected
        is OpResult.Rejected -> BenchOutcome.Rejected(r.error.code.wire, r.error.detail)
        is OpResult.Refused -> when (r.reason) {
            is LocalRefusal.Unresolved, LocalRefusal.InProgress -> BenchOutcome.AwaitingVerification
            else -> BenchOutcome.NotConnected
        }
    }

}
