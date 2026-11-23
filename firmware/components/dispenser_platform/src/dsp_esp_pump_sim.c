#include "dsp_esp_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * SIMULATED pump (M1.9c): no GPIO is configured or driven anywhere in this file, so nothing
 * can energize a motor. run() waits for the commanded runtime (the caller has already
 * validated it against every limit and the hardware maximum) and counts. The real driver
 * needs the hardware review: gate pin (no strapping pin), polarity, external pull-down,
 * hardware cutoff independent of the CPU, watchdog timeout below the cutoff, behavior at
 * reset/brownout/deep sleep (see firmware/README.md and HARDWARE_ACCEPTANCE.md).
 */

static const char *TAG = "dsp_pump_sim";
static dsp_esp_pump_sim_stats_t s_stats;

static dsp_pump_result_t sim_run(void *ctx, uint32_t runtime_ms)
{
    (void)ctx;
    s_stats.running = true;
    s_stats.runs++;
    ESP_LOGW(TAG, "SIMULATED pump run %u ms (no GPIO, nothing actuated)", (unsigned)runtime_ms);
    vTaskDelay(pdMS_TO_TICKS(runtime_ms));
    s_stats.total_ms += runtime_ms;
    s_stats.running = false;
    return DSP_PUMP_OK;
}

static void sim_force_off(void *ctx)
{
    (void)ctx;
    s_stats.force_offs++;
    s_stats.running = false;
}

static bool sim_is_off(void *ctx)
{
    (void)ctx;
    return !s_stats.running;
}

static const dsp_pump_port_t SIM = {NULL, sim_run, sim_force_off, sim_is_off, NULL, NULL, NULL, NULL}; /* no priming */

const dsp_pump_port_t *dsp_esp_pump_sim(void)
{
    return &SIM;
}

dsp_esp_pump_sim_stats_t dsp_esp_pump_sim_stats(void)
{
    return s_stats;
}
