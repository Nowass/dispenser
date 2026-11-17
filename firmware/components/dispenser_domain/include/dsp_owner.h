#ifndef DSP_OWNER_H
#define DSP_OWNER_H

/*
 * Ownership overlay rules and the recovery confirmation (milestone M1.10b.3). Pure domain code.
 * docs/OWNERSHIP_DESIGN.md sections 4 and 5.7 (approved, R1/R5).
 *
 * Overlays (invite, recovery arm) in the RTC-retained block are PRECONDITIONS, never authority.
 * They are valid only while ALL of these hold:
 *  1. reset reason: kept only across deep-sleep wakes (TIMER, BUTTON); every reset (power-on,
 *     brownout, software incl. the fail-safe reset, watchdog, panic) clears them (R1);
 *  2. integrity: CRC and structural consistency (dsp_power_overlays_consistent);
 *  3. clock continuity: the raw clock never went back below the last save or the creation time,
 *     and no clock step was applied (the Application Service clears them on a step);
 *  4. expiry: now_raw < deadline_raw (fixed at creation, never extended);
 *  5. epoch binding: the overlay's epoch equals the durable owner_epoch (arm taken while the
 *     credentials were damaged: DSP_OWNER_EPOCH_DEGRADED, valid only while still damaged).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_power.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_OWNER_ARM_HOLD_MS 10000u /* verified continuous hold from the first sample after boot */
#define DSP_OWNER_ARM_MAX_ABORTS 3   /* the third failed confirmation disarms */

/* Rules 1, 2 (block_valid: decoded with a valid CRC), 3 (raw clock vs. saved_raw) and 4 at the
 * start of a wake. Clears whatever does not hold. */
void dsp_owner_on_wake(dsp_power_retained_t *r, dsp_wake_cause_t cause, bool block_valid,
                       dsp_utc_t now_raw);

/* Rules 3 (creation time), 4 and 5 for a use now. epoch: the durable owner_epoch, or
 * DSP_OWNER_EPOCH_DEGRADED while the credentials are damaged. */
bool dsp_owner_invite_valid(const dsp_power_retained_t *r, dsp_utc_t now_raw, uint32_t epoch);
bool dsp_owner_arm_valid(const dsp_power_retained_t *r, dsp_utc_t now_raw, uint32_t epoch);

void dsp_owner_invite_set(dsp_power_retained_t *r, uint8_t creator, uint32_t epoch,
                          const uint8_t token[16], dsp_utc_t now_raw);
void dsp_owner_invite_clear(dsp_power_retained_t *r);
void dsp_owner_arm_set(dsp_power_retained_t *r, uint32_t epoch, dsp_utc_t now_raw);
void dsp_owner_arm_clear(dsp_power_retained_t *r);
/* A failed confirmation: aborts + 1; the DSP_OWNER_ARM_MAX_ABORTS-th disarms. */
void dsp_owner_arm_abort(dsp_power_retained_t *r);

/* ---- Physical confirmation (exactly 3 short presses), bound to one recovery operation ------
 *
 * Timings (ARCHITECTURE, PROPOSED until hardware tests): presses count only with their
 * debounced press edge at or after start + DSP_CONFIRM_GUARD_MS (a press held at entry, queued
 * events and replays of older events are ignored); each press <= DSP_CONFIRM_SHORT_MAX_MS;
 * gap release -> next press <= DSP_CONFIRM_GAP_MAX_MS; the three presses before the deadline
 * (start + DSP_CONFIRM_WINDOW_MS); the sequence ends after DSP_CONFIRM_SEQ_END_MS of silence.
 * A long press, a fourth press, a too long gap or press, an event out of order or the deadline
 * aborts. The result is single-use and bound to (session_id, transcript hash). */
#define DSP_CONFIRM_WINDOW_MS 20000u
#define DSP_CONFIRM_GUARD_MS 300u
#define DSP_CONFIRM_SHORT_MAX_MS 400u
#define DSP_CONFIRM_GAP_MAX_MS 500u
#define DSP_CONFIRM_SEQ_END_MS 700u
#define DSP_CONFIRM_PRESSES 3u

typedef enum {
    DSP_CONFIRM_NONE = 0,
    DSP_CONFIRM_PENDING,
    DSP_CONFIRM_CONFIRMED,
    DSP_CONFIRM_ABORTED,
} dsp_confirm_state_t;

typedef enum {
    DSP_BUTTON_SHORT = 1,
    DSP_BUTTON_LONG = 2,
} dsp_button_kind_t;

/* A debounced button event from the button driver (M10), monotonic milliseconds. */
typedef struct {
    uint8_t kind;        /* dsp_button_kind_t */
    uint64_t press_ms;   /* debounced press edge */
    uint64_t release_ms; /* debounced release edge (>= press_ms) */
} dsp_button_event_t;

typedef struct {
    uint8_t state; /* dsp_confirm_state_t */
    uint32_t session_id;
    uint8_t transcript[32];
    uint64_t start_ms;
    uint64_t deadline_ms;
    uint8_t presses;
    uint64_t last_release_ms;
} dsp_confirm_t;

void dsp_confirm_begin(dsp_confirm_t *c, uint32_t session_id, const uint8_t transcript[32],
                       uint64_t now_ms);
void dsp_confirm_event(dsp_confirm_t *c, const dsp_button_event_t *ev);
/* Evaluates the sequence end and the deadline; returns the state. */
dsp_confirm_state_t dsp_confirm_poll(dsp_confirm_t *c, uint64_t now_ms);
void dsp_confirm_reset(dsp_confirm_t *c);

#ifdef __cplusplus
}
#endif

#endif /* DSP_OWNER_H */
