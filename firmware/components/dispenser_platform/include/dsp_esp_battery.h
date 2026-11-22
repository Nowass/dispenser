#ifndef DSP_ESP_BATTERY_H
#define DSP_ESP_BATTERY_H

/*
 * Battery voltage adapter (ESP-IDF ADC oneshot driver + curve-fitting calibration) for the
 * domain's battery port (dsp_battery.h). Pin: CONFIG_DSP_BATTERY_ADC_GPIO (-1 = none: the default
 * image has no battery telemetry and returns NULL); divider ratio: CONFIG_DSP_BATTERY_DIVIDER_X1000.
 * The MVP circuit (sdkconfig.battery.defaults): XIAO ESP32-C3 BAT+ -> 1 MOhm -> D2 = GPIO4
 * (ADC1_CH4) -> 1 MOhm -> GND, optional 100 nF from GPIO4 to GND. Details: firmware/README.md
 * "Battery voltage".
 */

#include "dsp_battery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The port (static), or NULL when the build has no battery pin. */
const dsp_battery_port_t *dsp_esp_battery_port(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_BATTERY_H */
