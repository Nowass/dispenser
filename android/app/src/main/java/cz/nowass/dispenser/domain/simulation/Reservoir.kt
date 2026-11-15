package cz.nowass.dispenser.domain.simulation

import cz.nowass.dispenser.domain.Volume
import java.time.Instant

/**
 * Low-stock warning threshold (REQUIREMENTS_DRAFT FR-021, value PROPOSED).
 * "Low" means strictly below [thresholdPercent] of capacity; integer arithmetic only.
 */
data class LowStockPolicy(val thresholdPercent: Int = DEFAULT_THRESHOLD_PERCENT) {
    init {
        require(thresholdPercent in 0..100) { "thresholdPercent must be within 0..100" }
    }

    fun isLow(remainingUl: Long, capacityUl: Long): Boolean =
        // remaining / capacity < threshold / 100, without floating point (no overflow: ≤ 2^53 · 100).
        remainingUl * 100 < capacityUl * thresholdPercent

    companion object {
        /** PROPOSED default (FR-021); configurable. */
        const val DEFAULT_THRESHOLD_PERCENT = 20
    }
}

enum class ReservoirStatus { NORMAL, LOW, BLOCKED }

/**
 * Estimated reservoir state in integer microlitres. Modelled from commanded doses and
 * refills, never measured by a sensor (FR-006, FR-020).
 *
 * Invariants (enforced): 0 < capacity ≤ [Volume.MAX_MICROLITRES]; 0 ≤ remaining ≤ capacity.
 *
 * @property blocked set when a dose could not be covered; stays set until a refill (FR-022, D2).
 * @property lowWarningLatched the low-stock warning was already issued in this episode (FR-007, D5).
 */
data class ReservoirState(
    val capacityUl: Long,
    val remainingUl: Long,
    val updatedAt: Instant,
    val blocked: Boolean = false,
    val lowWarningLatched: Boolean = false,
) {
    init {
        require(capacityUl in 1..Volume.MAX_MICROLITRES) { "capacity must be > 0 and within the storage range" }
        require(remainingUl in 0..capacityUl) { "remaining must be within 0..capacity" }
    }

    /** A dose runs only if not blocked and the estimate covers its full volume (no partial doses). */
    fun canDispense(requestedUl: Long): Boolean = !blocked && requestedUl in 1..remainingUl

    fun status(policy: LowStockPolicy): ReservoirStatus = when {
        blocked -> ReservoirStatus.BLOCKED
        policy.isLow(remainingUl, capacityUl) -> ReservoirStatus.LOW
        else -> ReservoirStatus.NORMAL
    }
}

/**
 * Operational readiness of the (simulated or future physical) device.
 *
 * [SERVICE_REQUIRED] is reserved for the future container-replacement / tubing-priming
 * service workflow (REQUIREMENTS_DRAFT FR-025): normal dispensing is suspended until the
 * service is completed. The A1.3 simulation never enters it on its own.
 */
enum class DeviceReadiness { READY, SERVICE_REQUIRED }
