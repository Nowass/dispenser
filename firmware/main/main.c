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
 * application cycle with a SIMULATED pump (no GPIO) and deep sleep; see app_runtime(). The bench
 * opt-in CONFIG_DSP_PUMP_GPIO (default off) replaces the simulation with a GPIO control signal
 * (dsp_esp_pump_gpio; no load connected at this stage).
 *
 * Optional RNG-gate diagnostic image (M1.10c.2b evidence, CONFIG_DSP_RNG_GATE_DIAGNOSTIC, default
 * off, sdkconfig.rnggate.defaults): BT controller only, no host stack; see rng_gate_task().
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
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC_BATTERY_EVENT
#include "dsp_esp_battery.h"
#include "dsp_notify.h"
#include "esp_random.h"
#endif

#if CONFIG_DSP_APP_RUNTIME
#include "dsp_app.h"
#include "dsp_esp_app.h"
#include "dsp_esp_battery.h"
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

#if CONFIG_DSP_RNG_GATE_DIAGNOSTIC
#include <stdio.h>

#include <sodium.h>

#include "dsp_esp_rng.h"
#include "esp_bt.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_DSP_RNG_GATE_PSK_VECTORS
#include "dsp_auth.h" /* dsp_secure_zero */
#include "dsp_secderive.h"
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
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC_BATTERY_EVENT
/* One BATTERY_LOW test event through the production transport, then the identical retry. */
static const char *tx_name(dsp_tx_result_t r)
{
    return r == DSP_TX_ACK ? "ACK" : r == DSP_TX_UNCERTAIN ? "UNCERTAIN" : r == DSP_TX_REJECTED ? "REJECTED" : "FAILED";
}
static void battery_event_diagnostic(dsp_https_transport_t *https, uint64_t device_id, dsp_batt_sample_t b,
                                     dsp_utc_t now)
{
    if (b.state != DSP_BATT_VALID) {
        ESP_LOGW(TAG, "battery event diagnostic: no VALID battery measurement (state %u): nothing sent",
                 (unsigned)b.state);
        return;
    }
    dsp_note_t note;
    memset(&note, 0, sizeof note);
    note.seq = 1;
    note.type = DSP_NOTE_BATTERY_LOW;
    note.severity = DSP_SEV_WARNING;
    note.condition = DSP_COND_BATTERY_LOW;
    note.flags = DSP_NOTE_F_TIME_VALID;
    note.created_at = now;
    note.value = b.battery_mv;
    note.value2 = 0;
    uint32_t epoch = 0xD1A60000u | (esp_random() & 0xFFFFu);
    if ((epoch & 0xFFFFu) == 0) {
        epoch |= 1u;
    }
    char id[DSP_NOTIFY_ID_BUF_LEN];
    (void)dsp_notify_format_id(device_id, epoch, note.seq, id, sizeof id);
    if (!dsp_https_begin(https, 60000u)) {
        ESP_LOGW(TAG, "battery event diagnostic: HTTPS session could not start: nothing sent");
        return;
    }
    dsp_notify_transport_t tx = dsp_https_transport(https);
    for (uint16_t attempt = 1; attempt <= 2; attempt++) {
        note.attempts = attempt;
        dsp_tx_detail_t d;
        memset(&d, 0, sizeof d);
        dsp_tx_result_t r = tx.send(tx.ctx, &note, epoch, &d);
        ESP_LOGI(TAG, "battery event diagnostic: %s attempt %u: %s http=%u error=%u diag=0x%02x phase=%u (%u mV)",
                 id, (unsigned)attempt, tx_name(r), (unsigned)d.http_status, (unsigned)d.error,
                 (unsigned)d.diag, (unsigned)d.phase, (unsigned)note.value);
        if (r != DSP_TX_ACK && r != DSP_TX_UNCERTAIN) {
            break; /* FAILED / REJECTED: no blind retry */
        }
    }
    dsp_https_end(https);
}
#endif

static void notify_https_diagnostic(void)
{
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC_BATTERY_EVENT
    /* measured first, before the radio starts (as the application does) */
    const dsp_battery_port_t *bp = dsp_esp_battery_port();
    dsp_batt_sample_t batt = {DSP_BATT_NOT_MEASURED, 0};
    if (bp != NULL && bp->measure != NULL) {
        batt = dsp_batt_sanitize(bp->measure(bp->ctx));
    }
#endif
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
#if CONFIG_DSP_NOTIFY_HTTPS_DIAGNOSTIC_BATTERY_EVENT
        if (token) {
            battery_event_diagnostic(&https, device_id, batt, port.now(port.ctx));
        } else {
            ESP_LOGW(TAG, "battery event diagnostic: no API token provisioned: nothing sent");
        }
#endif
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
#if CONFIG_DSP_APP_CONNECTIVITY || CONFIG_DSP_APP_WIFI_TIME
static dsp_wifi_manager_t s_wifi;
#endif
#if CONFIG_DSP_APP_CONNECTIVITY
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
#if CONFIG_DSP_PUMP_GPIO
    const dsp_pump_port_t *pump = dsp_esp_pump_gpio(); /* BENCH: GPIO control signal, no load */
#else
    const dsp_pump_port_t *pump = dsp_esp_pump_sim();
#endif
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
#if CONFIG_DSP_APP_CONNECTIVITY || CONFIG_DSP_APP_WIFI_TIME
    dsp_esp_power_port(&s_power, pump, dsp_esp_wifi_driver()); /* fail-safe also stops Wi-Fi */
#else
    dsp_esp_power_port(&s_power, pump, NULL); /* no radio is ever started */
#endif
    s_cfg.kv = &s_kv;
    s_cfg.tz = &DSP_TZ_EUROPE_PRAGUE;
    s_cfg.pump = pump;
#if CONFIG_DSP_PUMP_GPIO
    s_cfg.hw_max_runtime_ms = (uint32_t)CONFIG_DSP_PUMP_GPIO_MAX_RUNTIME_MS;
    s_cfg.event_pump_flags = DSP_EVF_PUMP_BENCH_GPIO; /* history: control signal only, no liquid verified */
#else
    s_cfg.hw_max_runtime_ms = (uint32_t)CONFIG_DSP_APP_SIM_HW_MAX_RUNTIME_MS;
    s_cfg.event_pump_flags = DSP_EVF_PUMP_SIMULATED; /* history: nothing was actuated */
#endif
    s_cfg.power = &s_power;
    s_cfg.clock = &s_clock;
    s_cfg.time = &s_time;
    s_cfg.mono = &s_mono;
    s_cfg.sync = &s_sync;
    s_cfg.ble = NULL; /* service mode only with CONFIG_DSP_BLE_TRANSPORT (below) */
    s_cfg.random = esp_random();
    s_cfg.max_sleep_s = CONFIG_DSP_APP_MAX_SLEEP_S;
    s_cfg.battery = dsp_esp_battery_port(); /* NULL unless CONFIG_DSP_BATTERY_ADC_GPIO >= 0 */
#if CONFIG_DSP_BATTERY_ADC_GPIO >= 0
    _Static_assert(CONFIG_DSP_BATTERY_LOW_MV < CONFIG_DSP_BATTERY_RECOVER_MV,
                   "CONFIG_DSP_BATTERY_RECOVER_MV must be greater than CONFIG_DSP_BATTERY_LOW_MV");
    s_cfg.battery_low_mv = (uint16_t)CONFIG_DSP_BATTERY_LOW_MV;
    s_cfg.battery_recover_mv = (uint16_t)CONFIG_DSP_BATTERY_RECOVER_MV;
#endif
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
#if CONFIG_DSP_APP_WIFI_TIME
    /* Bench MVP: Wi-Fi/SNTP time synchronization only (no HTTP client, no notification delivery,
     * no endpoint, no API token). dsp_connectivity runs time-only sessions when https is NULL. */
    bool wifi_secrets = dsp_esp_secrets_init();
    dsp_wifi_config_t wifi_cfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    const dsp_wifi_credentials_provider_t *wifi_creds =
        wifi_secrets ? dsp_esp_wifi_credentials() : &DSP_WIFI_NO_CREDENTIALS;
    if (dsp_wifi_init(&s_wifi, dsp_esp_wifi_driver(), wifi_creds, &wifi_cfg) == DSP_NET_OK) {
        s_cfg.wifi = &s_wifi;
        s_cfg.ntp = dsp_esp_sntp_source();
        s_cfg.https = NULL;
        ESP_LOGI(TAG, "app: Wi-Fi time sync only (credentials=%d, attempts %d x %u ms, no HTTPS)",
                 (int)wifi_secrets, (int)wifi_cfg.max_attempts, (unsigned)wifi_cfg.connect_timeout_ms);
    } else {
        ESP_LOGW(TAG, "app: Wi-Fi time sync unavailable (credentials=%d)", (int)wifi_secrets);
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

#if CONFIG_DSP_RNG_GATE_DIAGNOSTIC
/* ---- RNG-gate diagnostic image (M1.10c.2b R1, R2, R3, R5, R6) ------------------------
 * Design: docs/ANDROID_BLE_DESIGN.md Appendix B; record sheet: HARDWARE_ACCEPTANCE.md 8a.
 * Runs once per boot in its own static task, then idles. The Bluetooth controller is the only
 * radio component (CONFIG_BT_CONTROLLER_ONLY: no host, so no GATT, advertising, scanning or
 * connections can exist); it is enabled only to make the RF entropy source of esp_fill_random
 * active. No NVS (PHY calibration storage off), no credentials, no eFuse writes, no GPIO output.
 * No libsodium call is ever made while dsp_esp_rng_ready() is false (the adapter would abort).
 *
 * R5 stream format (parsed by tools/rng_capture.py), one line each, per source:
 *   RNGBEGIN source=<name> bytes=<N> block=4096
 *   RNG <name> <index> <crc32 hex8 of the raw block> <Base64 of the raw block (<= 4096 bytes)>
 *   RNGEND source=<name> blocks=<count> bytes=<N> crc32=<crc32 hex8 of the whole stream>
 * Sources, sequentially: "esp_fill_random" (direct), then "randombytes_buf" (libsodium through
 * the registered adapter). The samples are public test data; no secret exists in this image. */

#define RNG_GATE_BLOCK 4096u
#define RNG_GATE_B64_LEN (((RNG_GATE_BLOCK + 2u) / 3u) * 4u)

static uint8_t s_gate_block[RNG_GATE_BLOCK];
static char s_gate_line[64 + RNG_GATE_B64_LEN];
static StaticTask_t s_gate_task;
static StackType_t s_gate_stack[8192 / sizeof(StackType_t)];

/* CRC-32 (IEEE 802.3, reflected, as zlib.crc32): crc = rng_gate_crc32(crc, ...), start 0. */
static uint32_t rng_gate_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* RFC 4648 Base64 with padding; returns the number of characters written (no terminator). */
static size_t rng_gate_b64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = A[(v >> 6) & 63];
        out[o++] = A[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = A[(v >> 6) & 63];
        out[o++] = '=';
    }
    return o;
}

static void rng_gate_put(const char *line, size_t len)
{
    /* one write per line so that log lines of other tasks cannot split a frame */
    fwrite(line, 1, len, stdout);
    fflush(stdout);
}

/* Streams `total` bytes from one source; false if the source refused (entropy inactive). */
static bool rng_gate_stream(const char *source, bool sodium_path, uint32_t total)
{
    int n = snprintf(s_gate_line, sizeof s_gate_line, "RNGBEGIN source=%s bytes=%lu block=%u\n",
                     source, (unsigned long)total, (unsigned)RNG_GATE_BLOCK);
    rng_gate_put(s_gate_line, (size_t)n);
    uint32_t sent = 0;
    uint32_t index = 0;
    uint32_t crc_all = 0;
    while (sent < total) {
        size_t len = total - sent < RNG_GATE_BLOCK ? total - sent : RNG_GATE_BLOCK;
        /* the source must still be active for every block (never sample with RF off) */
        if (!dsp_esp_rng_ready(NULL)) {
            ESP_LOGE(TAG, "RNGGATE R5 %s: entropy source inactive at block %lu: stream aborted",
                     source, (unsigned long)index);
            return false;
        }
        if (sodium_path) {
            randombytes_buf(s_gate_block, len);
        } else {
            esp_fill_random(s_gate_block, len);
        }
        uint32_t crc = rng_gate_crc32(0, s_gate_block, len);
        crc_all = rng_gate_crc32(crc_all, s_gate_block, len);
        n = snprintf(s_gate_line, sizeof s_gate_line, "RNG %s %lu %08lx ", source,
                     (unsigned long)index, (unsigned long)crc);
        size_t pos = (size_t)n;
        pos += rng_gate_b64(s_gate_block, len, s_gate_line + pos);
        s_gate_line[pos++] = '\n';
        rng_gate_put(s_gate_line, pos);
        sent += (uint32_t)len;
        index++;
        vTaskDelay(1); /* the console write may busy-wait: let the idle task (watchdog) run */
    }
    memset(s_gate_block, 0, sizeof s_gate_block);
    n = snprintf(s_gate_line, sizeof s_gate_line, "RNGEND source=%s blocks=%lu bytes=%lu crc32=%08lx\n",
                 source, (unsigned long)index, (unsigned long)sent, (unsigned long)crc_all);
    rng_gate_put(s_gate_line, (size_t)n);
    return true;
}

#if CONFIG_DSP_RNG_GATE_PSK_VECTORS
typedef struct {
    uint8_t is_not;    /* psk_not record: must equal psk and differ from wrong */
    uint8_t purpose;
    uint8_t has_token;
    uint8_t setup[DSP_SETUP_CODE_LEN];
    uint8_t token[DSP_INVITE_TOKEN_LEN];
    uint8_t psk[DSP_PSK_LEN];
    uint8_t wrong[DSP_PSK_LEN];
} dsp_rng_gate_vector_t;
#include "dsp_rng_gate_vectors.h" /* generated from test-vectors/ble_session_v1.txt */

/* R6: every psk / psk_not record through dsp_psk_derive (PSA HKDF). Counts only, never bytes. */
static bool rng_gate_psk_vectors(void)
{
    unsigned pass[2] = {0, 0};
    unsigned fail[2] = {0, 0};
    uint8_t out[DSP_PSK_LEN];
    for (size_t i = 0; i < sizeof DSP_RNG_GATE_VECTORS / sizeof DSP_RNG_GATE_VECTORS[0]; i++) {
        const dsp_rng_gate_vector_t *v = &DSP_RNG_GATE_VECTORS[i];
        bool ok = dsp_psk_derive(v->purpose, v->setup, v->has_token ? v->token : NULL, out) &&
                  memcmp(out, v->psk, DSP_PSK_LEN) == 0 &&
                  (!v->is_not || memcmp(out, v->wrong, DSP_PSK_LEN) != 0);
        dsp_secure_zero(out, sizeof out);
        if (ok) {
            pass[v->is_not]++;
        } else {
            fail[v->is_not]++;
        }
    }
    ESP_LOGI(TAG, "RNGGATE R6 psk pass=%u fail=%u psk_not pass=%u fail=%u (expected %u/%u)", pass[0],
             fail[0], pass[1], fail[1], (unsigned)DSP_RNG_GATE_N_PSK, (unsigned)DSP_RNG_GATE_N_PSK_NOT);
    return fail[0] == 0 && fail[1] == 0 && pass[0] == DSP_RNG_GATE_N_PSK &&
           pass[1] == DSP_RNG_GATE_N_PSK_NOT;
}
#endif

static const char *pf(bool ok)
{
    return ok ? "PASS" : "FAIL";
}

static void rng_gate_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "RNGGATE diagnostic image (M1.10c.2b): BT controller only, no host, no GATT, "
                  "no advertising; not a deployment image");

    /* R1: registration before any libsodium use (no randomness is drawn here) */
    bool reg = dsp_esp_rng_register();
    ESP_LOGI(TAG, "RNGGATE R1 register=%d", (int)reg);

    /* R3 (pre): controller not enabled -> the adapter refuses; no libsodium call in this state */
    esp_bt_controller_status_t st = esp_bt_controller_get_status();
    bool ready_pre = dsp_esp_rng_ready(NULL);
    uint8_t probe[8];
    bool bytes_pre = dsp_esp_rng_bytes(NULL, probe, sizeof probe);
    bool r3_pre = st != ESP_BT_CONTROLLER_STATUS_ENABLED && !ready_pre && !bytes_pre;
    ESP_LOGI(TAG, "RNGGATE R3pre controller_status=%d ready=%d bytes=%d", (int)st, (int)ready_pre,
             (int)bytes_pre);

    /* RF on: controller only (BLE mode), no host stack */
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t e_init = esp_bt_controller_init(&cfg);
    esp_err_t e_en = e_init == ESP_OK ? esp_bt_controller_enable(ESP_BT_MODE_BLE) : ESP_FAIL;
    st = esp_bt_controller_get_status();
    bool ready_on = dsp_esp_rng_ready(NULL); /* sodium_init happens here, on the first ready call */
    ESP_LOGI(TAG, "RNGGATE controller init=0x%x enable=0x%x status=%d ready=%d", (unsigned)e_init,
             (unsigned)e_en, (int)st, (int)ready_on);

    const char *name = dsp_esp_rng_name();
    bool before = dsp_esp_rng_registered_before_init();
    bool r1 = reg && ready_on && before && strcmp(name, "dsp_esp32c3_hw") == 0;
    ESP_LOGI(TAG, "RNGGATE R1 name=%s registered_before_init=%d", name, (int)before);

    /* R2: libsodium requests go through the adapter (call counter increases) */
    bool r2 = false;
    uint32_t c0 = dsp_esp_rng_calls();
    uint32_t c1 = c0;
    if (ready_on) {
        for (int i = 0; i < 4; i++) {
            randombytes_buf(probe, sizeof probe);
        }
        (void)randombytes_random();
        c1 = dsp_esp_rng_calls();
        r2 = c1 >= c0 + 5u;
    }
    memset(probe, 0, sizeof probe); /* not secret; no libsodium call while RF may be off */
    ESP_LOGI(TAG, "RNGGATE R2 calls_before=%lu calls_after=%lu (5 requests)", (unsigned long)c0,
             (unsigned long)c1);

    /* R5: samples with RF on, both paths */
    bool r5a = false;
    bool r5b = false;
    uint32_t cs0 = 0;
    uint32_t cs1 = 0;
    if (ready_on) {
        r5a = rng_gate_stream("esp_fill_random", false, (uint32_t)CONFIG_DSP_RNG_GATE_SAMPLE_BYTES);
        cs0 = dsp_esp_rng_calls();
        r5b = r5a && rng_gate_stream("randombytes_buf", true, (uint32_t)CONFIG_DSP_RNG_GATE_SAMPLE_BYTES);
        cs1 = dsp_esp_rng_calls();
    }
    ESP_LOGI(TAG, "RNGGATE R5 esp_fill_random=%d randombytes_buf=%d adapter_calls_during_sodium_pass=%lu",
             (int)r5a, (int)r5b, (unsigned long)(cs1 - cs0));

#if CONFIG_DSP_RNG_GATE_PSK_VECTORS
    bool r6 = ready_on && rng_gate_psk_vectors();
    const char *r6s = pf(r6);
#else
    const char *r6s = "SKIP";
#endif

    /* R3 (post): RF off -> the adapter refuses again; no libsodium call after this point */
    esp_err_t e_dis = esp_bt_controller_disable();
    esp_err_t e_deinit = e_dis == ESP_OK ? esp_bt_controller_deinit() : ESP_FAIL;
    st = esp_bt_controller_get_status();
    bool ready_post = dsp_esp_rng_ready(NULL);
    bool bytes_post = dsp_esp_rng_bytes(NULL, s_gate_block, 8);
    bool r3_post = e_dis == ESP_OK && e_deinit == ESP_OK && st != ESP_BT_CONTROLLER_STATUS_ENABLED &&
                   !ready_post && !bytes_post;
    ESP_LOGI(TAG, "RNGGATE R3post disable=0x%x deinit=0x%x controller_status=%d ready=%d bytes=%d",
             (unsigned)e_dis, (unsigned)e_deinit, (int)st, (int)ready_post, (int)bytes_post);

    ESP_LOGI(TAG, "RNGGATE SUMMARY R1=%s R2=%s R3pre=%s R3post=%s R5=%s R6=%s stack_free_min=%lu "
                  "(R5 quality is judged off-device; R4 is not covered by this image)",
             pf(r1), pf(r2), pf(r3_pre), pf(r3_post), r5a && r5b ? "STREAMED" : "FAIL", r6s,
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void rng_gate_start(void)
{
    TaskHandle_t t = xTaskCreateStatic(rng_gate_task, "dsp_rnggate", sizeof s_gate_stack / sizeof(StackType_t),
                                       NULL, tskIDLE_PRIORITY + 2, s_gate_stack, &s_gate_task);
    if (t == NULL) {
        ESP_LOGE(TAG, "RNGGATE: task not created");
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
#if CONFIG_DSP_PUMP_GPIO
    ESP_LOGW(TAG, "application runtime: BENCH GPIO%d pump control signal (no load), deep sleep",
             CONFIG_DSP_PUMP_GPIO_NUM);
#else
    ESP_LOGI(TAG, "M1.9c application runtime: SIMULATED pump (no GPIO), deep sleep");
#endif
#elif CONFIG_DSP_RNG_GATE_DIAGNOSTIC
    ESP_LOGI(TAG, "RNG-gate diagnostic: BT controller only, no GPIO, NVS, sleep or dosing activity");
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
#if CONFIG_DSP_RNG_GATE_DIAGNOSTIC
    rng_gate_start();
#endif
    /* Returning from app_main is fine: the main task is deleted, the scheduler keeps
     * running (in the runtime build the application task then owns everything). */
}
