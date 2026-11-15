package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.DoseOccurrence
import cz.nowass.dispenser.domain.RecurrenceEngine
import cz.nowass.dispenser.domain.Schedule
import cz.nowass.dispenser.domain.Volume
import java.time.Duration
import java.time.Instant

enum class PredictionStatus {
    /** Stock runs out within the horizon: [ReservoirPrediction.firstUncovered] is known. */
    DEPLETES_WITHIN_HORIZON,
    /** The very next dose cannot be covered. */
    INSUFFICIENT_FOR_NEXT_DOSE,
    /** Dispensing is blocked until a refill (D2); no dose will run. */
    BLOCKED_UNTIL_REFILL,
    /** No enabled schedule has any future dose. */
    NO_FUTURE_DOSES,
    /** Every remaining dose of finite schedules (end dates / one-offs) is covered. */
    ALL_REMAINING_DOSES_COVERED,
    /** Stock lasts beyond the horizon, or too many occurrences to evaluate: shown as unknown. */
    UNKNOWN_BEYOND_HORIZON,
}

/**
 * Result of [ReservoirPredictor.predict]. Values are exact for the given schedules and
 * estimate; the depletion moment should be presented with day precision only.
 */
data class ReservoirPrediction(
    val status: PredictionStatus,
    /** Complete future doses (in order) the current estimate can cover within the horizon. */
    val coveredDoses: Int,
    /** First scheduled dose that cannot be covered, if found within the horizon. */
    val firstUncovered: DoseOccurrence?,
    /** = time of [firstUncovered]: when dosing would be blocked (refill needed before it). */
    val depletionAt: Instant?,
    /** Estimated remaining volume after the last covered dose (µl). */
    val remainingAfterLastCoveredUl: Long,
    val horizonEnd: Instant,
)

/**
 * Predicts reservoir coverage by walking the actual future occurrences of all enabled
 * schedules in order (not an average-consumption division), within a bounded horizon.
 */
class ReservoirPredictor(
    val horizon: Duration = Duration.ofDays(365),
    val maxOccurrences: Int = 10_000,
) {
    fun predict(reservoir: ReservoirState, now: Instant, schedules: List<Schedule>): ReservoirPrediction {
        val horizonEnd = now.plus(horizon)
        val enabled = schedules.filter { it.enabled }

        fun result(status: PredictionStatus, covered: Int = 0, uncovered: DoseOccurrence? = null, remaining: Long = reservoir.remainingUl) =
            ReservoirPrediction(status, covered, uncovered, uncovered?.scheduledAt, remaining, horizonEnd)

        val anyFuture = enabled.any { RecurrenceEngine.nextOccurrence(it, now) != null }
        if (!anyFuture) return result(PredictionStatus.NO_FUTURE_DOSES)
        if (reservoir.blocked) {
            val next = enabled.mapNotNull { RecurrenceEngine.nextOccurrence(it, now) }.minWithOrNull(ORDER)
            return result(PredictionStatus.BLOCKED_UNTIL_REFILL, uncovered = next)
        }

        val occurrences = ArrayList<DoseOccurrence>()
        for (s in enabled) {
            val occ = RecurrenceEngine.occurrencesBetween(s, now, horizonEnd, maxOccurrences - occurrences.size)
                ?: return result(PredictionStatus.UNKNOWN_BEYOND_HORIZON)
            occurrences += occ
        }
        occurrences.sortWith(ORDER)

        var remaining = reservoir.remainingUl
        var covered = 0
        for (occ in occurrences) {
            val requested = (Volume.mlToMicrolitres(occ.amountMl) as Volume.Conversion.Ok).microlitres
            if (requested > remaining) {
                val status = if (covered == 0) PredictionStatus.INSUFFICIENT_FOR_NEXT_DOSE else PredictionStatus.DEPLETES_WITHIN_HORIZON
                return result(status, covered, occ, remaining)
            }
            remaining -= requested
            covered++
        }
        // All doses within the horizon are covered: finite schedules may be fully covered,
        // otherwise the outcome lies beyond the horizon and is reported as unknown.
        val moreAfterHorizon = enabled.any { RecurrenceEngine.nextOccurrence(it, horizonEnd) != null }
        return result(
            if (moreAfterHorizon) PredictionStatus.UNKNOWN_BEYOND_HORIZON else PredictionStatus.ALL_REMAINING_DOSES_COVERED,
            covered,
            remaining = remaining,
        )
    }

    private companion object {
        val ORDER = compareBy<DoseOccurrence>({ it.scheduledAt }, { it.scheduleId })
    }
}
