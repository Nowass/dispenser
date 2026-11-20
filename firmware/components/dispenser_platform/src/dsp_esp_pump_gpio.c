#include "dsp_esp_app.h"

#include "sdkconfig.h"

#if CONFIG_DSP_PUMP_GPIO
/*
 * BENCH GPIO pump control signal (CONFIG_DSP_PUMP_GPIO, off by default): the ESP-IDF hardware
 * interface of dsp_pump_gpio (pure, host-tested). Compiled only when a build opts in, so every
 * other configuration contains no pump GPIO code at all. Pin, polarity and the electrical
 * requirements: firmware/README.md "GPIO pump control signal".
 *
 * Cutoff: an esp_timer one-shot (dispatched by the esp_timer task, priority above the application
 * task) drives the pad inactive at runtime + 100 ms even if the application task stalls. If the
 * whole CPU stops, the interrupt/task watchdogs reset the chip; after a reset the pad is an input
 * again and only the external resistor of the driver keeps the gate inactive.
 */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dsp_pump_gpio.h"

#define PIN CONFIG_DSP_PUMP_GPIO_NUM
/* never a strapping pin (2, 8, 9), the SPI flash pins (11-17), USB D-/D+ (18, 19) or UART0 (20, 21) */
_Static_assert(PIN >= 0 && PIN <= 21 && PIN != 2 && PIN != 8 && PIN != 9 && !(PIN >= 11 && PIN <= 21),
               "CONFIG_DSP_PUMP_GPIO_NUM: not an allowed pump control pin on the ESP32-C3");
#if CONFIG_DSP_BUTTON_WAKE_GPIO >= 0
_Static_assert(PIN != CONFIG_DSP_BUTTON_WAKE_GPIO, "pump control pin equals the button wake pin");
#endif
#if CONFIG_DSP_PUMP_GPIO_ACTIVE_HIGH
#define ACTIVE_HIGH true
#else
#define ACTIVE_HIGH false
#endif

static const char *TAG = "dsp_pump_gpio";
static dsp_pump_gpio_t s_drv;
static dsp_pump_port_t s_port;
static esp_timer_handle_t s_cutoff;
static bool s_inited;

static int inactive_level(void)
{
    return ACTIVE_HIGH ? 0 : 1;
}

static bool hw_configure(void *ctx, int level)
{
    (void)ctx;
    (void)gpio_hold_dis(PIN);
    /* level first, then output: the pad never drives the active level while switching to output */
    (void)gpio_set_level(PIN, level);
    gpio_config_t c = {
        .pin_bit_mask = 1ull << PIN,
        .mode = GPIO_MODE_INPUT_OUTPUT, /* output with input enabled for the readback */
        .pull_up_en = ACTIVE_HIGH ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = ACTIVE_HIGH ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&c) == ESP_OK && gpio_set_level(PIN, level) == ESP_OK;
}

static bool hw_set_level(void *ctx, int level)
{
    (void)ctx;
    return gpio_set_level(PIN, level) == ESP_OK;
}

static int hw_read_level(void *ctx)
{
    (void)ctx;
    return gpio_get_level(PIN);
}

static void hw_hold(void *ctx, bool on)
{
    (void)ctx;
    if (on) {
        (void)gpio_hold_en(PIN);
        gpio_deep_sleep_hold_en(); /* digital pads keep a held level only with this (ESP32-C3) */
    } else {
        (void)gpio_hold_dis(PIN);
    }
}

static void cutoff_cb(void *arg)
{
    (void)arg;
    (void)gpio_set_level(PIN, inactive_level()); /* independent of the application task */
    dsp_pump_gpio_cutoff_fired(&s_drv);
}

static bool hw_cutoff_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (s_cutoff == NULL) {
        esp_timer_create_args_t a = {.callback = cutoff_cb, .dispatch_method = ESP_TIMER_TASK,
                                     .name = "dsp_pump_cutoff"};
        if (esp_timer_create(&a, &s_cutoff) != ESP_OK) {
            s_cutoff = NULL;
            return false;
        }
    }
    (void)esp_timer_stop(s_cutoff);
    return esp_timer_start_once(s_cutoff, (uint64_t)ms * 1000u) == ESP_OK;
}

static void hw_cutoff_disarm(void *ctx)
{
    (void)ctx;
    if (s_cutoff != NULL) {
        (void)esp_timer_stop(s_cutoff);
    }
}

static bool hw_wait_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGW(TAG, "GPIO%d ACTIVE for %u ms (bench: control signal only, no liquid measured)", PIN,
             (unsigned)ms);
    vTaskDelay((ms + portTICK_PERIOD_MS - 1u) / portTICK_PERIOD_MS); /* never shorter than ms */
    ESP_LOGW(TAG, "GPIO%d inactive after %u ms", PIN, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static const dsp_pump_gpio_hw_t HW = {NULL, hw_configure, hw_set_level, hw_read_level, hw_hold,
                                      hw_cutoff_arm, hw_cutoff_disarm, hw_wait_ms};

const dsp_pump_port_t *dsp_esp_pump_gpio(void)
{
    if (!s_inited) {
        dsp_pump_gpio_init(&s_drv, &HW, ACTIVE_HIGH, (uint32_t)CONFIG_DSP_PUMP_GPIO_MAX_RUNTIME_MS);
        dsp_pump_gpio_port(&s_drv, &s_port);
        s_inited = true;
        ESP_LOGW(TAG, "BENCH pump control signal on GPIO%d, active-%s, max %u ms per run", PIN,
                 ACTIVE_HIGH ? "high" : "low", (unsigned)CONFIG_DSP_PUMP_GPIO_MAX_RUNTIME_MS);
    }
    return &s_port;
}
#endif /* CONFIG_DSP_PUMP_GPIO */
