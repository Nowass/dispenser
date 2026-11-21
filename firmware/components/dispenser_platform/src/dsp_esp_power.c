#include "dsp_esp_app.h"

#include <string.h>

#include "dsp_esp_time.h"
#include "dsp_platform_logic.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "dsp_power";

/* RTC_NOINIT: survives deep sleep and soft resets, arbitrary after power-on (length header
 * checked here; magic/version/CRC/ranges by the domain). */
static RTC_NOINIT_ATTR dsp_plat_rtc_t s_rtc;

#if CONFIG_DSP_BUTTON_WAKE_GPIO >= 0
/* MVP wake button (sdkconfig.button.defaults: XIAO D1 = GPIO3, normally-open to GND, external
 * 10 kOhm pull-up to 3V3, active low). ESP32-C3 deep-sleep GPIO wake pads are GPIO0-GPIO5; the
 * strapping pin GPIO2 is refused. dsp_plat_button_check repeats the rule at run time. */
_Static_assert(CONFIG_DSP_BUTTON_WAKE_GPIO <= 5 && CONFIG_DSP_BUTTON_WAKE_GPIO != 2,
               "button wake GPIO must be a non-strapping ESP32-C3 deep-sleep wake pad (0, 1, 3, 4, 5)");
#define BTN_GPIO CONFIG_DSP_BUTTON_WAKE_GPIO
#if CONFIG_DSP_BUTTON_WAKE_ACTIVE_HIGH
#define BTN_ACTIVE_LEVEL 1
#else
#define BTN_ACTIVE_LEVEL 0
#endif
#define BTN_SAMPLES 5
#define BTN_SAMPLE_GAP_US 2000

/* Level of the button right before sleep (input only, never an output). The previous deep sleep
 * left the pad latched by the IDF wake-pad hold, and a held pad ignores configuration: release it
 * first. The pull matches what ESP-IDF applies in sleep (CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_
 * RESISTORS: pull-up for a low-level wake), in parallel with the external 10 kOhm. Active in ANY
 * of 5 samples over ~8 ms counts as active (conservative: never arm a level wake that would fire
 * at once). A configuration failure also counts as active (timer only, short retry). */
static bool button_active(void)
{
    (void)gpio_hold_dis(BTN_GPIO);
    gpio_config_t c = {
        .pin_bit_mask = 1ull << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BTN_ACTIVE_LEVEL ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = BTN_ACTIVE_LEVEL ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&c) != ESP_OK) {
        return true;
    }
    esp_rom_delay_us(100); /* settle after the pull change */
    for (int i = 0; i < BTN_SAMPLES; i++) {
        if (gpio_get_level(BTN_GPIO) == BTN_ACTIVE_LEVEL) {
            return true;
        }
        if (i + 1 < BTN_SAMPLES) {
            esp_rom_delay_us(BTN_SAMPLE_GAP_US);
        }
    }
    return false;
}
#endif

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
    static bool logged;
    uint32_t causes = esp_sleep_get_wakeup_causes();
    uint32_t w = 0;
    if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        w |= DSP_PLAT_WAKE_TIMER;
    }
    if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        w |= DSP_PLAT_WAKE_GPIO;
    }
    dsp_wake_cause_t cause = dsp_plat_wake_cause(reset_kind(), w);
    if (!logged) {
        /* Once per boot; only the cause, the reset reason code and the wake pad mask. */
        logged = true;
        ESP_LOGI(TAG, "wake: cause=%s (reset reason %d, timer=%d, gpio=%d, gpio mask 0x%llx)",
                 dsp_plat_wake_cause_name(cause), (int)esp_reset_reason(),
                 (w & DSP_PLAT_WAKE_TIMER) != 0, (w & DSP_PLAT_WAKE_GPIO) != 0,
                 (unsigned long long)esp_sleep_get_gpio_wakeup_status());
    }
    return cause;
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
    /* Both wake sources: the timer (scheduler, always) and, when usable and not held, the
     * button. A held or stuck button arms the timer only, capped to a short retry (level wake:
     * arming it would wake at once into a new service window). */
#if CONFIG_DSP_BUTTON_WAKE_GPIO >= 0
    dsp_plat_sleep_arm_t arm = dsp_plat_sleep_arm(BTN_GPIO, button_active(), sleep_s);
#else
    dsp_plat_sleep_arm_t arm = dsp_plat_sleep_arm(-1, false, sleep_s);
#endif
    esp_err_t err = esp_sleep_enable_timer_wakeup(dsp_plat_sleep_us(arm.sleep_s));
    if (err != ESP_OK) {
        /* Without a timer the device could sleep forever: reset instead (a new wake). */
        ESP_LOGE(TAG, "timer wake-up not armed (0x%x)", (unsigned)err);
        dsp_esp_fail_safe(0);
    }
#if CONFIG_DSP_BUTTON_WAKE_GPIO >= 0
    if (arm.arm_gpio) {
        err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ull << BTN_GPIO, BTN_ACTIVE_LEVEL ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "button wake-up not armed (0x%x): timer only", (unsigned)err);
        } else {
            ESP_LOGI(TAG, "button wake armed (GPIO%d, active %s)", BTN_GPIO,
                     BTN_ACTIVE_LEVEL ? "high" : "low");
        }
    } else if (arm.held) {
        ESP_LOGW(TAG, "button active at sleep entry: GPIO wake not armed, timer %lld s (planned %lld s)",
                 (long long)arm.sleep_s, (long long)sleep_s);
    } else {
        ESP_LOGW(TAG, "button GPIO %d rejected (not wake-capable or strapping): timer only", BTN_GPIO);
    }
#endif
    /* Last action before sleep: the pump output inactive (the GPIO driver also enables the
     * deep-sleep pad hold of the inactive level; idempotent). */
    if (s_pump != NULL && s_pump->force_off != NULL) {
        s_pump->force_off(s_pump->ctx);
    }
    /* Stack high-water mark of the application task (hardware acceptance 7.6). */
    ESP_LOGI(TAG, "deep sleep for %lld s (stack free min %u B)", (long long)arm.sleep_s,
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
