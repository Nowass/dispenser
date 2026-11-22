#include "dsp_esp_battery.h"

#include <stddef.h>

#include "sdkconfig.h"

#if CONFIG_DSP_BATTERY_ADC_GPIO >= 0
/*
 * One measurement = DSP_PLAT_BATT_DISCARD conversions thrown away (the sampling capacitor settles
 * through the ~500 kOhm divider source), then DSP_PLAT_BATT_SAMPLES calibrated conversions back to
 * back, reduced by dsp_plat_batt_compute (trimmed mean, divider, credible window, spread). No
 * delays and no continuous sampling: about 20 oneshot conversions (~1 ms) per call. The unit is
 * created on the first call of a boot (ADC1, 12 dB attenuation: about 0-2.5 V at the pad on the
 * ESP32-C3, the midpoint of a 1S cell is 1.5-2.1 V) and kept until deep sleep powers it down.
 * The domain calls this from the application task only, never while the pump runs.
 */
#include "dsp_platform_logic.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#if CONFIG_DSP_BATTERY_ADC_GPIO > 4 || CONFIG_DSP_BATTERY_ADC_GPIO == 2
#error "CONFIG_DSP_BATTERY_ADC_GPIO must be an ESP32-C3 ADC1 pad (GPIO0-GPIO4) other than the strapping pin GPIO2"
#endif
#if CONFIG_DSP_BATTERY_ADC_GPIO == CONFIG_DSP_BUTTON_WAKE_GPIO
#error "CONFIG_DSP_BATTERY_ADC_GPIO must differ from the wake button GPIO"
#endif
#if defined(CONFIG_DSP_PUMP_GPIO_NUM) && CONFIG_DSP_BATTERY_ADC_GPIO == CONFIG_DSP_PUMP_GPIO_NUM
#error "CONFIG_DSP_BATTERY_ADC_GPIO must differ from the pump control GPIO"
#endif
#if !ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
#error "the battery adapter expects the curve-fitting ADC calibration of the ESP32-C3"
#endif

#ifdef CONFIG_DSP_PUMP_GPIO_NUM
#define BATT_PUMP_GPIO CONFIG_DSP_PUMP_GPIO_NUM
#else
#define BATT_PUMP_GPIO (-1)
#endif

static const char *TAG = "dsp_battery";

static bool s_tried;
static bool s_unit_ok;
static bool s_cali_ok;
static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

static void batt_init(void)
{
    s_tried = true;
    if (dsp_plat_batt_pin_check(CONFIG_DSP_BATTERY_ADC_GPIO, CONFIG_DSP_BUTTON_WAKE_GPIO, BATT_PUMP_GPIO) !=
        DSP_PLAT_BATT_PIN_OK) {
        ESP_LOGE(TAG, "GPIO%d is not a usable battery ADC pin", CONFIG_DSP_BATTERY_ADC_GPIO);
        return;
    }
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_DSP_BATTERY_ADC_GPIO, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d has no ADC1 channel", CONFIG_DSP_BATTERY_ADC_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_new_unit(&ucfg, &s_unit) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 unit unavailable");
        return;
    }
    if (adc_oneshot_config_channel(s_unit, s_chan, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 channel %d setup failed", (int)s_chan);
        (void)adc_oneshot_del_unit(s_unit);
        return;
    }
    s_unit_ok = true;
    adc_cali_curve_fitting_config_t kcfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t e = adc_cali_create_scheme_curve_fitting(&kcfg, &s_cali);
    s_cali_ok = e == ESP_OK;
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "no ADC calibration (esp_err 0x%x): battery voltage unavailable", (unsigned)e);
    }
}

static dsp_batt_sample_t batt_measure(void *ctx)
{
    (void)ctx;
    dsp_batt_sample_t out = {DSP_BATT_ADC_FAILED, 0};
    if (!s_tried) {
        batt_init();
    }
    if (!s_unit_ok) {
        return out;
    }
    if (!s_cali_ok) {
        out.state = DSP_BATT_NO_CALIBRATION;
        return out;
    }
    int raw = 0;
    for (unsigned i = 0; i < DSP_PLAT_BATT_DISCARD; i++) {
        if (adc_oneshot_read(s_unit, s_chan, &raw) != ESP_OK) {
            ESP_LOGW(TAG, "battery: ADC conversion failed");
            return out;
        }
    }
    int mv[DSP_PLAT_BATT_SAMPLES];
    for (unsigned i = 0; i < DSP_PLAT_BATT_SAMPLES; i++) {
        if (adc_oneshot_read(s_unit, s_chan, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(s_cali, raw, &mv[i]) != ESP_OK) {
            ESP_LOGW(TAG, "battery: ADC conversion failed");
            return out;
        }
    }
    dsp_plat_batt_result_t r = dsp_plat_batt_compute(mv, DSP_PLAT_BATT_SAMPLES,
                                                     (uint32_t)CONFIG_DSP_BATTERY_DIVIDER_X1000);
    ESP_LOGI(TAG, "battery: %s %u mV (pad %u mV, spread %u mV, divider x%u/1000)",
             dsp_plat_batt_state_name(r.state), (unsigned)r.battery_mv, (unsigned)r.adc_mv,
             (unsigned)r.spread_mv, (unsigned)CONFIG_DSP_BATTERY_DIVIDER_X1000);
    out.state = r.state;
    out.battery_mv = r.battery_mv;
    return out;
}

static const dsp_battery_port_t s_port = {NULL, batt_measure};

const dsp_battery_port_t *dsp_esp_battery_port(void)
{
    return &s_port;
}

#else

const dsp_battery_port_t *dsp_esp_battery_port(void)
{
    return NULL; /* no battery pin in this build */
}

#endif
