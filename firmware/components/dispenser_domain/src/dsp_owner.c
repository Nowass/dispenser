#include "dsp_owner.h"

#include <string.h>

#include "dsp_auth.h" /* dsp_secure_zero */

void dsp_owner_invite_clear(dsp_power_retained_t *r)
{
    if (r != NULL) {
        dsp_secure_zero(&r->invite, sizeof r->invite);
    }
}

void dsp_owner_arm_clear(dsp_power_retained_t *r)
{
    if (r != NULL) {
        memset(&r->arm, 0, sizeof r->arm);
    }
}

void dsp_owner_on_wake(dsp_power_retained_t *r, dsp_wake_cause_t cause, bool block_valid,
                       dsp_utc_t now_raw)
{
    if (r == NULL) {
        return;
    }
    bool deep_sleep = cause == DSP_CAUSE_TIMER || cause == DSP_CAUSE_BUTTON;
    if (!block_valid || !deep_sleep || !dsp_power_overlays_consistent(r) || now_raw < r->saved_raw) {
        dsp_owner_invite_clear(r);
        dsp_owner_arm_clear(r);
        return;
    }
    if (r->invite.valid && (now_raw < r->invite.created_raw || now_raw >= r->invite.deadline_raw)) {
        dsp_owner_invite_clear(r);
    }
    if (r->arm.armed && (now_raw < r->arm.armed_raw || now_raw >= r->arm.deadline_raw)) {
        dsp_owner_arm_clear(r);
    }
}

bool dsp_owner_invite_valid(const dsp_power_retained_t *r, dsp_utc_t now_raw, uint32_t epoch)
{
    return r != NULL && r->invite.valid == 1 && dsp_power_overlays_consistent(r) &&
           epoch != DSP_OWNER_EPOCH_DEGRADED && r->invite.owner_epoch == epoch &&
           now_raw >= r->invite.created_raw && now_raw < r->invite.deadline_raw;
}

bool dsp_owner_arm_valid(const dsp_power_retained_t *r, dsp_utc_t now_raw, uint32_t epoch)
{
    return r != NULL && r->arm.armed == 1 && dsp_power_overlays_consistent(r) &&
           r->arm.owner_epoch == epoch && now_raw >= r->arm.armed_raw &&
           now_raw < r->arm.deadline_raw;
}

void dsp_owner_invite_set(dsp_power_retained_t *r, uint8_t creator, uint32_t epoch,
                          const uint8_t token[16], dsp_utc_t now_raw)
{
    if (r == NULL || token == NULL) {
        return;
    }
    dsp_owner_invite_clear(r);
    r->invite.valid = 1;
    r->invite.creator_slot = creator;
    r->invite.owner_epoch = epoch;
    memcpy(r->invite.token, token, sizeof r->invite.token);
    r->invite.created_raw = now_raw;
    r->invite.deadline_raw = now_raw + DSP_OWNER_INVITE_S;
}

void dsp_owner_arm_set(dsp_power_retained_t *r, uint32_t epoch, dsp_utc_t now_raw)
{
    if (r == NULL) {
        return;
    }
    dsp_owner_arm_clear(r);
    r->arm.armed = 1;
    r->arm.owner_epoch = epoch;
    r->arm.armed_raw = now_raw;
    r->arm.deadline_raw = now_raw + DSP_OWNER_ARM_S;
}

void dsp_owner_arm_abort(dsp_power_retained_t *r)
{
    if (r == NULL || !r->arm.armed) {
        return;
    }
    if (r->arm.aborts + 1u >= DSP_OWNER_ARM_MAX_ABORTS) {
        dsp_owner_arm_clear(r);
    } else {
        r->arm.aborts++;
    }
}

/* ---- Confirmation ------------------------------------------------------------------------ */

void dsp_confirm_reset(dsp_confirm_t *c)
{
    if (c != NULL) {
        dsp_secure_zero(c, sizeof *c);
    }
}

void dsp_confirm_begin(dsp_confirm_t *c, uint32_t session_id, const uint8_t transcript[32],
                       uint64_t now_ms)
{
    if (c == NULL || transcript == NULL) {
        return;
    }
    dsp_confirm_reset(c);
    c->state = DSP_CONFIRM_PENDING;
    c->session_id = session_id;
    memcpy(c->transcript, transcript, sizeof c->transcript);
    c->start_ms = now_ms;
    c->deadline_ms = now_ms + DSP_CONFIRM_WINDOW_MS;
}

static void abort_confirm(dsp_confirm_t *c)
{
    c->state = DSP_CONFIRM_ABORTED;
}

void dsp_confirm_event(dsp_confirm_t *c, const dsp_button_event_t *ev)
{
    if (c == NULL || ev == NULL || c->state != DSP_CONFIRM_PENDING) {
        return;
    }
    if (ev->press_ms < c->start_ms + DSP_CONFIRM_GUARD_MS) {
        return; /* before the operation (queued, held at entry, replayed): never counts */
    }
    if (ev->release_ms < ev->press_ms || ev->release_ms > c->deadline_ms ||
        ev->kind != DSP_BUTTON_SHORT || ev->release_ms - ev->press_ms > DSP_CONFIRM_SHORT_MAX_MS) {
        abort_confirm(c);
        return;
    }
    if (c->presses > 0 && (ev->press_ms < c->last_release_ms ||
                           ev->press_ms - c->last_release_ms > DSP_CONFIRM_GAP_MAX_MS)) {
        abort_confirm(c); /* out of order, overlapping or a new sequence */
        return;
    }
    c->presses++;
    c->last_release_ms = ev->release_ms;
    if (c->presses > DSP_CONFIRM_PRESSES) {
        abort_confirm(c);
    }
}

dsp_confirm_state_t dsp_confirm_poll(dsp_confirm_t *c, uint64_t now_ms)
{
    if (c == NULL) {
        return DSP_CONFIRM_NONE;
    }
    if (c->state != DSP_CONFIRM_PENDING) {
        return (dsp_confirm_state_t)c->state;
    }
    if (now_ms < c->start_ms) {
        abort_confirm(c); /* monotonic clock went back */
    } else if (c->presses == DSP_CONFIRM_PRESSES &&
               now_ms >= c->last_release_ms + DSP_CONFIRM_SEQ_END_MS) {
        c->state = DSP_CONFIRM_CONFIRMED;
    } else if (c->presses < DSP_CONFIRM_PRESSES && now_ms >= c->deadline_ms) {
        abort_confirm(c);
    } else if (now_ms >= c->deadline_ms + DSP_CONFIRM_SEQ_END_MS) {
        abort_confirm(c);
    }
    return (dsp_confirm_state_t)c->state;
}
