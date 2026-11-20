package cz.nowass.dispenser.ui.schedule

import androidx.annotation.StringRes
import cz.nowass.dispenser.R
import cz.nowass.dispenser.data.ScheduleWriteResult

/**
 * DEVICE-mode schedule write outcomes that are neither success nor the shared conflict/not-found
 * cases. [UNCERTAIN] is not a failure: the change may have been applied and is verified at the next
 * connection, so no retry is offered for it.
 */
enum class ScheduleWriteNotice(@param:StringRes val text: Int) {
    NOT_SENT(R.string.schedule_write_not_sent),
    UNCERTAIN(R.string.schedule_write_uncertain),
    AWAITING_VERIFICATION(R.string.schedule_write_awaiting_verification),
    REJECTED(R.string.schedule_write_rejected),
    DEVICE_LIMIT(R.string.schedule_write_device_limit),
    ABOVE_MAX_DOSE(R.string.schedule_write_above_max_dose),
    ;

    companion object {
        /** Null for the results every mode shares (success, invalid, conflict, not found). */
        fun of(result: ScheduleWriteResult): ScheduleWriteNotice? = when (result) {
            ScheduleWriteResult.NotSent -> NOT_SENT
            ScheduleWriteResult.Uncertain -> UNCERTAIN
            ScheduleWriteResult.AwaitingVerification -> AWAITING_VERIFICATION
            is ScheduleWriteResult.DeviceRejected -> REJECTED
            is ScheduleWriteResult.DeviceLimit -> DEVICE_LIMIT
            is ScheduleWriteResult.AboveMaxDose -> ABOVE_MAX_DOSE
            is ScheduleWriteResult.Success, is ScheduleWriteResult.Invalid, ScheduleWriteResult.Conflict, ScheduleWriteResult.NotFound -> null
        }
    }
}
