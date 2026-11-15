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

#if CONFIG_DSP_STORAGE_SMOKE_TEST
#include "dsp_nvs_kv.h"
#include "dsp_storage.h"
#endif

#if CONFIG_DSP_TIME_SYNC_DIAGNOSTIC
#include "dsp_esp_time.h"
#include "dsp_time_service.h"
#endif

static const char *TAG = "dispenser";

#if CONFIG_DSP_STORAGE_SMOKE_TEST
/* Diagnostic only (Kconfig, default off): the check opens the namespace read-only, does
 * not provision, writes no dispenser keys and requests no erase. ESP-IDF's own NVS
 * initialization (nvs_flash_init_partition) may still write to flash internally (blank
 * page setup, completing or reverting interrupted operations, recovery erasing/rewriting
 * pages); that is library behavior outside this code. The storage context (~8 KB) is
 * static because the main task stack is small; main/ is the composition root. */
static dsp_storage_t s_storage;

static void storage_smoke_test(void)
{
    dsp_nvs_kv_t nvs;
    dsp_kv_t kv;
    dsp_nvs_status_t ns = dsp_nvs_kv_open(&nvs, DSP_NVS_DEFAULT_PARTITION, DSP_NVS_NAMESPACE,
                                          true, &kv);
    if (ns != DSP_NVS_OK) {
        ESP_LOGW(TAG, "storage check: NVS unavailable (status %d, esp_err 0x%x), no erase requested",
                 (int)ns, (unsigned)nvs.last_err);
        return;
    }
    dsp_storage_status_t st = dsp_storage_open(&s_storage, &kv, &DSP_TZ_EUROPE_PRAGUE, false);
    ESP_LOGI(TAG, "storage check (read-only): status %d", (int)st);
    dsp_nvs_kv_close(&nvs);
}
#endif

#if CONFIG_DSP_TIME_SYNC_DIAGNOSTIC
/* Diagnostic only (Kconfig, default off). No NVS access: the timekeeper starts from
 * "never synced". With the "no credentials" provider the Wi-Fi radio is never started. */
static void time_sync_diagnostic(void)
{
    dsp_clock_port_t port;
    dsp_esp_clock_port(&port);
    dsp_time_meta_t meta = {0, 0, 0, 0, 0};
    dsp_time_retained_t retained;
    bool have_retained = dsp_esp_clock_load_retained(&retained);
    static dsp_timekeeper_t tk;
    if (!dsp_timekeeper_init(&tk, &meta, 24, have_retained ? &retained : NULL)) {
        ESP_LOGW(TAG, "time diagnostic: timekeeper init failed");
        return;
    }
    dsp_timekeeper_evaluate(&tk, dsp_esp_clock_reading(meta.last_sync_utc));
    static dsp_wifi_manager_t wifi;
    dsp_wifi_config_t wcfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    if (dsp_wifi_init(&wifi, dsp_esp_wifi_driver(), &DSP_WIFI_NO_CREDENTIALS, &wcfg) != DSP_NET_OK) {
        ESP_LOGW(TAG, "time diagnostic: Wi-Fi manager init failed");
        return;
    }
    dsp_time_service_t svc = {&tk, &wifi, dsp_esp_sntp_source(), &port, NULL, 0};
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    dsp_esp_clock_save_retained(&tk.retained);
    ESP_LOGI(TAG, "time diagnostic: trust=%d reason=%d wifi=%d sync=%d (Wi-Fi state %d)",
             (int)tk.trust, (int)tk.reason, (int)r.wifi, (int)r.decision.status,
             (int)dsp_wifi_state(&wifi));
}
#endif

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
#if CONFIG_DSP_STORAGE_SMOKE_TEST
    storage_smoke_test();
#endif
#if CONFIG_DSP_TIME_SYNC_DIAGNOSTIC
    time_sync_diagnostic();
#endif

    /* Returning from app_main is fine: the main task is deleted, the scheduler keeps
     * running idle. Deep sleep and wake-up handling come in a later milestone. */
}
