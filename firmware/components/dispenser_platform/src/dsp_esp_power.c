#include "dsp_esp_app.h"

#include <string.h>

#include "dsp_esp_time.h"
#include "dsp_platform_logic.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "dsp_power";

/* RTC_NOINIT: survives deep sleep and soft resets, arbitrary after power-on (length header
 * checked here; magic/version/CRC/ranges by the domain). */
static RTC_NOINIT_ATTR dsp_plat_rtc_t s_rtc;

static const dsp_pump_port_t *s_pump;
static const dsp_wifi_driver_t *s_wifi;

static dsp_plat_reset_t reset_kind(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return DSP_PLAT_RST_POWER_ON;
    case ESP_RST_SW:
        return DSP_PLAT_RST_SOFTWARE;
    case ESP_RST_PANIC:
        return DSP_PLAT_RST_PANIC;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return DSP_PLAT_RST_WATCHDOG;
    case ESP_RST_DEEPSLEEP:
        return DSP_PLAT_RST_DEEP_SLEEP;
    case ESP_RST_BROWNOUT:
        return DSP_PLAT_RST_BROWNOUT;
    case ESP_RST_UNKNOWN:
        return DSP_PLAT_RST_UNKNOWN;
    default:
        return DSP_PLAT_RST_EXTERNAL;
    }
}

static dsp_wake_cause_t pw_wake_cause(void *ctx)
{
    (void)ctx;
    uint32_t causes = esp_sleep_get_wakeup_causes();
    uint32_t w = 0;
    if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        w |= DSP_PLAT_WAKE_TIMER;
    }
    if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        w |= DSP_PLAT_WAKE_GPIO;
    }
    return dsp_plat_wake_cause(reset_kind(), w);
}

static size_t pw_load(void *ctx, uint8_t *buf, size_t cap)
{
    (void)ctx;
    return dsp_plat_rtc_load(&s_rtc, buf, cap);
}

static bool pw_save(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return dsp_plat_rtc_store(&s_rtc, buf, len);
}

static void stop_actuators(void)
{
    if (s_pump != NULL && s_pump->force_off != NULL) {
        s_pump->force_off(s_pump->ctx);
    }
    if (s_wifi != NULL && s_wifi->stop != NULL) {
        s_wifi->stop(s_wifi->ctx); /* idempotent */
    }
}

void dsp_esp_fail_safe(uint32_t failures)
{
    /* Never deep sleep here. The retained block (with the fail-safe count) was saved by the
     * application before this call. HARDWARE-UNVERIFIED: the pump gate must fall OFF through
     * its hardware pull-down during and after the reset (P1). A pump-off confirmation that
     * never succeeds makes this a reset loop (battery drain): hardware integration risk. */
    stop_actuators();
    ESP_LOGE(TAG, "fail-safe: shutdown not confirmed (failures 0x%x): resetting",
             (unsigned)failures);
    esp_restart();
}

static void pw_fail_safe(void *ctx, uint32_t failures)
{
    (void)ctx;
    dsp_esp_fail_safe(failures);
}

static void pw_sleep(void *ctx, int64_t sleep_s)
{
    (void)ctx;
    esp_err_t err = esp_sleep_enable_timer_wakeup(dsp_plat_sleep_us(sleep_s));
    if (err != ESP_OK) {
        /* Without a timer the device could sleep forever: reset instead (a new wake). */
        ESP_LOGE(TAG, "timer wake-up not armed (0x%x)", (unsigned)err);
        dsp_esp_fail_safe(0);
    }
#if CONFIG_DSP_BUTTON_WAKE_GPIO >= 0
    if (dsp_plat_button_check(CONFIG_DSP_BUTTON_WAKE_GPIO) == DSP_PLAT_BUTTON_OK) {
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ull << CONFIG_DSP_BUTTON_WAKE_GPIO,
#if CONFIG_DSP_BUTTON_WAKE_ACTIVE_HIGH
            ESP_GPIO_WAKEUP_GPIO_HIGH
#else
            ESP_GPIO_WAKEUP_GPIO_LOW
#endif
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "button wake-up not armed (0x%x): timer only", (unsigned)err);
        }
    } else {
        ESP_LOGW(TAG, "button GPIO %d rejected (not wake-capable or strapping): timer only",
                 CONFIG_DSP_BUTTON_WAKE_GPIO);
    }
#endif
    /* Stack high-water mark of the application task (hardware acceptance 7.6). */
    ESP_LOGI(TAG, "deep sleep for %lld s (stack free min %u B)", (long long)sleep_s,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    esp_deep_sleep_start();
}

void dsp_esp_power_port(dsp_power_port_t *out, const dsp_pump_port_t *pump,
                        const dsp_wifi_driver_t *wifi)
{
    s_pump = pump;
    s_wifi = wifi;
    out->ctx = NULL;
    out->wake_cause = pw_wake_cause;
    out->load_retained = pw_load;
    out->save_retained = pw_save;
    out->sleep = pw_sleep;
    out->fail_safe = pw_fail_safe;
}

/* ---- Time continuity port ------------------------------------------------------------- */

static dsp_time_reading_t tp_reading(void *ctx, dsp_utc_t last_sync)
{
    (void)ctx;
    return dsp_esp_clock_reading(last_sync);
}

void dsp_esp_time_port(dsp_time_port_t *out)
{
    out->ctx = NULL;
    out->reading = tp_reading;
}
