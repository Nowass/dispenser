/*
 * Dispenser V2 firmware entry point (milestone M0.2 skeleton).
 *
 * main/ is the composition root: the only place that combines ESP-IDF APIs with
 * domain components. In this milestone it ONLY logs firmware identification.
 *
 * Optional diagnostics (Kconfig, all default off): read-only storage check, time sync,
 * HTTPS transport (M1.7; never posts an event).
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

#if CONFIG_DSP_TIME_SYNC_DIAGNOSTIC || CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC
#include "dsp_esp_time.h"
#include "dsp_time_service.h"
#endif

#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC
#include "dsp_esp_notify.h"
#include "dsp_https_transport.h"
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

#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC
/* Diagnostic only (Kconfig, default off). Verifies the M1.7 platform path on a bench
 * device: credentials from the developer-provisioned "nvs_sec" partition (read-only), one
 * forced time sync, then - only with a trusted clock (D4) - one unauthenticated TLS GET of
 * the health URL with the full certificate bundle. It never posts a notification, never
 * touches the dispenser NVS namespace and logs outcomes only (no SSID, passphrase or
 * token). Without provisioned credentials the radio is never started. */
static void notify_https_diagnostic(void)
{
    bool secrets = dsp_esp_secrets_init();
    uint64_t device_id = 0;
    bool have_id = dsp_esp_device_id(&device_id);
    static dsp_monotonic_t mono;
    dsp_esp_monotonic(&mono);
    bool url_ok = dsp_esp_http_configure(CONFIG_DSP_NOTIFY_URL);
    static dsp_https_transport_t https;
    bool transport = have_id && dsp_https_init(&https, dsp_esp_http_port(), dsp_esp_api_token(),
                                               &mono, device_id, 0);
    bool token = transport && dsp_https_ready(&https);
    ESP_LOGI(TAG, "https diagnostic: nvs_sec=%d device_id=%d url=%d token_provisioned=%d",
             (int)secrets, (int)have_id, (int)url_ok, (int)token);

    dsp_clock_port_t port;
    dsp_esp_clock_port(&port);
    dsp_time_meta_t meta = {0, 0, 0, 0, 0};
    dsp_time_retained_t retained;
    bool have_retained = dsp_esp_clock_load_retained(&retained);
    static dsp_timekeeper_t tk;
    if (!dsp_timekeeper_init(&tk, &meta, 24, have_retained ? &retained : NULL)) {
        ESP_LOGW(TAG, "https diagnostic: timekeeper init failed");
        return;
    }
    dsp_timekeeper_evaluate(&tk, dsp_esp_clock_reading(meta.last_sync_utc));
    static dsp_wifi_manager_t wifi;
    dsp_wifi_config_t wcfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    const dsp_wifi_credentials_provider_t *creds =
        secrets ? dsp_esp_wifi_credentials() : &DSP_WIFI_NO_CREDENTIALS;
    if (dsp_wifi_init(&wifi, dsp_esp_wifi_driver(), creds, &wcfg) != DSP_NET_OK) {
        ESP_LOGW(TAG, "https diagnostic: Wi-Fi manager init failed");
        return;
    }
    dsp_time_service_t svc = {&tk, &wifi, dsp_esp_sntp_source(), &port, NULL, 0};
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, true);
    dsp_esp_clock_save_retained(&tk.retained);
    bool trusted = tk.trust != DSP_TRUST_UNKNOWN;
    if (r.wifi == DSP_NET_OK && dsp_wifi_state(&wifi) == DSP_WIFI_CONNECTED && trusted) {
        dsp_http_outcome_t o;
        bool probed = dsp_esp_http_probe(CONFIG_DSP_NOTIFY_HEALTH_URL, &o);
        ESP_LOGI(TAG, "https diagnostic: probe=%d phase=%d status=%u", (int)probed, (int)o.phase,
                 (unsigned)o.status);
    } else {
        ESP_LOGI(TAG, "https diagnostic: no TLS probe (wifi=%d trusted=%d): nothing sent",
                 (int)r.wifi, (int)trusted);
    }
    dsp_wifi_shutdown(&wifi);
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
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC
    notify_https_diagnostic();
#endif

    /* Returning from app_main is fine: the main task is deleted, the scheduler keeps
     * running idle. Deep sleep and wake-up handling come in a later milestone. */
}
