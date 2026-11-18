/*
 * Dispenser V2 firmware entry point (milestone M0.2 skeleton).
 *
 * main/ is the composition root: the only place that combines ESP-IDF APIs with
 * domain components. In this milestone it ONLY logs firmware identification.
 *
 * Optional diagnostics (Kconfig, all default off): read-only storage check, time sync,
 * HTTPS transport (M1.7; never posts an event).
 *
 * Optional application runtime (M1.9c, CONFIG_DSP_APP_RUNTIME, default off): the M1.9b
 * application cycle with a SIMULATED pump (no GPIO) and deep sleep; see app_runtime().
 *
 * Deliberately absent from the default image (separate milestones, pending hardware review):
 *   - no GPIO configuration of any kind (pump/motor, button, LED),
 *   - no Bluetooth or Wi-Fi start,
 *   - no power-management or deep-sleep changes,
 *   - no NVS access, no dosing.
 */

#include "dsp_identity.h"

#include <string.h>

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

#if CONFIG_DSP_APP_RUNTIME
#include "dsp_app.h"
#include "dsp_esp_app.h"
#include "dsp_esp_notify.h"
#include "dsp_esp_time.h"
#include "dsp_nvs_kv.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_DSP_BLE_SECURITY
#include "dsp_blesec.h"
#include "dsp_esp_rng.h"
#endif
#if CONFIG_DSP_BLE_TRANSPORT
#include "dsp_esp_ble.h"
#endif
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

#if CONFIG_DSP_APP_RUNTIME
/* ---- Application runtime (opt-in) ---------------------------------------------------
 * Single owner, run to completion: one static task runs dsp_app_run_wake() once; the wake
 * ends in deep sleep (or the fail-safe reset), and the next wake starts app_main again.
 * All state is static (the task stack holds only call frames). */

/* KV that always fails: an NVS open failure then leads the application into DEGRADED. */
static dsp_kv_status_t failing_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    (void)key;
    (void)buf;
    (void)cap;
    (void)len;
    return DSP_KV_ERR_READ;
}
static dsp_kv_status_t failing_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)key;
    (void)data;
    (void)len;
    return DSP_KV_ERR_WRITE;
}
static dsp_kv_status_t failing_commit(void *ctx)
{
    (void)ctx;
    return DSP_KV_ERR_WRITE;
}

static dsp_app_t s_app;
static dsp_app_config_t s_cfg;
static dsp_nvs_kv_t s_nvs;
static dsp_kv_t s_kv;
static dsp_power_port_t s_power;
static dsp_clock_port_t s_clock;
static dsp_time_port_t s_time;
static dsp_monotonic_t s_mono;
static dsp_app_sync_t s_sync;
#if CONFIG_DSP_APP_CONNECTIVITY
static dsp_wifi_manager_t s_wifi;
static dsp_https_transport_t s_https;
#endif
#if CONFIG_DSP_BLE_SECURITY
static dsp_blesec_t s_blesec;          /* build-only until M1.10c.2b */
static dsp_app_security_t s_security;
#endif
#if CONFIG_DSP_BLE_TRANSPORT
static dsp_ble_port_t s_ble;           /* build-only until M1.10c.2b and M1.10d.2 */
static dsp_app_identity_t s_identity;  /* Info record and advertising payload */

/* "major.minor.patch[-...]" -> numbers (0 for a missing part). */
static void parse_version(const char *v, dsp_app_identity_t *id)
{
    unsigned part[3] = {0, 0, 0};
    for (int i = 0; i < 3 && *v != '\0'; i++) {
        while (*v >= '0' && *v <= '9') {
            part[i] = part[i] * 10u + (unsigned)(*v - '0');
            if (part[i] > 0xFFFFu) {
                part[i] = 0xFFFFu;
            }
            v++;
        }
        if (*v != '.') {
            break;
        }
        v++;
    }
    id->fw_major = (uint8_t)(part[0] > 0xFFu ? 0xFFu : part[0]);
    id->fw_minor = (uint8_t)(part[1] > 0xFFu ? 0xFFu : part[1]);
    id->fw_patch = (uint16_t)part[2];
}
#endif
static StaticTask_t s_task;
static StackType_t s_stack[CONFIG_DSP_APP_TASK_STACK / sizeof(StackType_t)];

static void app_task(void *arg)
{
    (void)arg;
    dsp_app_report_t rep = dsp_app_run_wake(&s_app, &s_cfg);
    /* Only reached when the sleep port could not run (invalid wiring) or on host-like
     * behavior: never stay awake with unknown state. */
    ESP_LOGE(TAG, "application cycle returned (slept=%d fail_safe=%d): fail-safe",
             (int)rep.slept, (int)rep.fail_safe);
    dsp_esp_fail_safe(rep.shutdown_failures);
}

static void app_runtime(void)
{
    /* SAFE_OFF before anything else (the application repeats it as its first stage). */
    const dsp_pump_port_t *pump = dsp_esp_pump_sim();
    (void)dsp_dosing_port_safe_off(pump);

    memset(&s_cfg, 0, sizeof s_cfg);
    dsp_nvs_status_t ns = dsp_nvs_kv_open(&s_nvs, DSP_NVS_DEFAULT_PARTITION, DSP_NVS_NAMESPACE,
                                          false, &s_kv);
    if (ns != DSP_NVS_OK) {
        ESP_LOGW(TAG, "app: NVS unavailable (status %d): DEGRADED", (int)ns);
        s_kv.ctx = NULL;
        s_kv.get = failing_get;
        s_kv.set = failing_set;
        s_kv.commit = failing_commit;
    }
    dsp_esp_clock_port(&s_clock);
    dsp_esp_time_port(&s_time);
    dsp_esp_monotonic(&s_mono);
    dsp_esp_app_sync(&s_sync);
#if CONFIG_DSP_APP_CONNECTIVITY
    dsp_esp_power_port(&s_power, pump, dsp_esp_wifi_driver()); /* fail-safe also stops Wi-Fi */
#else
    dsp_esp_power_port(&s_power, pump, NULL); /* no radio is ever started */
#endif
    s_cfg.kv = &s_kv;
    s_cfg.tz = &DSP_TZ_EUROPE_PRAGUE;
    s_cfg.pump = pump;
    s_cfg.hw_max_runtime_ms = (uint32_t)CONFIG_DSP_APP_SIM_HW_MAX_RUNTIME_MS;
    s_cfg.power = &s_power;
    s_cfg.clock = &s_clock;
    s_cfg.time = &s_time;
    s_cfg.mono = &s_mono;
    s_cfg.sync = &s_sync;
    s_cfg.ble = NULL; /* service mode only with CONFIG_DSP_BLE_TRANSPORT (below) */
    s_cfg.random = esp_random();
    s_cfg.max_sleep_s = CONFIG_DSP_APP_MAX_SLEEP_S;
#if CONFIG_DSP_BLE_SECURITY
    /* C2: the libsodium RNG implementation is registered before any libsodium use; sodium_init
     * runs only once the hardware entropy source is active (dsp_esp_rng_ready). Without the
     * registration no security layer exists and no ownership authorization can be issued. */
    uint64_t did = 0;
    (void)dsp_esp_secrets_init();
    if (dsp_esp_rng_register() && dsp_esp_device_id(&did)) {
        dsp_blesec_config_t bc;
        memset(&bc, 0, sizeof bc);
        bc.entropy_ready = dsp_esp_rng_ready;
        bc.random = dsp_esp_rng_bytes;
        bc.read_setup_code = dsp_esp_setup_code_read;
        for (int i = 0; i < 6; i++) {
            bc.device_id[i] = (uint8_t)(did >> (8 * (5 - i)));
        }
        dsp_blesec_init(&s_blesec, &bc, &s_security);
        s_cfg.security = &s_security;
#if CONFIG_DSP_BLE_TRANSPORT
        /* M1.10d.1: the NimBLE transport exists only together with the security layer (never a
         * BLE service without it). The window still opens only on a presence wake. */
        memcpy(s_identity.device_id, bc.device_id, sizeof s_identity.device_id);
        parse_version(esp_app_get_description()->version, &s_identity);
        s_cfg.identity = &s_identity;
        if (dsp_esp_ble_port(&s_app, &s_ble)) {
            s_cfg.ble = &s_ble;
        } else {
            ESP_LOGW(TAG, "app: BLE transport unavailable");
        }
#if CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW
        s_cfg.service_on_power_on = true; /* T7: build-only bring-up, refused with production */
        ESP_LOGW(TAG, "app: BRING-UP build: a power-on wake opens a BLE service window");
#endif
#endif
    } else {
        ESP_LOGW(TAG, "app: security layer not registered (RNG registration or device ID failed)");
    }
#endif
#if CONFIG_DSP_APP_CONNECTIVITY
    bool secrets = dsp_esp_secrets_init();
    uint64_t device_id = 0;
    dsp_wifi_config_t wcfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    const dsp_wifi_credentials_provider_t *creds =
        secrets ? dsp_esp_wifi_credentials() : &DSP_WIFI_NO_CREDENTIALS;
    /* CONFIG_DSP_APP_NOTIFY_URL is checked at build time (dsp_endpoint_guard.cmake): explicit,
     * https only, production only with CONFIG_DSP_APP_ALLOW_PRODUCTION. Never DSP_NOTIFY_URL. */
    if (CONFIG_DSP_APP_NOTIFY_URL[0] != '\0' && dsp_esp_device_id(&device_id) &&
        dsp_esp_http_configure(CONFIG_DSP_APP_NOTIFY_URL) &&
        dsp_wifi_init(&s_wifi, dsp_esp_wifi_driver(), creds, &wcfg) == DSP_NET_OK &&
        dsp_https_init(&s_https, dsp_esp_http_port(), dsp_esp_api_token(), &s_mono, device_id, 0)) {
        s_cfg.wifi = &s_wifi;
        s_cfg.ntp = dsp_esp_sntp_source();
        s_cfg.https = &s_https;
    } else {
        ESP_LOGW(TAG, "app: connectivity unavailable (credentials=%d)", (int)secrets);
    }
#endif
}

/* Last action of app_main: from here on the application task is the single owner. */
static void app_runtime_start(void)
{
    TaskHandle_t t = xTaskCreateStatic(app_task, "dsp_app", CONFIG_DSP_APP_TASK_STACK / sizeof(StackType_t),
                                       NULL, tskIDLE_PRIORITY + 2, s_stack, &s_task);
    if (t == NULL) {
        dsp_esp_fail_safe(0);
    }
}
#endif

void app_main(void)
{
#if CONFIG_DSP_APP_RUNTIME
    app_runtime(); /* SAFE_OFF is its first statement; the task starts at the end */
#endif
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
#if CONFIG_DSP_APP_RUNTIME
    ESP_LOGI(TAG, "M1.9c application runtime: SIMULATED pump (no GPIO), deep sleep");
#else
    ESP_LOGI(TAG, "M0.2 skeleton: no GPIO, radio, sleep or dosing activity");
#endif
#if CONFIG_DSP_STORAGE_SMOKE_TEST
    storage_smoke_test();
#endif
#if CONFIG_DSP_TIME_SYNC_DIAGNOSTIC
    time_sync_diagnostic();
#endif
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC
    notify_https_diagnostic();
#endif

#if CONFIG_DSP_APP_RUNTIME
    app_runtime_start();
#endif
    /* Returning from app_main is fine: the main task is deleted, the scheduler keeps
     * running (in the runtime build the application task then owns everything). */
}
