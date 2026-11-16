#ifndef DSP_POWER_H
#define DSP_POWER_H

/*
 * Power Service (milestone M1.9b): pure domain code. Owns the sleep plan, the RTC-retained
 * block (versioned, CRC-checked) and the shutdown checklist definitions; the platform
 * implements dsp_power_port_t (deep sleep, timer and button wake, RTC memory, fail-safe).
 * No ESP-IDF here, and no real deep sleep in this milestone (M1.9c adapters).
 *
 * Approved rules (M1.9 design, Q6, Q13, D1-B):
 *  - The plan is the earliest of: the next dose (only while dose wakes are allowed, see
 *    dsp_dosing_dose_wakes_allowed), the next time sync, and the re-evaluation cap
 *    (configurable, default 6 h). Earlier deadlines win.
 *  - Never a wake-up for a notification: pending notifications travel in a session on an
 *    existing wake (dsp_connectivity.h); the plan has no notification input.
 *  - Sleep is entered only after the shutdown checklist passed (pump OFF confirmed, HTTPS
 *    ended, Wi-Fi DISABLED, BLE stopped). If it cannot be confirmed, the fail-safe port is
 *    called instead of sleep: never silently to sleep with an active pump or radio.
 *  - The retained block survives deep sleep and soft resets; it is discarded after a
 *    power-on or brownout reset and whenever its magic, version, length, CRC or field ranges
 *    do not validate (then: defaults, i.e. one sync attempt allowed, no backoff).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_connectivity.h"
#include "dsp_timekeeping.h"
#include "dsp_wakeup.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_POWER_MAX_SLEEP_DEFAULT_S ((int64_t)6 * 3600) /* approved Q6 */
#define DSP_POWER_MAX_SLEEP_MIN_S ((int64_t)60)
#define DSP_POWER_MAX_SLEEP_MAX_S ((int64_t)24 * 3600)
/* A sync that is due but could not run in this wake (e.g. a dose was too close) is
 * re-planned this far ahead instead of waking at once (no busy wake loop). */
#define DSP_POWER_SYNC_DEFER_S ((int64_t)300)
#define DSP_POWER_MIN_SLEEP_S ((int64_t)1)

/* ---- Wake cause (platform reset reason + deep-sleep wake source) ---------------------- */

typedef enum {
    DSP_CAUSE_POWER_ON = 0, /* also the fallback for unknown causes */
    DSP_CAUSE_TIMER,        /* deep-sleep timer */
    DSP_CAUSE_BUTTON,       /* deep-sleep GPIO wake: service mode (BLE) */
    DSP_CAUSE_SOFTWARE,     /* software reset (incl. fail-safe reset) */
    DSP_CAUSE_WATCHDOG,
    DSP_CAUSE_PANIC,
    DSP_CAUSE_BROWNOUT,
} dsp_wake_cause_t;

/* RTC memory content is undefined after these resets: the retained block is discarded. */
bool dsp_power_cause_keeps_retained(dsp_wake_cause_t cause);

/* ---- RTC-retained block ------------------------------------------------------------- */

#define DSP_POWER_RETAINED_VERSION 1
#define DSP_POWER_RETAINED_BYTES 48
#define DSP_POWER_FAILSAFE_LIMIT 3 /* consecutive fail-safe resets before the inhibit */

typedef struct {
    dsp_time_retained_t time;          /* M1.4 sync retry/backoff state */
    dsp_connectivity_retained_t conn;  /* M1.7 hourly CRITICAL attempt limit */
    uint32_t wake_count;               /* wakes since the block was (re)initialized */
    uint8_t failsafe_count;            /* consecutive unconfirmed shutdowns (fail-safe) */
    uint8_t last_reason;               /* dsp_wake_reason_t of the last plan */
    dsp_utc_t last_wake_at;            /* raw time the last plan woke for (diagnostics) */
} dsp_power_retained_t;

void dsp_power_retained_defaults(dsp_power_retained_t *r);
bool dsp_power_retained_is_valid(const dsp_power_retained_t *r);

/* Fixed little-endian image: magic "DSPR" | version | length | 2 zero | fields | CRC-32
 * (IEEE) over everything before it. encode: false on invalid input. decode: false (and
 * *out untouched) unless magic, version, length, reserved bytes, CRC and ranges validate. */
bool dsp_power_retained_encode(const dsp_power_retained_t *r, uint8_t out[DSP_POWER_RETAINED_BYTES]);
bool dsp_power_retained_decode(const uint8_t *buf, size_t len, dsp_power_retained_t *out);

uint32_t dsp_power_crc32(const uint8_t *data, size_t len);

/* ---- Platform port -------------------------------------------------------------------- */

/* Shutdown checklist items (bits of the failures reported to fail_safe). */
enum {
    DSP_POWER_CHK_PUMP = 1u << 0,  /* pump OFF not confirmed */
    DSP_POWER_CHK_HTTPS = 1u << 1, /* an HTTPS session is still open */
    DSP_POWER_CHK_WIFI = 1u << 2,  /* Wi-Fi not DISABLED */
    DSP_POWER_CHK_BLE = 1u << 3,   /* BLE not confirmed stopped */
};

typedef struct {
    void *ctx;
    dsp_wake_cause_t (*wake_cause)(void *ctx);
    /* Raw RTC block: copies at most cap bytes, returns the stored length (0 = none). */
    size_t (*load_retained)(void *ctx, uint8_t *buf, size_t cap);
    bool (*save_retained)(void *ctx, const uint8_t *buf, size_t len);
    /* Arms the timer (sleep_s > 0) and the button wake, enters deep sleep. Does not return on
     * hardware; host fakes return. Called only after the checklist passed. */
    void (*sleep)(void *ctx, int64_t sleep_s);
    /* Called INSTEAD of sleep when the checklist cannot be confirmed (failures = CHK bits).
     * Must not return on hardware: drive every actuator off by the strongest means available
     * and reset the SoC (the pump gate falls OFF through its hardware pull-down, P1). */
    void (*fail_safe)(void *ctx, uint32_t failures);
} dsp_power_port_t;

/* ---- Plan ------------------------------------------------------------------------------ */

typedef struct {
    bool dose_known; /* from dsp_dosing_next_due (already empty while dose wakes are off) */
    dsp_utc_t dose_at;
    bool sync_known;
    dsp_utc_t sync_at; /* dsp_timekeeper_next_sync (raw time) */
    int64_t max_sleep_s; /* 0 = DSP_POWER_MAX_SLEEP_DEFAULT_S; clamped to MIN..MAX */
} dsp_power_inputs_t;

int64_t dsp_power_max_sleep_s(int64_t configured);

/* dsp_wake_plan over the inputs, with an overdue sync (<= now) deferred by
 * DSP_POWER_SYNC_DEFER_S. A dose at or before now gives sleep_s = 0 (act now). */
dsp_wake_plan_t dsp_power_plan(const dsp_power_inputs_t *in, dsp_utc_t now);

#ifdef __cplusplus
}
#endif

#endif /* DSP_POWER_H */
