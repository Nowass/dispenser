/*
 * Firmware-in-the-loop simulator for the Android client tests (M1.10e.2b, docs/ANDROID_BLE_DESIGN.md
 * 6.3, decision E10). TEST-ONLY: a JNI shared library loaded by the Android JVM unit tests, never
 * part of any firmware image or the APK.
 *
 * It runs the REAL firmware logic, unmodified: dsp_app (Application Service, wake cycle, BLE stage),
 * dsp_blesec (Noise responder, dispatcher, response cache) and dsp_ble_transport (fragmentation,
 * generations, close-after-drain), inside the existing host harness world (test_blesec.c over
 * test_app_ops.c: fake KV storage, simulated pump, clocks, power port, setup-code port; the host
 * deterministic RNG). Only the radio is simulated:
 *
 *   - the application task: one native thread per button wake (dsp_app_run_wake);
 *   - the NimBLE host task role: the JVM caller of fwsimConnect/Write/Disconnect (thread-safe through
 *     the transport's own lock, as with NimBLE);
 *   - the radio: advertising flag, one connection, notifications into an outbound queue the JVM
 *     polls, terminate -> disconnect event; the retry callout on virtual time;
 *   - time: the device's monotonic and UTC clocks advance with real elapsed time while the
 *     application task waits, plus explicit jumps requested by the test (fwsimAdvance).
 *
 * Host interoperability with the real firmware logic only: NOT radio behavior, NOT the target RNG,
 * NOT timing on the ESP32-C3 (M1.10d.2, M1.10c.2b, M1.10e.5).
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, pthread_cond_timedwait */
#define DSP_BLESEC_NO_MAIN
#include "test_blesec.c"

#include <jni.h>
#include <pthread.h>
#include <time.h>

#if !defined(DSP_VIA_TRANSPORT)
#error "fwsim needs the transport variant of the harness (DSP_VIA_TRANSPORT)"
#endif

/* ---- state ---------------------------------------------------------------------------------- */

#define FS_EVT_NOTIFY 1
#define FS_EVT_DISCONNECTED 2
#define FS_QCAP 1024

typedef struct {
    uint8_t type;
    uint8_t handle; /* the connection the event belongs to (stale events are recognizable) */
    uint16_t len;
    uint8_t b[DSP_BLE_TR_MTU_PREFERRED];
} fs_evt_t;

static pthread_mutex_t fs_tr = PTHREAD_MUTEX_INITIALIZER; /* the transport's radio lock */
static pthread_mutex_t fs_m = PTHREAD_MUTEX_INITIALIZER;  /* everything below */
static pthread_cond_t fs_cv = PTHREAD_COND_INITIALIZER;
static fs_evt_t fs_q[FS_QCAP];
static int fs_qhead, fs_qcount, fs_qdropped;
static bool fs_adv, fs_conn, fs_signal, fs_armed, fs_running, fs_installed, fs_rng;
static uint64_t fs_arm_at, fs_advance_req, fs_mono_copy, fs_ms_frac;
static uint16_t fs_handle = 1;
static pthread_t fs_thread;
static bool fs_hold;

static uint64_t real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void fs_push(uint8_t type, uint16_t handle, const uint8_t *b, size_t len)
{
    pthread_mutex_lock(&fs_m);
    if (fs_qcount < FS_QCAP && len <= sizeof fs_q[0].b) {
        fs_evt_t *e = &fs_q[(fs_qhead + fs_qcount) % FS_QCAP];
        e->type = type;
        e->handle = (uint8_t)handle;
        e->len = (uint16_t)len;
        if (len > 0) {
            memcpy(e->b, b, len);
        }
        fs_qcount++;
    } else {
        fs_qdropped++;
    }
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
}

/* ---- radio (dsp_ble_radio_t) ---------------------------------------------------------------- */

static bool fr_init(void *ctx)
{
    (void)ctx;
    return true;
}
static void fr_deinit(void *ctx)
{
    (void)ctx;
}
static bool fr_is_off(void *ctx)
{
    (void)ctx;
    return true;
}
static bool fr_adv_start(void *ctx, const uint8_t advert[4], bool fast)
{
    (void)ctx;
    (void)advert;
    (void)fast;
    pthread_mutex_lock(&fs_m);
    fs_adv = true;
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
    return true;
}
static void fr_adv_stop(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&fs_m);
    fs_adv = false;
    pthread_mutex_unlock(&fs_m);
}
static void fr_terminate(void *ctx, uint16_t handle)
{
    (void)ctx;
    pthread_mutex_lock(&fs_m);
    bool mine = fs_conn && handle == fs_handle;
    if (mine) {
        fs_conn = false;
    }
    pthread_mutex_unlock(&fs_m);
    if (mine) {
        fs_push(FS_EVT_DISCONNECTED, handle, NULL, 0);
        dsp_ble_tr_on_disconnect(&TR, handle); /* the controller reports the GAP disconnect */
    }
}
static int fr_notify(void *ctx, uint16_t handle, const uint8_t *frag, size_t len)
{
    (void)ctx;
    pthread_mutex_lock(&fs_m);
    bool ok = fs_conn && handle == fs_handle;
    pthread_mutex_unlock(&fs_m);
    if (!ok) {
        return DSP_BLE_NOTIFY_ERR;
    }
    fs_push(FS_EVT_NOTIFY, handle, frag, len);
    return DSP_BLE_NOTIFY_OK;
}
static void fr_retry_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    pthread_mutex_lock(&fs_m);
    fs_armed = true;
    fs_arm_at = mono_ms + ms;
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
}
static void fr_signal(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&fs_m);
    fs_signal = true;
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
}
/* Application task only: real-time wait (<= 20 ms per call), then the virtual clocks advance by
 * the real elapsed time plus any requested jump; a due retry callout fires. */
static void fr_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    uint64_t start = real_ms();
    uint64_t slice = max_ms < 20u ? max_ms : 20u;
    pthread_mutex_lock(&fs_m);
    while (!fs_signal && fs_advance_req == 0 && !(fs_armed && mono_ms >= fs_arm_at) && real_ms() - start < slice) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&fs_cv, &fs_m, &ts);
    }
    uint64_t elapsed = real_ms() - start + fs_advance_req;
    fs_advance_req = 0;
    fs_signal = false;
    mono_ms += elapsed;
    fs_ms_frac += elapsed;
    g_true += (dsp_utc_t)(fs_ms_frac / 1000u);
    fs_ms_frac %= 1000u;
    fs_mono_copy = mono_ms;
    bool fire = fs_armed && mono_ms >= fs_arm_at;
    if (fire) {
        fs_armed = false;
    }
    pthread_mutex_unlock(&fs_m);
    if (fire) {
        dsp_ble_tr_on_retry(&TR);
    }
}
static uint32_t fr_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)mono_ms;
}
static void fr_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&fs_tr);
}
static void fr_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&fs_tr);
}
static const dsp_ble_radio_t FS_RADIO = {NULL,        fr_init,  fr_deinit,    fr_is_off, fr_adv_start,
                                         fr_adv_stop, fr_terminate, fr_notify, fr_retry_arm, fr_signal,
                                         fr_wait,     fr_now,   fr_lock,      fr_unlock};

/* the Application Service's queue lock: real mutual exclusion between the two tasks */
static pthread_mutex_t fs_queue = PTHREAD_MUTEX_INITIALIZER;
static void fq_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&fs_queue);
}
static void fq_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&fs_queue);
}
static const dsp_app_sync_t FS_SYNC = {NULL, fq_lock, fq_unlock};

/* ---- application task -------------------------------------------------------------------------- */

static void *fs_app_task(void *arg)
{
    (void)arg;
    CFG.recovery_hold_ms = fs_hold ? DSP_OWNER_ARM_HOLD_MS : 0;
    clear_script();
    (void)wake(DSP_CAUSE_BUTTON); /* returns when the service window ends (idle, maximum, owner's END) */
    CFG.recovery_hold_ms = 0;
    pthread_mutex_lock(&fs_m);
    fs_running = false;
    fs_adv = false;
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
    return NULL;
}

/* ---- JNI: cz.nowass.dispenser.device.fwsim.FirmwareSim (test source set) ------------------------ */

#define FS(name) Java_cz_nowass_dispenser_device_fwsim_FirmwareSim_##name

static bool get_bytes(JNIEnv *env, jbyteArray a, uint8_t *out, size_t want)
{
    if (a == NULL || (size_t)(*env)->GetArrayLength(env, a) != want) {
        return false;
    }
    (*env)->GetByteArrayRegion(env, a, 0, (jsize)want, (jbyte *)out);
    return !(*env)->ExceptionCheck(env);
}

/* A fresh, provisioned, UNCLAIMED device with this device ID and setup code. */
JNIEXPORT jboolean JNICALL FS(reset)(JNIEnv *env, jclass cls, jbyteArray device_id, jbyteArray setup)
{
    (void)cls;
    if (fs_running) {
        return JNI_FALSE;
    }
    uint8_t id[6], code[16];
    if (!get_bytes(env, device_id, id, 6) || !get_bytes(env, setup, code, 16)) {
        return JNI_FALSE;
    }
    if (!fs_rng) {
        if (!test_rng_install(0xF1A5u)) {
            return JNI_FALSE;
        }
        fs_rng = true;
    }
    memcpy(DEV_ID, id, 6);
    fresh_unclaimed(); /* world, provisioning wake, blesec init, transport (re-initialized below) */
    memcpy(g_dev_setup, code, 16);
    dsp_ble_tr_init(&TR, &FS_RADIO, &app, &TRP);
    BLE2 = TRP;
    BLE2.start = e2e_start;
    BLE2.suspend = e2e_suspend;
    BLE2.resume = e2e_resume;
    BLE2.stop = e2e_stop;
    CFG.ble = &BLE2;
    CFG.sync = &FS_SYNC;
    manual_via_ble = true;
    pthread_mutex_lock(&fs_m);
    fs_qhead = fs_qcount = fs_qdropped = 0;
    fs_adv = fs_conn = fs_signal = fs_armed = false;
    fs_advance_req = 0;
    fs_mono_copy = mono_ms;
    pthread_mutex_unlock(&fs_m);
    fs_installed = true;
    return JNI_TRUE;
}

/* A button wake: the application task runs the service window until it ends. */
JNIEXPORT jboolean JNICALL FS(buttonWake)(JNIEnv *env, jclass cls, jboolean recovery_hold)
{
    (void)env;
    (void)cls;
    if (!fs_installed || fs_running) {
        return JNI_FALSE;
    }
    fs_hold = recovery_hold == JNI_TRUE;
    fs_running = true;
    if (pthread_create(&fs_thread, NULL, fs_app_task, NULL) != 0) {
        fs_running = false;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL FS(waitAdvertising)(JNIEnv *env, jclass cls, jint timeout_ms)
{
    (void)env;
    (void)cls;
    uint64_t end = real_ms() + (uint64_t)timeout_ms;
    pthread_mutex_lock(&fs_m);
    while (!fs_adv && fs_running && real_ms() < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&fs_cv, &fs_m, &ts);
    }
    bool adv = fs_adv;
    pthread_mutex_unlock(&fs_m);
    return adv ? JNI_TRUE : JNI_FALSE;
}

/* GAP connect + MTU exchange + CCCD subscribe from the phone (host task role). 0 = refused. */
JNIEXPORT jint JNICALL FS(connect)(JNIEnv *env, jclass cls, jint mtu)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&fs_m);
    bool ok = fs_adv && !fs_conn && fs_running;
    if (ok) {
        fs_conn = true;
        fs_adv = false; /* the stack stops advertising on a connection (dsp_ble_tr_on_connect relies on it);
                           the transport restarts it from the app task after the disconnect */
        fs_handle = (uint16_t)(fs_handle % 8u + 1u);
    }
    uint16_t h = fs_handle;
    pthread_mutex_unlock(&fs_m);
    if (!ok) {
        return 0;
    }
    dsp_ble_tr_on_connect(&TR, h, true);
    dsp_ble_tr_on_mtu(&TR, h, (uint16_t)mtu);
    dsp_ble_tr_on_subscribe(&TR, h, true);
    return h;
}

/* One ATT write from the phone; returns the ATT result (0 = OK). */
JNIEXPORT jint JNICALL FS(write)(JNIEnv *env, jclass cls, jbyteArray fragment)
{
    (void)cls;
    uint8_t buf[DSP_BLE_TR_MTU_PREFERRED];
    jsize n = fragment == NULL ? -1 : (*env)->GetArrayLength(env, fragment);
    if (n <= 0 || (size_t)n > sizeof buf) {
        return -1;
    }
    (*env)->GetByteArrayRegion(env, fragment, 0, n, (jbyte *)buf);
    pthread_mutex_lock(&fs_m);
    bool ok = fs_conn;
    uint16_t h = fs_handle;
    pthread_mutex_unlock(&fs_m);
    if (!ok) {
        return -2;
    }
    return dsp_ble_tr_on_write(&TR, h, buf, (size_t)n);
}

/* Next event of connection [handle] for the phone: {type, handle, payload...}; null on timeout.
 * Events of an earlier connection are dropped (counted as stale); a poller of an earlier connection
 * never takes the current connection's events. */
static int fs_stale;
JNIEXPORT jbyteArray JNICALL FS(poll)(JNIEnv *env, jclass cls, jint handle, jint timeout_ms)
{
    (void)cls;
    uint64_t end = real_ms() + (uint64_t)timeout_ms;
    fs_evt_t e;
    bool got = false;
    pthread_mutex_lock(&fs_m);
    for (;;) {
        while (fs_qcount > 0 && fs_q[fs_qhead].handle != (uint8_t)handle &&
               fs_q[fs_qhead].handle != (uint8_t)fs_handle) {
            fs_qhead = (fs_qhead + 1) % FS_QCAP; /* an earlier connection's event */
            fs_qcount--;
            fs_stale++;
        }
        bool mine = fs_qcount > 0 && fs_q[fs_qhead].handle == (uint8_t)handle;
        bool others = fs_qcount > 0 && !mine;
        if (mine || others || real_ms() >= end) {
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&fs_cv, &fs_m, &ts);
    }
    if (fs_qcount > 0 && fs_q[fs_qhead].handle == (uint8_t)handle) {
        e = fs_q[fs_qhead];
        fs_qhead = (fs_qhead + 1) % FS_QCAP;
        fs_qcount--;
        got = true;
    }
    pthread_mutex_unlock(&fs_m);
    if (!got) {
        return NULL;
    }
    jbyteArray a = (*env)->NewByteArray(env, (jsize)(e.len + 2));
    if (a != NULL) {
        const uint8_t hdr[2] = {e.type, e.handle};
        (*env)->SetByteArrayRegion(env, a, 0, 2, (const jbyte *)hdr);
        if (e.len > 0) {
            (*env)->SetByteArrayRegion(env, a, 2, (jsize)e.len, (const jbyte *)e.b);
        }
    }
    return a;
}

/* The phone drops the link (out of range / app closed the GATT client). */
JNIEXPORT void JNICALL FS(disconnect)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&fs_m);
    bool was = fs_conn;
    fs_conn = false;
    uint16_t h = fs_handle;
    pthread_mutex_unlock(&fs_m);
    if (was) {
        dsp_ble_tr_on_disconnect(&TR, h);
    }
}

/* Three short presses of the device button (recovery confirmation). */
JNIEXPORT void JNICALL FS(press3)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&fs_m);
    uint64_t base = fs_mono_copy + 400;
    pthread_mutex_unlock(&fs_m);
    for (int k = 0; k < 3; k++) {
        dsp_button_event_t e = {DSP_BUTTON_SHORT, base + (uint64_t)k * 400, base + (uint64_t)k * 400 + 100};
        (void)dsp_app_post_button(&app, &e);
    }
}

/* Requests a virtual time jump of the device's clocks (applied by the application task). */
JNIEXPORT void JNICALL FS(advance)(JNIEnv *env, jclass cls, jlong ms)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&fs_m);
    fs_advance_req += (uint64_t)ms;
    pthread_cond_broadcast(&fs_cv);
    pthread_mutex_unlock(&fs_m);
}

JNIEXPORT jboolean JNICALL FS(wakeRunning)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    pthread_mutex_lock(&fs_m);
    bool r = fs_running;
    pthread_mutex_unlock(&fs_m);
    return r ? JNI_TRUE : JNI_FALSE;
}

/* Waits for the wake to end (true) or the timeout (false); joins the application task. */
JNIEXPORT jboolean JNICALL FS(joinWake)(JNIEnv *env, jclass cls, jint timeout_ms)
{
    (void)env;
    (void)cls;
    uint64_t end = real_ms() + (uint64_t)timeout_ms;
    pthread_mutex_lock(&fs_m);
    while (fs_running && real_ms() < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&fs_cv, &fs_m, &ts);
    }
    bool done = !fs_running;
    pthread_mutex_unlock(&fs_m);
    if (done) {
        pthread_join(fs_thread, NULL);
    }
    return done ? JNI_TRUE : JNI_FALSE;
}

/* Durable ownership state (only between wakes): claim_state u8 | id_pub 32 | 4 x {used u8, pub 32}. */
JNIEXPORT jbyteArray JNICALL FS(inspect)(JNIEnv *env, jclass cls)
{
    (void)cls;
    if (fs_running) {
        return NULL;
    }
    dsp_auth_t a = durable_auth();
    uint8_t out[1 + 32 + 4 * 33];
    out[0] = a.claim_state;
    memcpy(out + 1, a.id_pub, 32);
    for (int i = 0; i < 4; i++) {
        out[33 + i * 33] = a.slot[i].used ? 1 : 0;
        memcpy(out + 34 + i * 33, a.slot[i].pub, 32);
    }
    dsp_auth_wipe(&a);
    jbyteArray r = (*env)->NewByteArray(env, (jsize)sizeof out);
    if (r != NULL) {
        (*env)->SetByteArrayRegion(env, r, 0, (jsize)sizeof out, (const jbyte *)out);
    }
    return r;
}

/* Harness consistency: DSP_CHECK failures inside the firmware world, dropped radio events,
 * whether the security layer is idle with its state wiped (only between wakes), the last BLE end
 * reason, pump runs and the harness pump-oracle violations (a pump run without a durable decision,
 * or a DUPLICATE run of the same manual request: test_app_ops.c pump_run). */
JNIEXPORT jintArray JNICALL FS(diagnostics)(JNIEnv *env, jclass cls)
{
    (void)cls;
    jint d[6];
    d[0] = dsp_test_failures;
    pthread_mutex_lock(&fs_m);
    d[1] = fs_qdropped;
    pthread_mutex_unlock(&fs_m);
    d[2] = fs_running ? -1 : (SECS.state == DSP_BLESEC_IDLE ? 1 : 0);
    d[3] = (jint)app.rep.ble_end;
    d[4] = pump.calls;      /* read while the application task is between pump runs */
    d[5] = pump.violations;
    jintArray r = (*env)->NewIntArray(env, 6);
    if (r != NULL) {
        (*env)->SetIntArrayRegion(env, r, 0, 6, d);
    }
    return r;
}

/* The Info characteristic value the transport serves (read under the transport's lock). */
JNIEXPORT jbyteArray JNICALL FS(info)(JNIEnv *env, jclass cls)
{
    (void)cls;
    uint8_t v[DSP_BLE_INFO_LEN];
    pthread_mutex_lock(&fs_tr);
    memcpy(v, TR.info, sizeof v);
    pthread_mutex_unlock(&fs_tr);
    jbyteArray r = (*env)->NewByteArray(env, (jsize)sizeof v);
    if (r != NULL) {
        (*env)->SetByteArrayRegion(env, r, 0, (jsize)sizeof v, (const jbyte *)v);
    }
    return r;
}
