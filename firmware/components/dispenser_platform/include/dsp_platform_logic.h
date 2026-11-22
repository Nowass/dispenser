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

#include "dsp_battery.h"
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
 * -1 = no button configured (the Kconfig default; the MVP wake button build sets GPIO3 through
 * sdkconfig.button.defaults). Strapping pins (GPIO2 on the ESP32-C3; GPIO8/9 are not
 * wake-capable anyway) are rejected. */
typedef enum {
    DSP_PLAT_BUTTON_DISABLED = 0,
    DSP_PLAT_BUTTON_OK,
    DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE,
    DSP_PLAT_BUTTON_STRAPPING,
} dsp_plat_button_t;

dsp_plat_button_t dsp_plat_button_check(int gpio);

/* Sleep-entry decision for the button wake (MVP wake button, XIAO D1 = GPIO3, active low).
 * The ESP32-C3 GPIO deep-sleep wake is LEVEL triggered only: a button that is still active at
 * sleep entry would wake the chip at once, and every button wake opens a BLE service window
 * (a wake loop that drains the battery). So:
 *  - pin not usable (disabled, not wake-capable, strapping): timer only, sleep unchanged;
 *  - button active (held or stuck) at sleep entry: GPIO wake NOT armed, the timer sleep is
 *    capped to DSP_PLAT_BUTTON_HELD_RETRY_S (the following timer wake re-checks and arms the
 *    button again once it is released; a timer wake never opens a service window);
 *  - otherwise: GPIO wake armed together with the unchanged timer sleep.
 * The timer is ALWAYS armed (the scheduler's wake source is never replaced). */
#define DSP_PLAT_BUTTON_HELD_RETRY_S ((int64_t)60)

typedef struct {
    dsp_plat_button_t pin; /* dsp_plat_button_check(gpio) */
    bool held;             /* pin usable and the button was active at sleep entry */
    bool arm_gpio;         /* arm the GPIO wake for this sleep */
    int64_t sleep_s;       /* timer sleep to arm (never longer than requested) */
} dsp_plat_sleep_arm_t;

dsp_plat_sleep_arm_t dsp_plat_sleep_arm(int gpio, bool button_active, int64_t sleep_s);

/* Log-safe name of a wake cause ("BUTTON", "TIMER", ...; never device data). */
const char *dsp_plat_wake_cause_name(dsp_wake_cause_t cause);

/* ---- Battery voltage (XIAO ESP32-C3: BAT+ -> R1 -> ADC pad -> R2 -> GND) -------------------
 *
 * Pin check: only ADC1 pads (GPIO0-GPIO4 on the ESP32-C3; ADC2 is not usable together with the
 * radio), never the strapping pin GPIO2 (a divider at about half the battery voltage could
 * disturb the boot mode), never the wake button or the pump pin. -1 = no battery telemetry. */
typedef enum {
    DSP_PLAT_BATT_PIN_DISABLED = 0,
    DSP_PLAT_BATT_PIN_OK,
    DSP_PLAT_BATT_PIN_NOT_ADC1,
    DSP_PLAT_BATT_PIN_STRAPPING,
    DSP_PLAT_BATT_PIN_CONFLICT, /* the wake button or the pump pin */
} dsp_plat_batt_pin_t;

dsp_plat_batt_pin_t dsp_plat_batt_pin_check(int gpio, int button_gpio, int pump_gpio);

/* Sampling. The 1 MOhm / 1 MOhm divider is a ~500 kOhm source: without a capacitor at the pad
 * the ADC's sampling capacitor needs several conversions on the same channel to settle, so the
 * adapter first discards DSP_PLAT_BATT_DISCARD conversions, then takes DSP_PLAT_BATT_SAMPLES
 * calibrated readings back to back (no delays; about a millisecond in total). The result is the
 * mean of the samples left after dropping the DSP_PLAT_BATT_TRIM lowest and highest (robust
 * against single disturbed conversions). A 100 nF capacitor from the pad to GND makes the source
 * stiff; the method is the same with or without it. */
#define DSP_PLAT_BATT_DISCARD 4u
#define DSP_PLAT_BATT_SAMPLES 16u
#define DSP_PLAT_BATT_TRIM 4u
/* Kept samples spread (max - min at the ADC input) above which the reading is UNSTABLE. */
#define DSP_PLAT_BATT_MAX_SPREAD_MV 100u

typedef struct {
    uint8_t state;       /* dsp_batt_state_t */
    uint16_t battery_mv; /* 0 unless state == DSP_BATT_VALID */
    uint16_t adc_mv;     /* trimmed mean at the ADC input (diagnostics; 0 when not computed) */
    uint16_t spread_mv;  /* max - min of the kept samples (diagnostics) */
} dsp_plat_batt_result_t;

/* adc_mv: n calibrated readings in mV (after the discarded ones); n must be between
 * 2 * DSP_PLAT_BATT_TRIM + 1 and DSP_PLAT_BATT_SAMPLES (else ADC_FAILED). divider_x1000: battery
 * voltage / ADC voltage x 1000 (2000 for two equal resistors); outside 1000..10000 = ADC_FAILED.
 * A negative reading is ADC_FAILED. The battery voltage is rounded to the nearest mV and checked
 * against the credible window (dsp_battery.h), never clamped. */
dsp_plat_batt_result_t dsp_plat_batt_compute(const int *adc_mv, size_t n, uint32_t divider_x1000);

/* Log-safe name of a battery state ("VALID", "ADC_FAILED", ...). */
const char *dsp_plat_batt_state_name(uint8_t state);

/* RTC-retained storage of the domain block (120 bytes since M1.10b.3; contents validated by
 * the domain: magic, version, length, CRC, ranges). Lives in RTC_NOINIT memory: arbitrary
 * after a power-on, so the stored length is checked before anything is copied. */
#define DSP_PLAT_RTC_CAP 128u
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
