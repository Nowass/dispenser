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
    /* Format 2: anti-replay state. */
    for (size_t i = 0; i < DSP_MANUAL_SLOTS; i++) {
        if (s->manual_hw[i] > DSP_MANUAL_SEQ_MAX) {
            return false;
        }
    }
    if (s->marker_req != 0) {
        /* bound to an active marker; the request was decided (hw raised) in the same write */
        if (!(s->flags & DSP_DOSING_F_MANUAL_ACTIVE) || s->marker_slot >= DSP_MANUAL_SLOTS ||
            s->marker_req > s->manual_hw[s->marker_slot]) {
            return false;
        }
    } else if (s->marker_slot != 0) {
        return false;
    }
    if (s->ring_next >= DSP_MANUAL_RING) {
        return false;
    }
    for (size_t i = 0; i < DSP_MANUAL_RING; i++) {
        const dsp_manual_result_t *e = &s->ring[i];
        if (e->seq == 0) {
            if (e->slot != 0 || e->state != 0 || e->detail != 0) {
                return false;
            }
            continue;
        }
        if (e->slot >= DSP_MANUAL_SLOTS || e->state < DSP_MRES_PENDING ||
            e->state > DSP_MRES_REFUSED || e->seq > s->manual_hw[e->slot]) {
            return false;
        }
        /* PENDING exists only for the request of the active marker */
        if (e->state == DSP_MRES_PENDING &&
            (s->marker_req != e->seq || s->marker_slot != e->slot)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (s->ring[j].seq == e->seq && s->ring[j].slot == e->slot) {
                return false; /* one entry per request */
            }
        }
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
    if (!date_zero(s->adm_date) && !dsp_date_is_valid(s->adm_date)) {
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

static bool rings_equal(const dsp_dosing_state_t *a, const dsp_dosing_state_t *b)
{
    for (size_t i = 0; i < DSP_MANUAL_RING; i++) {
        const dsp_manual_result_t *x = &a->ring[i];
        const dsp_manual_result_t *y = &b->ring[i];
        if (x->slot != y->slot || x->state != y->state || x->seq != y->seq ||
            x->detail != y->detail) {
            return false;
        }
    }
    return true;
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
           a->next_seq == b->next_seq && a->manual_hw[0] == b->manual_hw[0] &&
           a->manual_hw[1] == b->manual_hw[1] && a->manual_hw[2] == b->manual_hw[2] &&
           a->manual_hw[3] == b->manual_hw[3] && a->marker_slot == b->marker_slot &&
           a->marker_req == b->marker_req && a->ring_next == b->ring_next && rings_equal(a, b) &&
           dsp_date_compare(a->adm_date, b->adm_date) == 0 && a->adm_count == b->adm_count;
}

const dsp_manual_result_t *dsp_dosing_state_find_result(const dsp_dosing_state_t *s, uint8_t slot,
                                                        uint64_t seq)
{
    if (s == NULL || seq == 0) {
        return NULL;
    }
    for (size_t i = 0; i < DSP_MANUAL_RING; i++) {
        if (s->ring[i].seq == seq && s->ring[i].slot == slot) {
            return &s->ring[i];
        }
    }
    return NULL;
}
