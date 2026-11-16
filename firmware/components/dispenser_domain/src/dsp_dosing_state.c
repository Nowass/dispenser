#include "dsp_dosing_state.h"

#include <stddef.h>
#include <string.h>

void dsp_dosing_state_defaults(dsp_dosing_state_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof *s);
    s->readiness = DSP_READINESS_SERVICE_REQUIRED; /* approved: never READY by default */
}

static bool date_zero(dsp_date_t d)
{
    return d.year == 0 && d.month == 0 && d.day == 0;
}

static bool instant_ok(dsp_utc_t t)
{
    return t == 0 || (t >= DSP_UTC_MIN && t <= DSP_UTC_MAX);
}

bool dsp_dosing_state_is_valid(const dsp_dosing_state_t *s)
{
    if (s == NULL ||
        (s->readiness != DSP_READINESS_READY && s->readiness != DSP_READINESS_SERVICE_REQUIRED) ||
        (s->flags & ~DSP_DOSING_F_ALL) != 0) {
        return false;
    }
    if (s->flags & DSP_DOSING_F_REFILL_PENDING) {
        if (s->refill_capacity_ul == 0 || s->refill_remaining_ul == 0 ||
            s->refill_remaining_ul > s->refill_capacity_ul || s->refill_seq == 0) {
            return false;
        }
    } else if (s->refill_capacity_ul != 0 || s->refill_remaining_ul != 0 || s->refill_seq != 0) {
        return false;
    }
    if (s->flags & DSP_DOSING_F_MANUAL_ACTIVE) {
        if (s->manual_volume_ul == 0 || s->manual_seq == 0 || s->manual_started_at == 0 ||
            !instant_ok(s->manual_started_at)) {
            return false;
        }
    } else if (s->manual_volume_ul != 0 || s->manual_started_at != 0 || s->manual_seq != 0) {
        return false;
    }
    if (date_zero(s->ledger_date)) {
        if (s->ledger_sum_ul != 0) {
            return false;
        }
    } else if (!dsp_date_is_valid(s->ledger_date)) {
        return false;
    }
    if (!instant_ok(s->last_start_utc)) {
        return false;
    }
    /* Sequence numbers in use are below next_seq. */
    if ((s->refill_seq != 0 && s->refill_seq >= s->next_seq) ||
        (s->manual_seq != 0 && s->manual_seq >= s->next_seq)) {
        return false;
    }
    return true;
}

bool dsp_dosing_state_is_provisioning(const dsp_dosing_state_t *s)
{
    if (s == NULL) {
        return false;
    }
    dsp_dosing_state_t d;
    dsp_dosing_state_defaults(&d);
    d.flags = (uint8_t)(s->flags & DSP_DOSING_F_BLOCKED);
    return dsp_dosing_state_equal(s, &d);
}

bool dsp_dosing_state_equal(const dsp_dosing_state_t *a, const dsp_dosing_state_t *b)
{
    return a->readiness == b->readiness && a->flags == b->flags &&
           a->refill_capacity_ul == b->refill_capacity_ul &&
           a->refill_remaining_ul == b->refill_remaining_ul && a->refill_seq == b->refill_seq &&
           a->manual_volume_ul == b->manual_volume_ul &&
           a->manual_started_at == b->manual_started_at && a->manual_seq == b->manual_seq &&
           dsp_date_compare(a->ledger_date, b->ledger_date) == 0 &&
           a->ledger_sum_ul == b->ledger_sum_ul && a->last_start_utc == b->last_start_utc &&
           a->next_seq == b->next_seq;
}
