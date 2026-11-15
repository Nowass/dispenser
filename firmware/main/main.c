/*
 * Dispenser V2 firmware entry point (milestone M0.2 skeleton).
 *
 * main/ is the composition root: the only place that combines ESP-IDF APIs with
 * domain components. In this milestone it ONLY logs firmware identification.
 *
 * Deliberately absent (separate milestones, pending hardware review):
 *   - no GPIO configuration of any kind (pump/motor, button, LED),
 *   - no Bluetooth or Wi-Fi start,
 *   - no power-management or deep-sleep changes,
 *   - no NVS access, no dosing.
 */

#include "dsp_identity.h"

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "dispenser";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    const dsp_identity_t id = {
        .fw_version = app->version,
        .target = CONFIG_IDF_TARGET,
        .sdk_version = esp_get_idf_version(),
    };

    char line[128];
    dsp_identity_result_t rc = dsp_identity_format(&id, line, sizeof line);
    if (rc == DSP_IDENTITY_OK || rc == DSP_IDENTITY_ERR_TRUNCATED) {
        ESP_LOGI(TAG, "%s", line);
    } else {
        ESP_LOGW(TAG, "firmware identification unavailable (rc=%d)", (int)rc);
    }
    ESP_LOGI(TAG, "project=%s build=%s %s", app->project_name, app->date, app->time);
    ESP_LOGI(TAG, "M0.2 skeleton: no GPIO, radio, sleep or dosing activity");

    /* Returning from app_main is fine: the main task is deleted, the scheduler keeps
     * running idle. Deep sleep and wake-up handling come in a later milestone. */
}
