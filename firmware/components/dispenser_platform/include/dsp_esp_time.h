#ifndef DSP_ESP_TIME_H
#define DSP_ESP_TIME_H

/*
 * ESP-IDF adapters for time management (milestone M1.4). Runtime behavior has NOT been
 * verified on hardware (no board yet): RTC continuity across reset types, deep-sleep RTC
 * accuracy and SNTP over a real network all remain to be tested.
 *
 *  - Clock port: system time (gettimeofday/settimeofday, RTC-backed in deep sleep with the
 *    default CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT).
 *  - Continuity evidence: reset reason (power-on/brownout/other resets lose the RTC time
 *    base) AND an RTC_NOINIT marker holding the last accepted sync time; both must agree.
 *  - Retained retry state in the same RTC block (lost on power-on, then defaults apply).
 *  - SNTP time source: esp_netif_sntp for the protocol; the weak sntp_sync_time() hook is
 *    overridden so that the clock is NEVER changed by SNTP itself: the sample is handed to
 *    the domain, which decides, and dsp_clock_port_t.set() applies an immediate step.
 *  - Wi-Fi station driver: bounded connect attempts, esp_wifi storage set to RAM so that
 *    credentials are never written to NVS by the Wi-Fi driver; credentials are never logged.
 */

#include "dsp_net.h"
#include "dsp_timekeeping.h"

#ifdef __cplusplus
extern "C" {
#endif

void dsp_esp_clock_port(dsp_clock_port_t *out);

/* Wake evidence: raw time + whether the time base ran continuously since the sync whose
 * time is persisted as last_sync (0 = never synced). */
dsp_time_reading_t dsp_esp_clock_reading(dsp_utc_t persisted_last_sync);

/* RTC-retained retry state: false (and *out untouched) when lost or invalid. */
bool dsp_esp_clock_load_retained(dsp_time_retained_t *out);
void dsp_esp_clock_save_retained(const dsp_time_retained_t *retained);

/* Singletons: one radio and one SNTP client per device. */
const dsp_wifi_driver_t *dsp_esp_wifi_driver(void);
const dsp_time_source_t *dsp_esp_sntp_source(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_TIME_H */
