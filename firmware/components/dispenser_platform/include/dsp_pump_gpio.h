#ifndef DSP_PUMP_GPIO_H
#define DSP_PUMP_GPIO_H

/*
 * GPIO pump control signal (bench stage, CONFIG_DSP_PUMP_GPIO; pure C, host-tested).
 *
 * Implements dsp_pump_port_t over a small hardware interface (pad drive, pad readback, pad hold,
 * an independent one-shot cutoff, a blocking wait). The Dosing Service stays the only caller and
 * has already passed every gate and limit (trusted clock, reservoir, calibration, readiness,
 * dose/runtime/daily limits, minimum interval, durable claim) before run() is called. The driver
 * adds its own defenses:
 *  - run() refuses runtime 0 or above its own maximum (never activates);
 *  - the output is activated only after the independent cutoff is armed (runtime + margin) and the
 *    pad reads back inactive; the cutoff forces the pad inactive even if the calling task stalls;
 *  - the pad is read back after activation and after deactivation; any mismatch, a failed wait or
 *    a fired cutoff is DSP_PUMP_FAILED (the Dosing Service counts the volume with uncertainty);
 *  - every return leaves the pad driven inactive and held (deep-sleep hold), force_off() is
 *    idempotent and is_off() is a pad readback, not a remembered flag.
 * What the electrical level is before the firmware configures the pad (reset, boot ROM, after a
 * wake from deep sleep) is outside software control: the driver circuit needs an external
 * resistor that keeps the gate inactive (see firmware/README.md "GPIO pump control signal").
 *
 * DSP_PUMP_OK means "the control signal was active for the commanded runtime"; nothing measures
 * liquid (there is no flow sensor), exactly as for the future pump.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_dosing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_PUMP_GPIO_CUTOFF_MARGIN_MS 100u /* independent cutoff = runtime + margin */
/* Priming (fw 0.3.3): the output is never active longer than prime_max_ms + this margin. */
#define DSP_PUMP_GPIO_PRIME_MARGIN_MS 500u

typedef struct {
    void *ctx;
    /* Pad as push-pull output with input enabled (readback), driven to `level`. */
    bool (*configure)(void *ctx, int level);
    bool (*set_level)(void *ctx, int level);
    /* Pad level read back (0/1), negative on error. */
    int (*read_level)(void *ctx);
    /* Pad hold on/off (held pads keep their level through deep sleep). */
    void (*hold)(void *ctx, bool on);
    /* One-shot independent cutoff: after ms it drives the pad inactive and calls
     * dsp_pump_gpio_cutoff_fired(). Must not depend on the calling task. */
    bool (*cutoff_arm)(void *ctx, uint32_t ms);
    void (*cutoff_disarm)(void *ctx);
    /* Blocking wait of ms while the output is active; false = the wait was interrupted. */
    bool (*wait_ms)(void *ctx, uint32_t ms);
    /* Priming only (NULL = priming not capable): restarts the PENDING cutoff to fire after ms;
     * false if it is no longer pending (already fired) or could not be restarted. */
    bool (*cutoff_rearm)(void *ctx, uint32_t ms);
    /* Priming only: monotonic milliseconds. */
    uint64_t (*now_ms)(void *ctx);
} dsp_pump_gpio_hw_t;

typedef struct {
    uint32_t runs;          /* activations */
    uint32_t refused;       /* run() refused before activation */
    uint32_t failures;      /* DSP_PUMP_FAILED */
    uint32_t cutoffs;       /* independent cutoff fired */
    uint32_t force_offs;
    uint64_t total_ms;      /* commanded active time of completed runs */
    uint32_t primes;        /* priming activations */
    uint64_t prime_ms;      /* active time of ended priming runs */
} dsp_pump_gpio_stats_t;

typedef struct {
    const dsp_pump_gpio_hw_t *hw;
    int active_level;       /* 1 = active-high */
    uint32_t max_runtime_ms;
    bool configured;
    volatile bool running;
    volatile bool cutoff_fired;
    uint32_t prime_max_ms;  /* 0 = priming not capable */
    bool priming;           /* between prime_start and prime_stop/force_off */
    bool prime_fault;       /* an extension or readback failed during priming */
    uint64_t prime_t0;      /* now_ms at prime_start */
    dsp_pump_gpio_stats_t st;
} dsp_pump_gpio_t;

/* RAM state only (as after a reset); nothing touches the pad until force_off()/run(). */
void dsp_pump_gpio_init(dsp_pump_gpio_t *d, const dsp_pump_gpio_hw_t *hw, bool active_high,
                        uint32_t max_runtime_ms);
/* Enables priming with its own maximum (separate from max_runtime_ms; 0 = disabled). Needs the
 * cutoff_rearm and now_ms hardware functions. Call before dsp_pump_gpio_port(). */
void dsp_pump_gpio_set_prime_max(dsp_pump_gpio_t *d, uint32_t prime_max_ms);
/* The pump port of d (run / force_off / is_off, and the priming functions when enabled). */
void dsp_pump_gpio_port(dsp_pump_gpio_t *d, dsp_pump_port_t *out);
/* Called from the cutoff context after it drove the pad inactive. */
void dsp_pump_gpio_cutoff_fired(dsp_pump_gpio_t *d);

/*
 * Tick count for a blocking wait of ms with a tick period of tick_ms (FreeRTOS vTaskDelay).
 * vTaskDelay(n) returns after n tick interrupts, i.e. after a time in ((n - 1) * tick, n * tick]
 * depending on the phase of the first tick. With n = ceil(ms / tick) the GPIO pulse is therefore
 * in (ms - tick, ms + tick): up to one tick SHORTER or longer than requested (bench 2026-11-20:
 * 7,991 ms for 8,000 ms at a 10 ms tick). The independent cutoff fires at ms +
 * DSP_PUMP_GPIO_CUTOFF_MARGIN_MS, which must exceed one tick (checked at build time by the ESP-IDF
 * adapter), so a normal run never reaches it. 0 for ms 0 or tick 0.
 */
uint32_t dsp_pump_gpio_delay_ticks(uint32_t ms, uint32_t tick_ms);

#ifdef __cplusplus
}
#endif

#endif
