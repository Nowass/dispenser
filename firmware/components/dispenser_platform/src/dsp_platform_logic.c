#include "dsp_platform_logic.h"

#include <string.h>

dsp_wake_cause_t dsp_plat_wake_cause(dsp_plat_reset_t reset, uint32_t wake_sources)
{
    switch (reset) {
    case DSP_PLAT_RST_DEEP_SLEEP:
        return (wake_sources & DSP_PLAT_WAKE_GPIO) != 0 ? DSP_CAUSE_BUTTON : DSP_CAUSE_TIMER;
    case DSP_PLAT_RST_SOFTWARE:
        return DSP_CAUSE_SOFTWARE;
    case DSP_PLAT_RST_PANIC:
        return DSP_CAUSE_PANIC;
    case DSP_PLAT_RST_WATCHDOG:
        return DSP_CAUSE_WATCHDOG;
    case DSP_PLAT_RST_BROWNOUT:
        return DSP_CAUSE_BROWNOUT;
    default:
        return DSP_CAUSE_POWER_ON; /* power-on, external, unknown */
    }
}

bool dsp_plat_reset_keeps_rtc(dsp_plat_reset_t reset)
{
    switch (reset) {
    case DSP_PLAT_RST_DEEP_SLEEP:
    case DSP_PLAT_RST_SOFTWARE:
    case DSP_PLAT_RST_PANIC:
    case DSP_PLAT_RST_WATCHDOG:
        return true;
    default:
        return false;
    }
}

uint64_t dsp_plat_sleep_us(int64_t sleep_s)
{
    if (sleep_s < DSP_POWER_MIN_SLEEP_S) {
        sleep_s = DSP_POWER_MIN_SLEEP_S;
    }
    if (sleep_s > DSP_POWER_MAX_SLEEP_MAX_S) {
        sleep_s = DSP_POWER_MAX_SLEEP_MAX_S;
    }
    return (uint64_t)sleep_s * 1000000u;
}

dsp_plat_button_t dsp_plat_button_check(int gpio)
{
    if (gpio < 0) {
        return DSP_PLAT_BUTTON_DISABLED;
    }
    if (gpio > 5) {
        return DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE;
    }
    if (gpio == 2) {
        return DSP_PLAT_BUTTON_STRAPPING;
    }
    return DSP_PLAT_BUTTON_OK;
}

dsp_plat_sleep_arm_t dsp_plat_sleep_arm(int gpio, bool button_active, int64_t sleep_s)
{
    dsp_plat_sleep_arm_t a;
    a.pin = dsp_plat_button_check(gpio);
    a.held = a.pin == DSP_PLAT_BUTTON_OK && button_active;
    a.arm_gpio = a.pin == DSP_PLAT_BUTTON_OK && !button_active;
    a.sleep_s = sleep_s;
    if (a.held && a.sleep_s > DSP_PLAT_BUTTON_HELD_RETRY_S) {
        a.sleep_s = DSP_PLAT_BUTTON_HELD_RETRY_S;
    }
    return a;
}

const char *dsp_plat_wake_cause_name(dsp_wake_cause_t cause)
{
    switch (cause) {
    case DSP_CAUSE_POWER_ON:
        return "POWER_ON";
    case DSP_CAUSE_TIMER:
        return "TIMER";
    case DSP_CAUSE_BUTTON:
        return "BUTTON";
    case DSP_CAUSE_SOFTWARE:
        return "SOFTWARE";
    case DSP_CAUSE_WATCHDOG:
        return "WATCHDOG";
    case DSP_CAUSE_PANIC:
        return "PANIC";
    case DSP_CAUSE_BROWNOUT:
        return "BROWNOUT";
    default:
        return "UNKNOWN";
    }
}

size_t dsp_plat_rtc_load(const dsp_plat_rtc_t *rtc, uint8_t *buf, size_t cap)
{
    if (rtc == NULL || buf == NULL || rtc->len == 0 || rtc->len > DSP_PLAT_RTC_CAP ||
        rtc->len_inv != ~rtc->len) {
        return 0;
    }
    size_t n = rtc->len < cap ? rtc->len : cap;
    memcpy(buf, rtc->data, n);
    return rtc->len;
}

bool dsp_plat_rtc_store(dsp_plat_rtc_t *rtc, const uint8_t *buf, size_t len)
{
    if (rtc == NULL || buf == NULL || len == 0 || len > DSP_PLAT_RTC_CAP) {
        return false;
    }
    rtc->len = 0; /* invalid while the data is being replaced */
    rtc->len_inv = 0;
    memcpy(rtc->data, buf, len);
    rtc->len_inv = ~(uint32_t)len;
    rtc->len = (uint32_t)len;
    return true;
}

dsp_plat_batt_pin_t dsp_plat_batt_pin_check(int gpio, int button_gpio, int pump_gpio)
{
    if (gpio < 0) {
        return DSP_PLAT_BATT_PIN_DISABLED;
    }
    if (gpio > 4) {
        return DSP_PLAT_BATT_PIN_NOT_ADC1; /* ESP32-C3 ADC1: GPIO0-GPIO4 */
    }
    if (gpio == 2) {
        return DSP_PLAT_BATT_PIN_STRAPPING;
    }
    if (gpio == button_gpio || gpio == pump_gpio) {
        return DSP_PLAT_BATT_PIN_CONFLICT;
    }
    return DSP_PLAT_BATT_PIN_OK;
}

static dsp_plat_batt_result_t batt_result(uint8_t state)
{
    dsp_plat_batt_result_t r;
    memset(&r, 0, sizeof r);
    r.state = state;
    return r;
}

dsp_plat_batt_result_t dsp_plat_batt_compute(const int *adc_mv, size_t n, uint32_t divider_x1000)
{
    if (adc_mv == NULL || n < 2u * DSP_PLAT_BATT_TRIM + 1u || n > DSP_PLAT_BATT_SAMPLES ||
        divider_x1000 < 1000u || divider_x1000 > 10000u) {
        return batt_result(DSP_BATT_ADC_FAILED);
    }
    int s[DSP_PLAT_BATT_SAMPLES];
    for (size_t i = 0; i < n; i++) {
        if (adc_mv[i] < 0 || adc_mv[i] > 10000) {
            return batt_result(DSP_BATT_ADC_FAILED); /* not a calibrated reading */
        }
        /* insertion sort (n <= 16) */
        size_t j = i;
        while (j > 0 && s[j - 1] > adc_mv[i]) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = adc_mv[i];
    }
    size_t lo = DSP_PLAT_BATT_TRIM;
    size_t hi = n - DSP_PLAT_BATT_TRIM; /* exclusive */
    uint32_t sum = 0;
    for (size_t i = lo; i < hi; i++) {
        sum += (uint32_t)s[i];
    }
    uint32_t k = (uint32_t)(hi - lo);
    uint32_t adc = (sum + k / 2u) / k;
    dsp_plat_batt_result_t r = batt_result(DSP_BATT_VALID);
    r.adc_mv = (uint16_t)adc;
    r.spread_mv = (uint16_t)(s[hi - 1] - s[lo]);
    uint32_t batt = (adc * divider_x1000 + 500u) / 1000u; /* <= 10000 * 10000: no overflow */
    if (r.spread_mv > DSP_PLAT_BATT_MAX_SPREAD_MV) {
        r.state = DSP_BATT_UNSTABLE;
    } else if (batt < DSP_BATT_MIN_MV || batt > DSP_BATT_MAX_MV) {
        r.state = DSP_BATT_OUT_OF_RANGE;
    } else {
        r.battery_mv = (uint16_t)batt;
    }
    return r;
}

const char *dsp_plat_batt_state_name(uint8_t state)
{
    switch (state) {
    case DSP_BATT_VALID:
        return "VALID";
    case DSP_BATT_NOT_MEASURED:
        return "NOT_MEASURED";
    case DSP_BATT_ADC_FAILED:
        return "ADC_FAILED";
    case DSP_BATT_NO_CALIBRATION:
        return "NO_CALIBRATION";
    case DSP_BATT_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case DSP_BATT_UNSTABLE:
        return "UNSTABLE";
    default:
        return "UNKNOWN";
    }
}
