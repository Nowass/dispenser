#ifndef DSP_PRIME_H
#define DSP_PRIME_H

/*
 * Pump priming (firmware 0.3.3, docs/BLE_PROTOCOL.md 7.9, 10.9): owner-started START/STOP of the
 * pump to fill the tubing, outside dose accounting. Pure decision rules; the Application Service
 * owns the state and the Dosing Service the pump.
 *
 *  - Runs only inside an authenticated BLE session, bound to it by a lease: every PRIME_START /
 *    PRIME_STATUS from the session renews it for DSP_PRIME_LEASE_MS. It stops at PRIME_STOP, the
 *    lease expiry, the per-activation maximum, a disconnect, the end of the security session or
 *    the service window, a pump fault, a due dose and before shutdown. Never restarted
 *    automatically (state in RAM only; a timeout needs another explicit PRIME_START).
 *  - The driver's independent cutoff (timer interrupt) is armed at start and re-armed with each
 *    renewal to the earlier of lease expiry and maximum plus DSP_PRIME_CUTOFF_MARGIN_MS, so a
 *    stalled task cannot keep the pump on.
 *  - No volume is assumed: nothing is claimed, logged as a dose or subtracted from the reservoir.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_PRIME_LEASE_MS 3000u         /* heartbeat expiry (the app sends one per second) */
#define DSP_PRIME_CUTOFF_MARGIN_MS 500u  /* independent cutoff after the service's own deadline */
#define DSP_PRIME_TICK_MS 200u           /* BLE loop period while priming */
#define DSP_PRIME_HARD_MAX_MS 30000u     /* no configuration may exceed this */
#define DSP_PRIME_DOSE_MARGIN_S 900u     /* refused when a dose is due within 15 min */
#define DSP_PRIME_MIN_BATTERY_MV 3800u   /* VALID measurement in this wake, no BATTERY_LOW episode */

/* PRIME_START verdict (wire values; PRIME_STOP / PRIME_STATUS always answer OK). */
typedef enum {
    DSP_PRIME_V_OK = 0,          /* running (started now, or already running: not restarted) */
    DSP_PRIME_V_NOT_CAPABLE = 1, /* firmware/hardware without priming */
    DSP_PRIME_V_BATTERY = 2,     /* battery not VALID >= 3,800 mV in this wake or BATTERY_LOW open */
    DSP_PRIME_V_DOSE_SOON = 3,   /* a scheduled dose is due within 15 min */
    DSP_PRIME_V_RESERVOIR = 4,   /* reservoir not configured, estimated empty or BLOCKED */
    DSP_PRIME_V_FAULT = 5,       /* pump fault/unconfirmed OFF, degraded, inhibited or start failed */
    DSP_PRIME_V_BUSY = 6,        /* firmware update pending */
} dsp_prime_verdict_t;

/* Why the last run stopped (wire values). */
typedef enum {
    DSP_PRIME_STOP_NONE = 0,
    DSP_PRIME_STOP_USER = 1,       /* PRIME_STOP */
    DSP_PRIME_STOP_MAX_TIME = 2,   /* per-activation maximum reached */
    DSP_PRIME_STOP_HEARTBEAT = 3,  /* lease expired */
    DSP_PRIME_STOP_DISCONNECT = 4, /* BLE connection lost */
    DSP_PRIME_STOP_SESSION = 5,    /* security session or service window ended */
    DSP_PRIME_STOP_FAULT = 6,      /* independent cutoff fired, readback or re-arm failed */
    DSP_PRIME_STOP_DOSE = 7,       /* a scheduled dose became due */
} dsp_prime_stop_t;

typedef struct {
    bool capable;          /* pump port with priming and a non-zero maximum */
    bool fault;            /* pump fault / unconfirmed OFF / degraded / inhibited / storage fault */
    bool busy;             /* firmware update pending */
    bool reservoir_ok;     /* configured, remaining > 0, not BLOCKED, no refill pending */
    bool battery_valid;    /* VALID measurement in this wake */
    uint32_t battery_mv;
    bool battery_low_open;
    bool clock_trusted;
    dsp_utc_t now;
    bool next_dose_known;
    dsp_utc_t next_dose_at;
} dsp_prime_conditions_t;

/* PRIME_START admission; first failing rule: NOT_CAPABLE, FAULT, BUSY, BATTERY, RESERVOIR,
 * DOSE_SOON. An untrusted clock does
 * not refuse: no dose can run without a trusted clock, and a dose that becomes due later in the
 * session stops priming (DSP_PRIME_STOP_DOSE). */
dsp_prime_verdict_t dsp_prime_check(const dsp_prime_conditions_t *c);

/* Effective maximum: cfg_ms capped at DSP_PRIME_HARD_MAX_MS (0 = not capable). */
uint32_t dsp_prime_max_ms(uint32_t cfg_ms);

#ifdef __cplusplus
}
#endif

#endif
