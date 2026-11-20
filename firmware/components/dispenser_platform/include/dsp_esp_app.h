#ifndef DSP_ESP_APP_H
#define DSP_ESP_APP_H

/*
 * ESP-IDF adapters for the M1.9b Application and Power Services (milestone M1.9c).
 * Compiled and cross-built only: NOT run on hardware (no board yet). Nothing here
 * configures a GPIO output, and nothing can energize a pump.
 *
 *  - Power port: reset reason + deep-sleep wake sources -> dsp_wake_cause_t; the 48-byte
 *    retained block in RTC_NOINIT memory (length-checked here, validated by the domain);
 *    deep sleep with the RTC timer and, only if a button GPIO is configured and passes
 *    dsp_plat_button_check, a GPIO wake (default: none, pending the hardware review);
 *    fail-safe: pump off, Wi-Fi stopped, outcome logged, SoC reset (never deep sleep).
 *  - Queue lock: a portMUX critical section around the command queue only (a few bytes
 *    copied, no blocking call inside; task context only, never from an ISR).
 *  - Pump: SIMULATION ONLY. No GPIO, no actuation: run() only waits for the commanded
 *    runtime and counts; is_off() reports the simulated state. The real driver (gate pin,
 *    polarity, pull-down, hardware cutoff, watchdog relationship, reset behavior) needs a
 *    separate hardware review.
 *  - Time port: dsp_esp_clock_reading() (M1.4 continuity rule).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power port singleton. pump/wifi: what the fail-safe forces off (either may be NULL). */
void dsp_esp_power_port(dsp_power_port_t *out, const dsp_pump_port_t *pump,
                        const dsp_wifi_driver_t *wifi);

/* Unconditional fail-safe outside the application cycle (e.g. invalid wiring): same path
 * as the port's fail_safe(); does not return. */
void dsp_esp_fail_safe(uint32_t failures) __attribute__((noreturn));

/* Command-queue lock (portMUX critical section). */
void dsp_esp_app_sync(dsp_app_sync_t *out);

/* Time-continuity port over the M1.4 clock adapter. */
void dsp_esp_time_port(dsp_time_port_t *out);

/* Simulated pump (no GPIO). */
typedef struct {
    uint32_t runs;
    uint32_t force_offs;
    uint64_t total_ms;
    bool running;
} dsp_esp_pump_sim_stats_t;

const dsp_pump_port_t *dsp_esp_pump_sim(void);
dsp_esp_pump_sim_stats_t dsp_esp_pump_sim_stats(void);

/* BENCH GPIO pump control signal (only with CONFIG_DSP_PUMP_GPIO; see dsp_pump_gpio.h). */
const dsp_pump_port_t *dsp_esp_pump_gpio(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_APP_H */
