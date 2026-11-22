#include "dsp_battery.h"

dsp_batt_sample_t dsp_batt_sanitize(dsp_batt_sample_t s)
{
    if (s.state > DSP_BATT_STATE_MAX) {
        s.state = DSP_BATT_ADC_FAILED;
    }
    if (s.state == DSP_BATT_VALID && (s.battery_mv < DSP_BATT_MIN_MV || s.battery_mv > DSP_BATT_MAX_MV)) {
        s.state = DSP_BATT_OUT_OF_RANGE;
    }
    if (s.state != DSP_BATT_VALID) {
        s.battery_mv = 0;
    }
    return s;
}
