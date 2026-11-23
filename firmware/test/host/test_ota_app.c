/*
 * Firmware update (docs/FIRMWARE_UPDATE.md, docs/BLE_PROTOCOL.md 7.8, 10.8): the pure request and
 * condition logic of dsp_ota, and the Application Service flow on the shared harness of
 * test_app_ops.c (complete wakes, KV, clock, pump, BLE fake running operations in the
 * application task) with a scripted OTA port, Wi-Fi manager and battery port.
 *
 * Oracles: the phone never supplies a URL (built from the compiled base); a refusal stores
 * nothing and never downloads; ACCEPTED ends the session and the download runs with the BLE
 * radio down, Wi-Fi only around it; an activated image restarts; a pending image confirms its
 * boot only after a complete healthy wake and otherwise requests the rollback.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#include "dsp_net.h"
#include "dsp_ota.h"

#define BASE "https://github.com/Nowass/dispenser/releases/download/"

static const uint8_t SHA[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

/* ==== Pure logic ============================================================================ */

static bool rel(const char *s, dsp_semver_t *v)
{
    return dsp_ota_parse_release(s, strlen(s), v);
}

static void test_versions(void)
{
    dsp_semver_t a, b;
    DSP_CHECK(rel("0.3.1", &a));
    DSP_CHECK(a.major == 0 && a.minor == 3 && a.patch == 1 && !a.pre);
    DSP_CHECK(rel("65535.65535.65535", &a));
    DSP_CHECK(!rel("65536.0.0", &a));
    DSP_CHECK(!rel("0.03.1", &a));   /* leading zero */
    DSP_CHECK(!rel("0.3", &a));
    DSP_CHECK(!rel("0.3.1-beta", &a)); /* release versions are plain */
    DSP_CHECK(!rel("0.3.1.", &a));
    DSP_CHECK(!rel("v0.3.1", &a));
    DSP_CHECK(!rel("", &a));
    DSP_CHECK(dsp_ota_parse_running("0.3.0-dev", &b));
    DSP_CHECK(b.pre);
    DSP_CHECK(dsp_ota_parse_running("0.3.0", &b));
    DSP_CHECK(!dsp_ota_parse_running("0.3.0-", &b));
    DSP_CHECK(!dsp_ota_parse_running("0.3.0+x", &b));
    DSP_CHECK(!dsp_ota_parse_running("unknown", &b));
    dsp_semver_t dev, r300, r301, r0100;
    DSP_CHECK(dsp_ota_parse_running("0.3.0-dev", &dev));
    DSP_CHECK(rel("0.3.0", &r300));
    DSP_CHECK(rel("0.3.1", &r301));
    DSP_CHECK(rel("0.10.0", &r0100));
    DSP_CHECK(dsp_ota_compare(&dev, &r300) < 0);
    DSP_CHECK(dsp_ota_compare(&r300, &r301) < 0);
    DSP_CHECK(dsp_ota_compare(&r301, &r0100) < 0); /* numeric, not lexical */
    DSP_CHECK(dsp_ota_compare(&r301, &r301) == 0);
}

static dsp_ota_verdict_t prep(const char *base, const char *running, const char *ver, const char *tag,
                              uint32_t size, dsp_ota_job_t *job)
{
    return dsp_ota_prepare(base, running, (const uint8_t *)ver, strlen(ver), (const uint8_t *)tag,
                           strlen(tag), size, SHA, job);
}

static void test_prepare_builds_only_the_release_url(void)
{
    dsp_ota_job_t j;
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, prep(BASE, "0.3.0", "0.3.1", "firmware-v0.3.1", 1430528, &j));
    DSP_CHECK(strcmp(j.url, BASE "firmware-v0.3.1/dispenser-firmware-0.3.1.bin") == 0);
    DSP_CHECK(strcmp(j.version, "0.3.1") == 0 && strcmp(j.tag, "firmware-v0.3.1") == 0);
    DSP_CHECK(j.size == 1430528 && memcmp(j.sha256, SHA, 32) == 0);
    /* the same release over a -dev build of it is newer */
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, prep(BASE, "0.3.1-dev", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_NEWER, prep(BASE, "0.3.1", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_NEWER, prep(BASE, "0.3.1", "0.3.0", "v1", 1, &j));
    /* tag and version can never leave the release path */
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "a/b", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "..", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", ".", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "a?b", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1/x", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "v1", 0, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "0.3.0", "0.3.1", "v1", DSP_OTA_SLOT_SIZE + 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, prep(BASE, "0.3.0", "0.3.1", "v1", DSP_OTA_SLOT_SIZE, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(BASE, "unknown", "0.3.1", "v1", 1, &j));
    /* only a GitHub release base */
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep("http://github.com/Nowass/dispenser/releases/download/", "0.3.0", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep("https://example.com/Nowass/dispenser/releases/download/", "0.3.0", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep("https://github.com/Nowass/releases/download/", "0.3.0", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep("https://github.com/a/b/c/releases/download/", "0.3.0", "0.3.1", "v1", 1, &j));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, prep(NULL, "0.3.0", "0.3.1", "v1", 1, &j));
}

static dsp_ota_conditions_t good(void)
{
    dsp_ota_conditions_t c;
    memset(&c, 0, sizeof c);
    c.capable = c.wifi_configured = c.clock_trusted = c.battery_valid = true;
    c.now = 1000000;
    c.next_dose_known = true;
    c.next_dose_at = c.now + DSP_OTA_DOSE_MARGIN_S + 1;
    c.battery_mv = DSP_OTA_MIN_BATTERY_MV;
    return c;
}

static void test_conditions(void)
{
    dsp_ota_conditions_t c = good();
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, dsp_ota_check(&c));
    c.next_dose_known = false; /* no schedule at all */
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, dsp_ota_check(&c));
    c = good(); c.next_dose_at = c.now + DSP_OTA_DOSE_MARGIN_S;
    DSP_CHECK_EQ_INT(DSP_OTA_V_DOSE_SOON, dsp_ota_check(&c));
    c = good(); c.next_dose_at = c.now - 5; /* overdue */
    DSP_CHECK_EQ_INT(DSP_OTA_V_DOSE_SOON, dsp_ota_check(&c));
    c = good(); c.clock_trusted = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_DOSE_SOON, dsp_ota_check(&c));
    c = good(); c.battery_mv = DSP_OTA_MIN_BATTERY_MV - 1;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, dsp_ota_check(&c));
    c = good(); c.battery_valid = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, dsp_ota_check(&c));
    c = good(); c.battery_low_open = true;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, dsp_ota_check(&c));
    c = good(); c.wifi_configured = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_NO_WIFI, dsp_ota_check(&c));
    c = good(); c.busy = true; c.wifi_configured = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BUSY, dsp_ota_check(&c));
    c = good(); c.capable = false; c.busy = true;
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_CAPABLE, dsp_ota_check(&c));
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_CAPABLE, dsp_ota_check(NULL));
}

/* ==== Fakes ================================================================================= */

static struct {
    const char *running;
    bool capable, pending;
    dsp_ota_result_t install_result;
    int installs, restarts, confirms_ok, confirms_bad, records;
    bool ble_active_at_install, wifi_up_at_install;
    dsp_ota_job_t job;
    dsp_ota_result_t last_result, recorded;
    char last_version[DSP_OTA_VERSION_MAX + 1], recorded_version[DSP_OTA_VERSION_MAX + 1];
} ota;

static struct {
    int starts, connects, stops;
    bool on, fail, creds;
} wf;

static const char *ota_running(void *ctx) { (void)ctx; return ota.running; }
static bool ota_capable(void *ctx) { (void)ctx; return ota.capable; }
static dsp_ota_result_t ota_install(void *ctx, const dsp_ota_job_t *job)
{
    (void)ctx;
    ota.installs++;
    ota.job = *job;
    ota.ble_active_at_install = app.ble_active;
    ota.wifi_up_at_install = wf.on;
    return ota.install_result;
}
static void ota_restart(void *ctx) { (void)ctx; ota.restarts++; }
static bool ota_pending(void *ctx) { (void)ctx; return ota.pending; }
static void ota_confirm(void *ctx, bool ok)
{
    (void)ctx;
    if (ok) {
        ota.confirms_ok++;
        ota.pending = false;
    } else {
        ota.confirms_bad++;
    }
}
static dsp_ota_result_t ota_last(void *ctx, char v[DSP_OTA_VERSION_MAX + 1])
{
    (void)ctx;
    memcpy(v, ota.last_version, sizeof ota.last_version);
    return ota.last_result;
}
static void ota_record(void *ctx, dsp_ota_result_t r, const char *v)
{
    (void)ctx;
    ota.records++;
    ota.recorded = r;
    snprintf(ota.recorded_version, sizeof ota.recorded_version, "%s", v);
}
static const dsp_ota_port_t OTA = {NULL, ota_running, ota_capable, ota_install, ota_restart,
                                   ota_pending, ota_confirm, ota_last, ota_record};

static dsp_net_status_t w_start(void *ctx) { (void)ctx; wf.starts++; return DSP_NET_OK; }
static dsp_net_status_t w_connect(void *ctx, const dsp_wifi_credentials_t *c, uint32_t t)
{
    (void)ctx; (void)c; (void)t;
    wf.connects++;
    if (wf.fail) {
        return DSP_NET_ERR_TIMEOUT;
    }
    wf.on = true;
    return DSP_NET_OK;
}
static void w_disconnect(void *ctx) { (void)ctx; }
static void w_stop(void *ctx) { (void)ctx; wf.stops++; wf.on = false; }
static const dsp_wifi_driver_t WDRV = {NULL, w_start, w_connect, w_disconnect, w_stop};
static dsp_cred_status_t w_creds(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    if (!wf.creds) {
        return DSP_CRED_UNAVAILABLE;
    }
    memset(out, 0, sizeof *out);
    strcpy(out->ssid, "bench");
    strcpy(out->passphrase, "passphrase-1");
    return DSP_CRED_OK;
}
static const dsp_wifi_credentials_provider_t WCREDS = {NULL, w_creds};
static dsp_wifi_manager_t wman;

static dsp_batt_sample_t batt_now;
static dsp_batt_sample_t b_measure(void *ctx) { (void)ctx; return batt_now; }
static const dsp_battery_port_t BATTP = {NULL, b_measure};

/* After provision(): OTA-capable 0.3.0 with Wi-Fi credentials and a good battery. */
static void with_ota(void)
{
    memset(&ota, 0, sizeof ota);
    memset(&wf, 0, sizeof wf);
    ota.running = "0.3.0";
    ota.capable = true;
    ota.install_result = DSP_OTA_INSTALLED;
    wf.creds = true;
    dsp_wifi_config_t wc = {DSP_WIFI_TIMEOUT_DEFAULT_MS, 1};
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&wman, &WDRV, &WCREDS, &wc));
    batt_now.state = DSP_BATT_VALID;
    batt_now.battery_mv = 4050;
    CFG.battery = &BATTP;
    CFG.wifi = &wman; /* ntp stays NULL: no connectivity stage */
    CFG.ota = &OTA;
    CFG.ota_release_base = BASE;
    pump_never_off = false;
}

static op_t op_start(const char *ver, const char *tag, uint32_t size)
{
    op_t o = mk(OP_START_UPDATE);
    o.upd.version.len = (uint8_t)strlen(ver);
    memcpy(o.upd.version.b, ver, o.upd.version.len);
    o.upd.tag.len = (uint8_t)strlen(tag);
    memcpy(o.upd.tag.b, tag, o.upd.tag.len);
    o.upd.size = size;
    memcpy(o.upd.sha256, SHA, 32);
    return o;
}

/* One session with a single START_UPDATE (the harness ends the window afterwards unless the
 * device does). Returns the verdict. */
static int start_session(const char *ver, const char *tag, uint32_t size, long *writes)
{
    clear_script();
    int i = step(g_true, op_start(ver, tag, size));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[i].r.status);
    if (writes != NULL) {
        *writes = sc.res[i].writes;
    }
    return sc.res[i].verdict;
}

/* ==== Application flow ====================================================================== */

static void test_status_without_and_with_port(void)
{
    provision(true);
    clear_script();
    int i = step(g_true, mk(OP_UPDATE_STATUS));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[i].r.status);
    DSP_CHECK(sc.res[i].ust.running_version.len == 7 && memcmp(sc.res[i].ust.running_version.b, "unknown", 7) == 0);
    DSP_CHECK(!sc.res[i].ust.capable);
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_CAPABLE, start_session("0.3.1", "v1", 1000, NULL));

    with_ota();
    ota.last_result = DSP_OTA_ROLLED_BACK;
    strcpy(ota.last_version, "0.3.1");
    clear_script();
    i = step(g_true, mk(OP_UPDATE_STATUS));
    DSP_CHECK(session());
    DSP_CHECK(sc.res[i].ust.running_version.len == 5 && memcmp(sc.res[i].ust.running_version.b, "0.3.0", 5) == 0);
    DSP_CHECK(sc.res[i].ust.capable);
    DSP_CHECK_EQ_INT(DSP_OTA_ROLLED_BACK, sc.res[i].ust.last_result);
    DSP_CHECK(sc.res[i].ust.last_version.len == 5 && memcmp(sc.res[i].ust.last_version.b, "0.3.1", 5) == 0);
    DSP_CHECK_EQ_INT(0, (int)sc.res[i].writes);
    wf.creds = false; /* no credentials: not capable */
    clear_script();
    i = step(g_true, mk(OP_UPDATE_STATUS));
    DSP_CHECK(session());
    DSP_CHECK(!sc.res[i].ust.capable);
    DSP_CHECK_EQ_INT(0, ota.installs);
    invariants();
}

static void test_accepted_update_runs_after_the_session(void)
{
    provision(true);
    with_ota();
    long writes = -1;
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, start_session("0.3.1", "firmware-v0.3.1", 1430528, &writes));
    DSP_CHECK_EQ_INT(0, (int)writes); /* nothing stored */
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, app.ss.end); /* the device ended the session itself */
    DSP_CHECK(rep.ota_attempted);
    DSP_CHECK_EQ_INT(1, ota.installs);
    DSP_CHECK(!ota.ble_active_at_install); /* BLE radio down */
    DSP_CHECK(ota.wifi_up_at_install);
    DSP_CHECK(!wf.on);                     /* Wi-Fi off again */
    DSP_CHECK(strcmp(ota.job.url, BASE "firmware-v0.3.1/dispenser-firmware-0.3.1.bin") == 0);
    DSP_CHECK(ota.job.size == 1430528 && memcmp(ota.job.sha256, SHA, 32) == 0);
    DSP_CHECK_EQ_INT(DSP_OTA_INSTALLED, rep.ota_result);
    DSP_CHECK_EQ_INT(DSP_OTA_INSTALLED, ota.recorded);
    DSP_CHECK(strcmp(ota.recorded_version, "0.3.1") == 0);
    DSP_CHECK_EQ_INT(1, ota.restarts);
    /* the trace: BLE before UPDATE */
    int ble = -1, upd = -1;
    for (size_t k = 0; k < rep.trace_len; k++) {
        if (rep.trace[k] == DSP_STAGE_BLE && ble < 0) ble = (int)k;
        if (rep.trace[k] == DSP_STAGE_UPDATE) upd = (int)k;
    }
    DSP_CHECK(ble >= 0 && upd > ble);
    invariants();
}

static void test_failures_are_recorded_and_nothing_restarts(void)
{
    provision(true);
    with_ota();
    ota.install_result = DSP_OTA_IMAGE_REJECTED;
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, start_session("0.3.1", "v1", 1000, NULL));
    DSP_CHECK_EQ_INT(1, ota.installs);
    DSP_CHECK_EQ_INT(0, ota.restarts);
    DSP_CHECK_EQ_INT(DSP_OTA_IMAGE_REJECTED, ota.recorded);
    DSP_CHECK(!wf.on);
    DSP_CHECK(rep.slept); /* the wake ends normally */

    with_ota();
    wf.fail = true;
    DSP_CHECK_EQ_INT(DSP_OTA_V_ACCEPTED, start_session("0.3.1", "v1", 1000, NULL));
    DSP_CHECK_EQ_INT(0, ota.installs);
    DSP_CHECK_EQ_INT(0, ota.restarts);
    DSP_CHECK_EQ_INT(DSP_OTA_WIFI_FAILED, ota.recorded);
    DSP_CHECK(!wf.on);
    DSP_CHECK(rep.slept);
    invariants();
}

static void test_refusals_store_nothing_and_never_download(void)
{
    provision(true);
    long w;
    with_ota();
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_NEWER, start_session("0.3.0", "v1", 1000, &w));
    DSP_CHECK_EQ_INT(0, (int)w);
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, start_session("0.3.1", "v1", 0, &w));
    DSP_CHECK_EQ_INT(DSP_OTA_V_INVALID_REQUEST, start_session("0.3.1", "v1", DSP_OTA_SLOT_SIZE + 1, &w));
    ota.capable = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_NOT_CAPABLE, start_session("0.3.1", "v1", 1000, &w));
    with_ota();
    wf.creds = false;
    DSP_CHECK_EQ_INT(DSP_OTA_V_NO_WIFI, start_session("0.3.1", "v1", 1000, &w));
    with_ota();
    batt_now.battery_mv = DSP_OTA_MIN_BATTERY_MV - 1;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, start_session("0.3.1", "v1", 1000, &w));
    batt_now.state = DSP_BATT_UNSTABLE;
    batt_now.battery_mv = 0;
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, start_session("0.3.1", "v1", 1000, &w));
    CFG.battery = NULL; /* no battery input at all */
    DSP_CHECK_EQ_INT(DSP_OTA_V_BATTERY, start_session("0.3.1", "v1", 1000, &w));
    DSP_CHECK_EQ_INT(0, ota.installs);
    DSP_CHECK_EQ_INT(0, ota.records);
    DSP_CHECK_EQ_INT(0, wf.connects);
    invariants();
}

static void test_refused_when_a_dose_is_near(void)
{
    provision(true);
    clear_script();
    dsp_schedule_t once = sched("one-shot", 8, 0, 1000); /* 08:00 local = T0 */
    once.kind = DSP_REC_ONCE;
    once.start_date = date_of(T0);
    step(g_true, op_create(once));
    DSP_CHECK(session());
    with_ota();
    /* about an hour before the dose: accepted would be fine, check the boundary instead */
    advance(T0 - DSP_OTA_DOSE_MARGIN_S + 30);
    DSP_CHECK_EQ_INT(DSP_OTA_V_DOSE_SOON, start_session("0.3.1", "v1", 1000, NULL));
    DSP_CHECK_EQ_INT(0, ota.installs);
    invariants();
}

static void test_pending_image_confirms_after_a_healthy_wake(void)
{
    provision(true);
    with_ota();
    ota.running = "0.3.1";
    ota.pending = true;
    advance(g_true + rep.plan.sleep_s);
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON)); /* the restart after the update */
    DSP_CHECK(rep.ota_confirmed);
    DSP_CHECK_EQ_INT(1, ota.confirms_ok);
    DSP_CHECK_EQ_INT(0, ota.confirms_bad);
    DSP_CHECK_EQ_INT(DSP_OTA_INSTALLED, ota.recorded);
    DSP_CHECK(strcmp(ota.recorded_version, "0.3.1") == 0);
    DSP_CHECK(rep.slept);
    /* confirmed once: later wakes do nothing */
    advance(g_true + rep.plan.sleep_s);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(1, ota.confirms_ok);
    invariants();
}

static void test_pending_image_rolls_back_when_unhealthy(void)
{
    provision(true);
    with_ota();
    ota.pending = true;
    pump_never_off = true; /* the pump-off confirmation fails */
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(rep.ota_rejected);
    DSP_CHECK_EQ_INT(0, ota.confirms_ok);
    DSP_CHECK_EQ_INT(1, ota.confirms_bad);
    pump_never_off = false;
}

int main(void)
{
    DSP_RUN(test_versions);
    DSP_RUN(test_prepare_builds_only_the_release_url);
    DSP_RUN(test_conditions);
    DSP_RUN(test_status_without_and_with_port);
    DSP_RUN(test_accepted_update_runs_after_the_session);
    DSP_RUN(test_failures_are_recorded_and_nothing_restarts);
    DSP_RUN(test_refusals_store_nothing_and_never_download);
    DSP_RUN(test_refused_when_a_dose_is_near);
    DSP_RUN(test_pending_image_confirms_after_a_healthy_wake);
    DSP_RUN(test_pending_image_rolls_back_when_unhealthy);
    return dsp_test_finish();
}
