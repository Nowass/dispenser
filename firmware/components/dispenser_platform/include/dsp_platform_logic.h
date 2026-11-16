#ifndef DSP_PLATFORM_LOGIC_H
#define DSP_PLATFORM_LOGIC_H

/*
 * Pure decision logic of the M1.9c platform adapters (no ESP-IDF includes): compiled into
 * the ESP-IDF component AND the host test build, so the mapping rules are host-tested.
 * The adapters translate ESP-IDF values into these plain enums first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_power.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reset reason categories (from esp_reset_reason()). */
typedef enum {
    DSP_PLAT_RST_UNKNOWN = 0,
    DSP_PLAT_RST_POWER_ON,
    DSP_PLAT_RST_EXTERNAL,   /* reset pin, JTAG, USB, SDIO, eFuse, power glitch, lock-up */
    DSP_PLAT_RST_SOFTWARE,   /* esp_restart (incl. the fail-safe reset) */
    DSP_PLAT_RST_PANIC,
    DSP_PLAT_RST_WATCHDOG,   /* interrupt, task or other watchdog */
    DSP_PLAT_RST_DEEP_SLEEP, /* wake from deep sleep */
    DSP_PLAT_RST_BROWNOUT,
} dsp_plat_reset_t;

/* Deep-sleep wake sources (from esp_sleep_get_wakeup_causes()). */
enum {
    DSP_PLAT_WAKE_TIMER = 1u << 0,
    DSP_PLAT_WAKE_GPIO = 1u << 1,
};

/* Domain wake cause. A GPIO wake (button) wins over a simultaneous timer wake (the timer
 * reason is re-planned by the application anyway). An unidentified deep-sleep wake is
 * treated as TIMER (RTC memory is kept; no service mode is opened). Unknown and external
 * resets map to POWER_ON (RTC content not trusted). */
dsp_wake_cause_t dsp_plat_wake_cause(dsp_plat_reset_t reset, uint32_t wake_sources);

/* RTC time base survives the reset (same rule as the M1.4 clock adapter). */
bool dsp_plat_reset_keeps_rtc(dsp_plat_reset_t reset);

/* Deep-sleep duration in microseconds: clamped to [DSP_POWER_MIN_SLEEP_S,
 * DSP_POWER_MAX_SLEEP_MAX_S], overflow-free. */
uint64_t dsp_plat_sleep_us(int64_t sleep_s);

/* Button wake pin check for the XIAO ESP32-C3 (ESP32-C3 deep-sleep wake pads GPIO0-GPIO5).
 * -1 = no button configured (the default until the hardware review assigns one).
 * Strapping pins (GPIO2 on the ESP32-C3; GPIO8/9 are not wake-capable anyway) are
 * rejected until the hardware review has analysed them. */
typedef enum {
    DSP_PLAT_BUTTON_DISABLED = 0,
    DSP_PLAT_BUTTON_OK,
    DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE,
    DSP_PLAT_BUTTON_STRAPPING,
} dsp_plat_button_t;

dsp_plat_button_t dsp_plat_button_check(int gpio);

/* RTC-retained storage of the 48-byte domain block (contents validated by the domain:
 * magic, version, length, CRC, ranges). Lives in RTC_NOINIT memory: arbitrary after a
 * power-on, so the stored length is checked before anything is copied. */
#define DSP_PLAT_RTC_CAP 64u
typedef struct {
    uint32_t len;
    uint32_t len_inv; /* ~len: a torn or random header is rejected */
    uint8_t data[DSP_PLAT_RTC_CAP];
} dsp_plat_rtc_t;

/* Copies at most cap bytes; returns the stored length, 0 when the header is invalid. */
size_t dsp_plat_rtc_load(const dsp_plat_rtc_t *rtc, uint8_t *buf, size_t cap);
bool dsp_plat_rtc_store(dsp_plat_rtc_t *rtc, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSP_PLATFORM_LOGIC_H */
