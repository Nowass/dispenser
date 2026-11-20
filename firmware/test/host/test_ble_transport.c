/*
 * M1.10d.1: the pure BLE byte transport (components/dispenser_platform/src/dsp_ble_transport.c)
 * over a fault-injecting fake radio. docs/BLE_TRANSPORT_DESIGN.md section 15.1.
 *
 *  - fragmentation and reassembly at every ATT MTU 23..247 (and a clamped 517), boundary lengths;
 *  - fragment faults (no FIRST, index gap, duplicate, empty, oversize, the 2 s gap), the
 *    transport's own fault limit, random fragment fuzz;
 *  - backpressure: queue refusal (no plaintext notice), buffer exhaustion with asynchronous
 *    retries, retry limit and absolute deadline, ring capacity, unsubscribed clients;
 *  - lifecycle: generations, stale handles and generations, disconnect during RX and TX, stale
 *    completions, suspend with and without a timely GAP disconnect, resume, stop, refused links,
 *    fast/slow advertising, Info and advertising payload, close after the final reply and the
 *    T13 best-effort semantics (no delivery assumption, no blocking, bounded close, disconnect
 *    and stale callouts during the grace period);
 *  - two threads (a simulated NimBLE host task and the application task) with an error-checking
 *    mutex (lock nesting fails), checking that no fragment ever reaches another connection.
 * The radio's lock is PTHREAD_MUTEX_ERRORCHECK: a nested or foreign unlock aborts the test.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "dsp_app.h"
#include "dsp_ble_transport.h"
#include "dsp_bleproto.h"
#include "dsp_test.h"

/* ---- Fake radio ---------------------------------------------------------------------------- */

#define MAXF 64
typedef struct {
    pthread_mutex_t mtx;
    bool threaded;
    /* state */
    bool inited, adv_on, adv_fast;
    int inits, deinits, adv_starts, adv_stops, terminates, notifies;
    uint8_t advert[4];
    uint16_t last_term;
    bool adv_fail;
    bool init_fail;
    /* notify behavior */
    int busy_next;      /* the next n notifies return BUSY */
    bool busy_always;
    bool err_next;
    bool air_loss;      /* accepted by the "stack" but never received by the phone */
    void (*in_notify)(void); /* hook during a notify (lock released) */
    /* the "phone": fragments received per handle */
    uint16_t phone_handle; /* the controller's current link (NO_HANDLE = none) */
    uint32_t phone_link;   /* link counter of the controller (== transport gen when 1:1) */
    dsp_ble_reasm_t phone_rx;
    uint8_t frames[MAXF][DSP_BLE_FRAME_MAX];
    size_t frame_len[MAXF];
    int nframes;
    size_t max_frag;
    int frags;
    /* time and timers */
    uint32_t now;
    bool retry_armed;
    uint32_t retry_at;
    /* terminate behavior: deliver the GAP disconnect at the next wait (or never) */
    bool term_ignored;
    bool disc_pending;
    uint16_t disc_handle;
    int signals;
    int waits;
} fake_t;

static fake_t F;
static dsp_ble_transport_t T;
static dsp_ble_port_t P;
static dsp_app_t app;
static dsp_app_config_t acfg;

static void f_lock(void *ctx)
{
    (void)ctx;
    int rc = pthread_mutex_lock(&F.mtx);
    if (rc != 0) {
        fprintf(stderr, "radio lock: nested or invalid (%d)\n", rc);
        abort();
    }
}
static void f_unlock(void *ctx)
{
    (void)ctx;
    int rc = pthread_mutex_unlock(&F.mtx);
    if (rc != 0) {
        fprintf(stderr, "radio unlock: not owner (%d)\n", rc);
        abort();
    }
}
static bool f_init(void *ctx)
{
    (void)ctx;
    F.inits++;
    if (F.init_fail) {
        return false;
    }
    F.inited = true;
    return true;
}
static void f_deinit(void *ctx)
{
    (void)ctx;
    F.deinits++;
    F.inited = false;
    F.adv_on = false;
}
static bool f_is_off(void *ctx)
{
    (void)ctx;
    return !F.inited;
}
static bool f_adv_start(void *ctx, const uint8_t advert[4], bool fast)
{
    (void)ctx;
    if (F.adv_fail || !F.inited) {
        return false;
    }
    F.adv_starts++;
    F.adv_on = true;
    F.adv_fast = fast;
    memcpy(F.advert, advert, 4);
    return true;
}
static void f_adv_stop(void *ctx)
{
    (void)ctx;
    F.adv_stops++;
    F.adv_on = false;
}
static void f_terminate(void *ctx, uint16_t handle)
{
    (void)ctx;
    F.terminates++;
    F.last_term = handle;
    if (!F.term_ignored && !F.threaded) {
        F.disc_pending = true;
        F.disc_handle = handle;
    }
}
static int f_notify(void *ctx, uint16_t handle, const uint8_t *frag, size_t len)
{
    (void)ctx;
    F.notifies++;
    if (F.in_notify != NULL) {
        void (*h)(void) = F.in_notify;
        F.in_notify = NULL;
        h();
    }
    if (F.err_next) {
        F.err_next = false;
        return DSP_BLE_NOTIFY_ERR;
    }
    if (F.busy_always || F.busy_next > 0) {
        if (F.busy_next > 0) {
            F.busy_next--;
        }
        return DSP_BLE_NOTIFY_BUSY;
    }
    if (handle != F.phone_handle) {
        return DSP_BLE_NOTIFY_ERR; /* not connected */
    }
    if (F.air_loss) {
        return DSP_BLE_NOTIFY_OK; /* a notification has no ATT acknowledgement: lost silently */
    }
    F.frags++;
    if (len > F.max_frag) {
        F.max_frag = len;
    }
    bool complete = false;
    if (dsp_ble_reasm_push(&F.phone_rx, frag, len, F.now, &complete) == DSP_BLE_OK && complete &&
        F.nframes < MAXF) {
        memcpy(F.frames[F.nframes], F.phone_rx.buf, F.phone_rx.len);
        F.frame_len[F.nframes++] = F.phone_rx.len;
        dsp_ble_reasm_init(&F.phone_rx);
    }
    return DSP_BLE_NOTIFY_OK;
}
static void f_retry_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    F.retry_armed = true;
    F.retry_at = F.now + ms;
}
static void f_signal(void *ctx)
{
    (void)ctx;
    F.signals++;
}
/* Single-threaded: the "host task" events due within the wait run inside it. */
static void f_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    F.waits++;
    uint32_t end = F.now + max_ms;
    if (F.disc_pending) {
        F.disc_pending = false;
        if (F.disc_handle == F.phone_handle) {
            F.phone_handle = DSP_BLE_TR_NO_HANDLE;
        }
        dsp_ble_tr_on_disconnect(&T, F.disc_handle);
        return;
    }
    if (F.retry_armed && (int32_t)(F.retry_at - end) <= 0) {
        F.now = F.retry_at > F.now ? F.retry_at : F.now;
        F.retry_armed = false;
        dsp_ble_tr_on_retry(&T);
        return;
    }
    F.now = end;
}
static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return F.now;
}
static const dsp_ble_radio_t RADIO = {NULL,      f_init,     f_deinit,    f_is_off, f_adv_start,
                                      f_adv_stop, f_terminate, f_notify,  f_retry_arm, f_signal,
                                      f_wait,    f_now,      f_lock,      f_unlock};

/* ---- Helpers ------------------------------------------------------------------------------- */

static void reset_all(void)
{
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_ERRORCHECK);
    memset(&F, 0, sizeof F);
    pthread_mutex_init(&F.mtx, &at);
    pthread_mutexattr_destroy(&at);
    F.now = 1000;
    F.phone_handle = DSP_BLE_TR_NO_HANDLE;
    memset(&app, 0, sizeof app);
    memset(&acfg, 0, sizeof acfg);
    app.cfg = &acfg;
    app.accepting = true; /* a service session is open (the queue accepts) */
    dsp_ble_tr_init(&T, &RADIO, &app, &P);
}

static uint8_t INFO[32], ADV[4] = {1, 0, 0x12, 0x34};

/* start + publish + one connection (handle h) with the given MTU, subscribed */
static uint32_t open_link(uint16_t h, uint16_t mtu)
{
    if (!F.inited) {
        DSP_CHECK(P.start(P.ctx));
        P.publish_info(P.ctx, INFO, ADV);
    }
    P.wait(P.ctx, 0);
    DSP_CHECK(F.adv_on);
    F.phone_handle = h;
    F.phone_link++;
    dsp_ble_reasm_init(&F.phone_rx);
    dsp_ble_tr_on_connect(&T, h, true);
    F.adv_on = false;
    dsp_ble_tr_on_mtu(&T, h, mtu);
    dsp_ble_tr_on_subscribe(&T, h, true);
    return T.gen;
}

static void drop_link(uint16_t h)
{
    F.phone_handle = DSP_BLE_TR_NO_HANDLE;
    dsp_ble_tr_on_disconnect(&T, h);
}

static void pop_all(void)
{
    app.q_head = 0;
    app.q_count = 0;
}

/* The phone writes `frame` at `mtu` (one ATT write per fragment). */
static int phone_write(uint16_t h, const uint8_t *frame, size_t len, uint16_t mtu)
{
    size_t n = dsp_ble_frag_count(len, mtu);
    uint8_t frag[DSP_BLE_TR_MTU_PREFERRED];
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        size_t fl = dsp_ble_frag_get(frame, len, mtu, i, frag, sizeof frag);
        rc |= dsp_ble_tr_on_write(&T, h, frag, fl);
    }
    return rc;
}

static void fill(uint8_t *b, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        b[i] = (uint8_t)(seed >> 16);
    }
}

/* ---- Tests ------------------------------------------------------------------------------- */

static void test_fragmentation_all_mtus(void)
{
    static uint8_t frame[DSP_BLE_FRAME_MAX];
    int checked = 0;
    for (unsigned mtu = 23; mtu <= 248; mtu++) {
        uint16_t neg = (uint16_t)(mtu == 248 ? 517 : mtu); /* a large client MTU is clamped */
        uint16_t eff = (uint16_t)(neg > 247 ? 247 : neg);
        size_t dm = (size_t)eff - 4u;
        size_t lens[] = {8, 9, dm - 1, dm, dm + 1, 2 * dm, 2 * dm + 1, 519, 520, 100 + mtu % 300};
        reset_all();
        uint32_t gen = open_link(7, neg);
        DSP_CHECK_EQ_INT(eff, T.mtu);
        for (size_t k = 0; k < sizeof lens / sizeof lens[0]; k++) {
            size_t len = lens[k];
            if (len < 8 || len > DSP_BLE_FRAME_MAX) {
                continue;
            }
            fill(frame, len, (uint32_t)(mtu * 1000u + k));
            /* RX: fragments in, one complete frame posted unchanged */
            pop_all();
            DSP_CHECK_EQ_INT(0, phone_write(7, frame, len, eff));
            DSP_CHECK_EQ_INT(1, (int)app.q_count);
            DSP_CHECK_EQ_INT((int)len, app.q[0].len);
            DSP_CHECK(app.q[0].gen == gen && memcmp(app.q[0].bytes, frame, len) == 0);
            /* TX: one frame out, fragments within the negotiated MTU, reassembled unchanged */
            F.nframes = 0;
            F.frags = 0;
            F.max_frag = 0;
            DSP_CHECK(P.send_frame(P.ctx, gen, frame, len));
            DSP_CHECK_EQ_INT(1, F.nframes);
            DSP_CHECK_EQ_INT((int)dsp_ble_frag_count(len, eff), F.frags);
            DSP_CHECK(F.max_frag <= (size_t)eff - 3u);
            DSP_CHECK(F.frame_len[0] == len && memcmp(F.frames[0], frame, len) == 0);
            checked++;
        }
        DSP_CHECK(dsp_ble_frag_count(DSP_BLE_FRAME_MAX, eff) <= 64);
    }
    DSP_CHECK(checked > 2000);
}

static void test_mtu_change_between_frames(void)
{
    reset_all();
    uint32_t gen = open_link(3, 23);
    static uint8_t frame[300];
    fill(frame, sizeof frame, 9);
    F.busy_next = 1; /* the frame starts (MTU 23 captured), then waits for a retry */
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    dsp_ble_tr_on_mtu(&T, 3, 247); /* during the frame: applies to the next frame only */
    F.frags = 0;
    P.wait(P.ctx, 1000); /* retry */
    DSP_CHECK_EQ_INT(1, F.nframes);
    DSP_CHECK_EQ_INT((int)dsp_ble_frag_count(sizeof frame, 23), F.frags);
    F.frags = 0;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    DSP_CHECK_EQ_INT((int)dsp_ble_frag_count(sizeof frame, 247), F.frags);
    DSP_CHECK_EQ_INT(2, F.nframes);
}

static void test_rx_faults(void)
{
    reset_all();
    uint32_t gen = open_link(5, 23);
    uint8_t f[32];
    memset(f, 0xAB, sizeof f);
    /* no FIRST */
    f[0] = 0x01;
    DSP_CHECK_EQ_INT(0, dsp_ble_tr_on_write(&T, 5, f, 10));
    DSP_CHECK_EQ_INT(1, app.lnk.faults);
    /* index gap / duplicate */
    f[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    f[0] = 0x00; /* index 0 again (duplicate of the FIRST's index) */
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    DSP_CHECK_EQ_INT(2, app.lnk.faults);
    /* empty fragment */
    f[0] = 0xC0;
    (void)dsp_ble_tr_on_write(&T, 5, f, 1);
    DSP_CHECK_EQ_INT(3, app.lnk.faults);
    /* FIRST mid-frame discards the partial frame and starts a new one (no error) */
    f[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    f[0] = 0x41;
    (void)dsp_ble_tr_on_write(&T, 5, f, 5);
    DSP_CHECK_EQ_INT(3, app.lnk.faults);
    DSP_CHECK_EQ_INT(1, (int)app.q_count);
    DSP_CHECK_EQ_INT(19 + 4, app.q[0].len);
    pop_all();
    /* the 2 s gap: 2000 ms is accepted, 2001 ms discards */
    f[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    F.now += 2000;
    f[0] = 0x41;
    (void)dsp_ble_tr_on_write(&T, 5, f, 5);
    DSP_CHECK_EQ_INT(1, (int)app.q_count);
    pop_all();
    f[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 5, f, 20);
    F.now += 2001;
    f[0] = 0x41;
    (void)dsp_ble_tr_on_write(&T, 5, f, 5);
    DSP_CHECK_EQ_INT(0, (int)app.q_count); /* a partial frame never completes into the queue */
    DSP_CHECK_EQ_INT(4, app.lnk.faults);
    /* oversize: more than 520 bytes */
    reset_all();
    gen = open_link(5, 247);
    uint8_t big[244];
    memset(big, 1, sizeof big);
    big[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 5, big, 244);
    big[0] = 0x01;
    (void)dsp_ble_tr_on_write(&T, 5, big, 244);
    big[0] = 0x42;
    (void)dsp_ble_tr_on_write(&T, 5, big, 244); /* 729 > 520 */
    DSP_CHECK_EQ_INT(1, app.lnk.faults);
    DSP_CHECK_EQ_INT(0, (int)app.q_count);
    /* a write longer than one fragment (long/prepared write) is refused at ATT level */
    DSP_CHECK_EQ_INT(DSP_BLE_WRITE_BAD_LEN, dsp_ble_tr_on_write(&T, 5, big, 245));
    /* the transport never answers a fault itself: nothing was notified */
    DSP_CHECK_EQ_INT(0, F.notifies);
    /* the transport's own fault limit closes the link */
    for (unsigned i = 0; i < DSP_BLE_TR_RX_FAULT_LIMIT; i++) {
        f[0] = 0x05;
        (void)dsp_ble_tr_on_write(&T, 5, f, 4);
    }
    DSP_CHECK_EQ_INT(1, F.terminates);
    DSP_CHECK_EQ_INT(5, F.last_term);
    DSP_CHECK(!P.connected(P.ctx));
    DSP_CHECK_EQ_INT(1, (int)T.st.rx_fault_closes);
    /* writes of a closing link are ignored */
    uint32_t ign = T.st.rx_ignored;
    (void)phone_write(5, big, 100, 247);
    DSP_CHECK(T.st.rx_ignored > ign);
    (void)gen;
}

static void test_fragment_fuzz(void)
{
    reset_all();
    (void)open_link(9, 64);
    uint32_t seed = 0xC0FFEEu;
    uint8_t f[61];
    size_t posted = 0;
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1664525u + 1013904223u;
        size_t len = (seed >> 8) % sizeof f + 1;
        fill(f, len, seed);
        if ((seed & 7) == 0) {
            f[0] = (uint8_t)(0xC0u | (f[0] & 0x3Fu)); /* some complete single fragments */
        }
        if ((seed & 0x3F00) == 0) {
            F.now += 2001;
        }
        (void)dsp_ble_tr_on_write(&T, 9, f, len);
        if (app.q_count > 0) {
            DSP_CHECK(app.q[0].len >= 1 && app.q[0].len <= DSP_BLE_FRAME_MAX);
            posted += app.q_count;
            pop_all();
        }
        if (!P.connected(P.ctx)) { /* the fault limit closed it: reconnect */
            drop_link(9);
            pop_all();
            app.lnk.up = false;
            (void)open_link(9, 64);
        }
    }
    DSP_CHECK(posted > 0);
    DSP_CHECK_EQ_INT(0, F.nframes); /* never a reply from the transport */
}

static void test_queue_refusal_no_plaintext(void)
{
    reset_all();
    (void)open_link(2, 247);
    uint8_t fr[8] = {1, 0, 1, 0, 1, 0, 0, 0};
    fr[0] = 1;
    uint8_t one[9];
    one[0] = 0xC0;
    memcpy(one + 1, fr, 8);
    for (int i = 0; i < DSP_APP_CMDQ_CAPACITY; i++) {
        (void)dsp_ble_tr_on_write(&T, 2, one, sizeof one);
    }
    DSP_CHECK_EQ_INT(DSP_APP_CMDQ_CAPACITY, (int)app.q_count);
    (void)dsp_ble_tr_on_write(&T, 2, one, sizeof one); /* refused by the full queue */
    DSP_CHECK_EQ_INT(1, (int)T.st.rx_refused);
    DSP_CHECK_EQ_INT(0, F.notifies); /* dropped silently: no plain EV_BUSY (T3) */
    app.accepting = false;            /* outside a session: refused as well */
    pop_all();
    (void)dsp_ble_tr_on_write(&T, 2, one, sizeof one);
    DSP_CHECK_EQ_INT(2, (int)T.st.rx_refused);
    DSP_CHECK_EQ_INT(0, F.notifies);
}

static void test_tx_backpressure(void)
{
    static uint8_t frame[520];
    fill(frame, sizeof frame, 4);
    /* exhausted pool, recovered by asynchronous retries (nothing blocks in send_frame) */
    reset_all();
    uint32_t gen = open_link(1, 23);
    F.busy_next = 10;
    int w0 = F.waits;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    DSP_CHECK_EQ_INT(w0, F.waits);  /* returned at once */
    DSP_CHECK(F.retry_armed);       /* the retry is a callout, not a wait */
    for (int i = 0; i < 20 && F.nframes == 0; i++) {
        P.wait(P.ctx, 100);
    }
    DSP_CHECK_EQ_INT(1, F.nframes);
    DSP_CHECK(F.frame_len[0] == sizeof frame && memcmp(F.frames[0], frame, sizeof frame) == 0);
    DSP_CHECK_EQ_INT(10, (int)T.st.tx_busy);
    DSP_CHECK_EQ_INT(0, F.terminates);
    /* a completion event resumes the pump at once */
    F.busy_next = 1;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 100));
    F.retry_armed = false;
    dsp_ble_tr_on_tx_done(&T, 1);
    DSP_CHECK_EQ_INT(2, F.nframes);
    /* retry limit: the frame fails, all queued frames are released, the link closes */
    reset_all();
    gen = open_link(1, 23);
    F.busy_always = true;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 50));
    for (int i = 0; i < 100 && F.terminates == 0; i++) {
        P.wait(P.ctx, 1000);
    }
    DSP_CHECK_EQ_INT(1, F.terminates);
    DSP_CHECK_EQ_INT(1, (int)T.st.tx_failures);
    DSP_CHECK_EQ_INT(0, T.tx_count);
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 50)); /* closing: refused */
    DSP_CHECK(T.st.tx_busy <= DSP_BLE_TR_RETRY_MAX + 1);
    /* absolute deadline: a slow trickle that never finishes in time fails */
    reset_all();
    gen = open_link(1, 23);
    F.busy_next = 1;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    F.now += DSP_BLE_TR_TX_DEADLINE_MS + 1;
    dsp_ble_tr_on_retry(&T);
    DSP_CHECK_EQ_INT(1, F.terminates);
    DSP_CHECK_EQ_INT(1, (int)T.st.tx_failures);
    DSP_CHECK_EQ_INT(0, F.nframes);
    /* an error from the stack fails the frame */
    reset_all();
    gen = open_link(1, 23);
    F.err_next = true;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 30));
    DSP_CHECK_EQ_INT(1, F.terminates);
    /* ring capacity */
    reset_all();
    gen = open_link(1, 23);
    F.busy_always = true;
    for (unsigned i = 0; i < DSP_BLE_TR_TX_SLOTS; i++) {
        DSP_CHECK(P.send_frame(P.ctx, gen, frame, 40));
    }
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 40));
    /* unsubscribed client: refused and counted */
    reset_all();
    gen = open_link(1, 23);
    dsp_ble_tr_on_subscribe(&T, 1, false);
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 40));
    DSP_CHECK_EQ_INT(1, (int)T.st.tx_unsubscribed);
    /* invalid frames */
    DSP_CHECK(!P.send_frame(P.ctx, gen, NULL, 10));
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 0));
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 521));
}

/* a disconnect and a new connection on the SAME handle while a frame is in transmission */
static void disc_in_notify(void)
{
    F.phone_handle = DSP_BLE_TR_NO_HANDLE;
    dsp_ble_tr_on_disconnect(&T, 4);
}
/* robustness beyond the invariant: disconnect AND a new link on the same handle inside notify */
static void reconnect_in_notify(void)
{
    F.phone_handle = DSP_BLE_TR_NO_HANDLE;
    dsp_ble_tr_on_disconnect(&T, 4);
    F.phone_handle = 4;
    F.phone_link++;
    dsp_ble_reasm_init(&F.phone_rx);
    dsp_ble_tr_on_connect(&T, 4, true);
    dsp_ble_tr_on_subscribe(&T, 4, true);
}
static void test_disconnect_during_tx_and_stale(void)
{
    static uint8_t frame[400];
    fill(frame, sizeof frame, 77);
    reset_all();
    uint32_t g1 = open_link(4, 23);
    F.busy_next = 3;
    DSP_CHECK(P.send_frame(P.ctx, g1, frame, sizeof frame));
    drop_link(4);
    DSP_CHECK_EQ_INT(0, T.tx_count); /* buffers released at the disconnect */
    DSP_CHECK(!app.lnk.up);
    uint32_t g2 = open_link(4, 23); /* the handle is reused */
    DSP_CHECK(g2 > g1);
    int fr0 = F.frags;
    dsp_ble_tr_on_retry(&T);       /* stale callout */
    dsp_ble_tr_on_tx_done(&T, 4);  /* stale completion */
    DSP_CHECK_EQ_INT(fr0, F.frags); /* nothing of g1's frame reaches g2 */
    DSP_CHECK(!P.send_frame(P.ctx, g1, frame, 50)); /* stale generation */
    P.disconnect(P.ctx, g1);                         /* stale: no effect */
    DSP_CHECK_EQ_INT(0, F.terminates);
    DSP_CHECK(P.connected(P.ctx));
    /* disconnect during a notification (the stack reports it while the pump is in notify) */
    F.in_notify = disc_in_notify;
    DSP_CHECK(P.send_frame(P.ctx, g2, frame, sizeof frame));
    DSP_CHECK_EQ_INT(0, F.nframes);
    DSP_CHECK_EQ_INT(0, T.tx_count);
    uint32_t g3 = open_link(4, 23);
    dsp_ble_tr_on_retry(&T);
    DSP_CHECK_EQ_INT(0, F.nframes);
    DSP_CHECK_EQ_INT(0, F.frags);
    /* Beyond the invariant (a new link on the same handle while a notify is inside the stack):
     * the one fragment already handed over cannot be recalled (that is why advertising is
     * restarted only by the app task), but the ring stays consistent and nothing more of the
     * old frame follows. */
    F.busy_next = 0;
    F.in_notify = reconnect_in_notify;
    DSP_CHECK(P.send_frame(P.ctx, g3, frame, 10)); /* one fragment: completes in this step */
    DSP_CHECK(T.gen > g3 && P.connected(P.ctx));
    DSP_CHECK_EQ_INT(0, T.tx_count);
    DSP_CHECK_EQ_INT(0, (int)T.st.tx_frames); /* never counted as delivered */
    int inflight = F.frags;
    DSP_CHECK(inflight <= 1);
    dsp_ble_tr_on_retry(&T);
    dsp_ble_tr_on_tx_done(&T, 4);
    DSP_CHECK_EQ_INT(inflight, F.frags);
    F.frags = 0;
    F.nframes = 0;
    /* disconnect during RX: the partial frame never continues on the next connection */
    uint8_t f[20];
    memset(f, 3, sizeof f);
    f[0] = 0x80;
    (void)dsp_ble_tr_on_write(&T, 4, f, 20);
    drop_link(4);
    (void)open_link(4, 23);
    pop_all();
    f[0] = 0x41;
    (void)dsp_ble_tr_on_write(&T, 4, f, 5);
    DSP_CHECK_EQ_INT(0, (int)app.q_count);
    DSP_CHECK(app.lnk.faults >= 1); /* a continuation without FIRST on the new link */
    /* a write and a disconnect of another (stale) handle are ignored */
    uint32_t gen_now = T.gen;
    dsp_ble_tr_on_disconnect(&T, 99);
    DSP_CHECK(P.connected(P.ctx));
    DSP_CHECK_EQ_INT((int)gen_now, (int)T.gen);
    (void)g3;
}

static void test_lifecycle(void)
{
    reset_all();
    F.init_fail = true;
    DSP_CHECK(!P.start(P.ctx)); /* START_FAILED in the application */
    F.init_fail = false;
    DSP_CHECK(P.start(P.ctx));
    DSP_CHECK(!F.adv_on); /* advertising waits for the published payload */
    P.publish_info(P.ctx, INFO, ADV);
    DSP_CHECK(F.adv_on && F.adv_fast);
    DSP_CHECK(memcmp(F.advert, ADV, 4) == 0);
    uint8_t rd[32];
    INFO[0] = 0x5A;
    P.publish_info(P.ctx, INFO, ADV);
    dsp_ble_tr_on_info_read(&T, rd);
    DSP_CHECK(rd[0] == 0x5A);
    int starts = F.adv_starts;
    uint8_t adv2[4] = {1, 1, 0x12, 0x34};
    P.publish_info(P.ctx, INFO, adv2); /* payload changed: advertising restarted */
    DSP_CHECK_EQ_INT(starts + 1, F.adv_starts);
    DSP_CHECK(memcmp(F.advert, adv2, 4) == 0);
    /* fast for 30 s, then slow */
    P.wait(P.ctx, 60000);
    P.wait(P.ctx, 10);
    DSP_CHECK(F.adv_on && !F.adv_fast);
    /* connection: generation reported; advertising is restarted only by the app task */
    F.phone_handle = 1;
    dsp_ble_tr_on_connect(&T, 1, true);
    F.adv_on = false;
    DSP_CHECK(app.lnk.up && app.lnk.gen == T.gen && T.gen == 1);
    DSP_CHECK(P.connected(P.ctx));
    starts = F.adv_starts;
    drop_link(1);
    DSP_CHECK(!app.lnk.up);
    DSP_CHECK_EQ_INT(starts, F.adv_starts); /* not from the host task */
    P.wait(P.ctx, 10);
    DSP_CHECK_EQ_INT(starts + 1, F.adv_starts);
    DSP_CHECK(F.adv_fast); /* fast again after a disconnect */
    /* failed connection attempt: advertising restarted by the app task */
    dsp_ble_tr_on_connect(&T, 2, false);
    DSP_CHECK(!app.lnk.up);
    P.wait(P.ctx, 10);
    DSP_CHECK_EQ_INT(starts + 2, F.adv_starts);
    /* the stack ended advertising without a link (e.g. an error): restarted */
    dsp_ble_tr_on_adv_stopped(&T);
    P.wait(P.ctx, 10);
    DSP_CHECK_EQ_INT(starts + 3, F.adv_starts);
    /* advertising start failure: retried at the next wait */
    F.adv_on = false;
    dsp_ble_tr_on_adv_stopped(&T);
    T.advertising = false;
    T.adv_pending = true;
    F.adv_fail = true;
    P.wait(P.ctx, 10);
    DSP_CHECK(!F.adv_on);
    F.adv_fail = false;
    P.wait(P.ctx, 10);
    DSP_CHECK(F.adv_on);
    /* 1000 rapid connect/disconnect cycles: strictly increasing generations, last one wins */
    uint32_t prev = T.gen_counter;
    bool mono = true;
    for (int i = 0; i < 1000; i++) {
        F.phone_handle = (uint16_t)(i & 3);
        dsp_ble_tr_on_connect(&T, F.phone_handle, true);
        mono = mono && T.gen == prev + 1 && app.lnk.gen == T.gen && app.lnk.up;
        prev = T.gen;
        drop_link(F.phone_handle);
        mono = mono && !app.lnk.up;
        P.wait(P.ctx, 0);
    }
    DSP_CHECK(mono);
    /* a second simultaneous link is refused (single connection, T1) and never reported */
    (void)open_link(1, 23);
    uint32_t g = app.lnk.gen;
    dsp_ble_tr_on_connect(&T, 2, true);
    DSP_CHECK_EQ_INT(2, F.last_term);
    DSP_CHECK(app.lnk.gen == g && app.lnk.up);
    DSP_CHECK_EQ_INT(1, (int)T.st.refused_conns);
    /* suspend with a timely GAP disconnect */
    int term0 = F.terminates;
    P.suspend(P.ctx);
    DSP_CHECK_EQ_INT(term0 + 1, F.terminates);
    DSP_CHECK(!app.lnk.up);
    DSP_CHECK_EQ_INT(0, (int)T.st.synth_disconnects);
    DSP_CHECK(!F.adv_on);
    DSP_CHECK(F.inited); /* T12: the controller stays enabled */
    /* T12: communication fully suspended: no send, no write reaches the application, no
     * advertising restart from wait() */
    uint8_t sf[9] = {0xC0, 1, 0, 1, 0, 1, 0, 0, 0};
    DSP_CHECK(!P.send_frame(P.ctx, g, sf + 1, 8));
    pop_all();
    (void)dsp_ble_tr_on_write(&T, 1, sf, sizeof sf);
    DSP_CHECK_EQ_INT(0, (int)app.q_count);
    int adv_before = F.adv_starts;
    P.wait(P.ctx, 100);
    DSP_CHECK_EQ_INT(adv_before, F.adv_starts);
    DSP_CHECK(!P.connected(P.ctx));
    /* a connection while suspended is refused, never reported */
    dsp_ble_tr_on_connect(&T, 3, true);
    DSP_CHECK(!app.lnk.up);
    DSP_CHECK_EQ_INT(3, F.last_term);
    dsp_ble_tr_on_disconnect(&T, 3);
    DSP_CHECK(P.resume(P.ctx));
    DSP_CHECK(F.adv_on && F.adv_fast);
    /* suspend WITHOUT a GAP disconnect: the transport reports it itself after the limit */
    (void)open_link(1, 23);
    F.term_ignored = true;
    uint32_t t0 = F.now;
    P.suspend(P.ctx);
    DSP_CHECK(F.now - t0 <= DSP_BLE_TR_CLOSE_WAIT_MS + DSP_BLE_TR_CLOSE_GRACE_MS);
    DSP_CHECK(!app.lnk.up);
    DSP_CHECK_EQ_INT(1, (int)T.st.synth_disconnects);
    dsp_ble_tr_on_disconnect(&T, 1); /* the late GAP event is stale */
    DSP_CHECK(!app.lnk.up);
    F.term_ignored = false;
    /* resume refused unless suspended; stop is idempotent; is_off after deinit */
    DSP_CHECK(P.resume(P.ctx));
    DSP_CHECK(!P.resume(P.ctx));
    (void)open_link(1, 23);
    P.stop(P.ctx);
    DSP_CHECK(P.is_off(P.ctx));
    DSP_CHECK_EQ_INT(1, F.deinits);
    P.stop(P.ctx);
    DSP_CHECK_EQ_INT(1, F.deinits);
    DSP_CHECK(!P.resume(P.ctx));
    /* start again after stop: a new window, generations keep increasing */
    uint32_t last = T.gen_counter;
    uint32_t g9 = open_link(6, 23);
    DSP_CHECK(g9 > last);
}

static void test_close_after_final_reply(void)
{
    static uint8_t frame[200];
    fill(frame, sizeof frame, 5);
    reset_all();
    uint32_t gen = open_link(8, 23);
    F.busy_next = 2;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame)); /* e.g. the END_SESSION reply */
    P.disconnect(P.ctx, gen);
    DSP_CHECK_EQ_INT(0, F.terminates);              /* not before the reply left */
    DSP_CHECK(!P.connected(P.ctx));                  /* but closing at once */
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 10)); /* nothing new is accepted */
    for (int i = 0; i < 20 && F.terminates == 0; i++) {
        P.wait(P.ctx, 50);
    }
    DSP_CHECK_EQ_INT(1, F.nframes);
    DSP_CHECK_EQ_INT(1, F.terminates);
    /* with nothing queued the terminate follows after the grace period */
    reset_all();
    gen = open_link(8, 23);
    P.disconnect(P.ctx, gen);
    DSP_CHECK(F.retry_armed);
    P.wait(P.ctx, DSP_BLE_TR_CLOSE_GRACE_MS);
    DSP_CHECK_EQ_INT(1, F.terminates);
}

/* Incident 2026-11-20 (F3 review): the app suspends BLE for a dose right after replying (here: the
 * SET_READINESS answer). The suspend path drains the reply first, then terminates; it never waits
 * longer than the close bound, also when the stack is busy. */
static void test_suspend_drains_a_just_sent_reply(void)
{
    static uint8_t frame[120];
    fill(frame, sizeof frame, 3);
    reset_all();
    uint32_t gen = open_link(8, 23);
    F.busy_next = 2; /* the stack is busy at first: the reply is retried */
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    uint32_t t0 = F.now;
    P.suspend(P.ctx);
    DSP_CHECK_EQ_INT(1, F.nframes);    /* the reply left before ... */
    DSP_CHECK_EQ_INT(1, F.terminates); /* ... the terminate */
    DSP_CHECK(F.now - t0 <= DSP_BLE_TR_CLOSE_WAIT_MS);
    DSP_CHECK(!P.connected(P.ctx));
    /* a stack that never accepts cannot hold the dose back beyond the close bound */
    reset_all();
    gen = open_link(8, 23);
    F.busy_always = true;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    t0 = F.now;
    P.suspend(P.ctx);
    DSP_CHECK(F.now - t0 <= DSP_BLE_TR_CLOSE_WAIT_MS);
    DSP_CHECK(!P.connected(P.ctx));
}

/* T13: the grace period before the close is best effort only.
 *  (a) a notification the stack accepted but the phone never received is indistinguishable to
 *      the transport: tx_frames counts submissions, and the close proceeds identically;
 *  (b) neither send_frame, disconnect nor the host-task callbacks ever wait;
 *  (c) a client that never drains cannot extend the close beyond the frame deadline (T5): the
 *      ring is released and the terminate forced;
 *  (d) a GAP disconnect during the grace period releases the ring and invalidates the
 *      generation; the stale grace callout never terminates or feeds the next connection on the
 *      same handle. */
static void test_close_grace_semantics(void)
{
    static uint8_t frame[200];
    fill(frame, sizeof frame, 9);
    /* (a) + (b) */
    reset_all();
    uint32_t gen = open_link(8, 23);
    F.air_loss = true;
    int waits = F.waits;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame)); /* e.g. the END_SESSION reply */
    P.disconnect(P.ctx, gen);
    DSP_CHECK_EQ_INT(waits, F.waits); /* the app task did not wait for anything */
    DSP_CHECK_EQ_INT(1, (int)T.st.tx_frames); /* submitted to the stack, not "delivered" */
    DSP_CHECK_EQ_INT(0, F.nframes);           /* the phone got nothing */
    DSP_CHECK(F.retry_armed);                 /* the grace runs on the host task's callout */
    DSP_CHECK_EQ_INT(0, F.terminates);
    F.now = F.retry_at;
    F.retry_armed = false;
    dsp_ble_tr_on_retry(&T); /* host-task callback */
    DSP_CHECK_EQ_INT(waits, F.waits);
    DSP_CHECK_EQ_INT(1, F.terminates); /* the same close as with a delivered reply */
    DSP_CHECK_EQ_INT(0, (int)T.st.tx_failures);

    /* (c) the stack never accepts: the close is bounded by the frame deadline */
    reset_all();
    gen = open_link(8, 23);
    F.term_ignored = true;
    F.busy_always = true;
    uint32_t t0 = F.now;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 40));
    P.disconnect(P.ctx, gen);
    DSP_CHECK_EQ_INT(0, F.terminates);
    for (int i = 0; i < 200 && F.terminates == 0; i++) {
        DSP_CHECK(F.retry_armed); /* every retry is asynchronous */
        F.now = F.retry_at;
        F.retry_armed = false;
        dsp_ble_tr_on_retry(&T);
    }
    DSP_CHECK_EQ_INT(1, F.terminates);
    DSP_CHECK(F.now - t0 <= DSP_BLE_TR_TX_DEADLINE_MS + DSP_BLE_TR_RETRY_MS);
    DSP_CHECK_EQ_INT(1, (int)T.st.tx_failures);
    DSP_CHECK_EQ_INT(0, T.tx_count); /* all outstanding buffers released */
    DSP_CHECK(!P.connected(P.ctx));
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 10));
    F.busy_always = false;
    F.term_ignored = false;

    /* (d) GAP disconnect during the grace period, then a new link on the same handle */
    reset_all();
    gen = open_link(8, 23);
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    P.disconnect(P.ctx, gen);
    DSP_CHECK_EQ_INT(1, F.nframes);
    DSP_CHECK(F.retry_armed && T.term_at_ms != 0); /* in the grace period */
    drop_link(8);                                  /* the phone (or supervision) closes first */
    DSP_CHECK(!T.conn && T.gen == 0 && T.tx_count == 0 && T.term_at_ms == 0);
    DSP_CHECK(!app.lnk.up);
    DSP_CHECK_EQ_INT(gen, app.lnk.gen);
    DSP_CHECK(!P.send_frame(P.ctx, gen, frame, 10)); /* the generation is invalid */
    uint32_t g2 = open_link(8, 23);
    DSP_CHECK(g2 > gen);
    int notifies = F.notifies;
    F.now = F.retry_at;
    F.retry_armed = false;
    dsp_ble_tr_on_retry(&T); /* the old grace callout fires late */
    DSP_CHECK(!F.retry_armed); /* nothing of the old close carries over to the new link */
    for (int i = 0; i < 5 && F.retry_armed; i++) {
        F.now = F.retry_at;
        F.retry_armed = false;
        dsp_ble_tr_on_retry(&T);
    }
    DSP_CHECK_EQ_INT(0, F.terminates);
    DSP_CHECK_EQ_INT(notifies, F.notifies);
    DSP_CHECK(P.connected(P.ctx));
    P.disconnect(P.ctx, gen); /* a stale close request is ignored as well */
    DSP_CHECK(P.connected(P.ctx));

    /* (d') GAP disconnect while queued replies are still draining (stack busy) */
    reset_all();
    gen = open_link(8, 23);
    F.busy_next = 1000;
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, sizeof frame));
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 30));
    DSP_CHECK(P.send_frame(P.ctx, gen, frame, 20));
    P.disconnect(P.ctx, gen);
    DSP_CHECK_EQ_INT(3, T.tx_count);
    drop_link(8);
    DSP_CHECK_EQ_INT(0, T.tx_count);
    F.busy_next = 0;
    g2 = open_link(8, 23);
    notifies = F.notifies;
    F.now = F.retry_at;
    F.retry_armed = false;
    dsp_ble_tr_on_retry(&T);
    DSP_CHECK_EQ_INT(notifies, F.notifies); /* nothing of the old link reaches the new one */
    DSP_CHECK_EQ_INT(0, F.terminates);
    DSP_CHECK_EQ_INT(0, F.nframes);
    (void)g2;
}

/* ---- Two threads -------------------------------------------------------------------------- */

static pthread_mutex_t wmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wcond = PTHREAD_COND_INITIALIZER;
static atomic_int stop_host;
static atomic_int violations;
static atomic_int frames_ok;
static atomic_uint cur_link; /* controller's link counter (0 = none) */
static atomic_int adv_flag;
static pthread_mutex_t qmtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t frag_link[2]; /* link on which the current partial frame started */

static uint32_t real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static uint32_t t_now(void *ctx)
{
    (void)ctx;
    return real_ms();
}
static void t_signal(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&wmtx);
    pthread_cond_broadcast(&wcond);
    pthread_mutex_unlock(&wmtx);
}
static void t_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    if (max_ms > 5) {
        max_ms = 5;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)max_ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&wmtx);
    (void)pthread_cond_timedwait(&wcond, &wmtx, &ts);
    pthread_mutex_unlock(&wmtx);
}
static atomic_int t_retry_armed;
static void t_retry_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
    atomic_store(&t_retry_armed, 1);
}
static bool t_adv_start(void *ctx, const uint8_t advert[4], bool fast)
{
    (void)ctx;
    (void)advert;
    (void)fast;
    atomic_store(&adv_flag, 1);
    return true;
}
static void t_adv_stop(void *ctx)
{
    (void)ctx;
    atomic_store(&adv_flag, 0);
}
static atomic_int term_req;
static void t_terminate(void *ctx, uint16_t handle)
{
    (void)ctx;
    (void)handle;
    atomic_store(&term_req, 1);
}
/* Frames carry the generation they were sent for in bytes 1..4 (the "phone" checks that every
 * fragment of a frame arrives on the link of that generation). */
static int t_notify(void *ctx, uint16_t handle, const uint8_t *frag, size_t len)
{
    (void)ctx;
    (void)handle;
    uint32_t link = atomic_load(&cur_link);
    if (link == 0) {
        return DSP_BLE_NOTIFY_ERR;
    }
    if ((rand() & 15) == 0) {
        return DSP_BLE_NOTIFY_BUSY;
    }
    if (frag[0] & 0x80) {
        uint32_t g = (uint32_t)frag[2] | ((uint32_t)frag[3] << 8) | ((uint32_t)frag[4] << 16) |
                     ((uint32_t)frag[5] << 24);
        if (g != link) {
            atomic_fetch_add(&violations, 1);
        }
        frag_link[0] = link;
    } else if (frag_link[0] != link) {
        atomic_fetch_add(&violations, 1);
    }
    if (frag[0] & 0x40) {
        atomic_fetch_add(&frames_ok, 1);
    }
    (void)len;
    return DSP_BLE_NOTIFY_OK;
}
static bool t_init(void *ctx)
{
    (void)ctx;
    return true;
}
static void t_deinit(void *ctx)
{
    (void)ctx;
}
static bool t_is_off(void *ctx)
{
    (void)ctx;
    return true;
}
static const dsp_ble_radio_t TRADIO = {NULL,       t_init,      t_deinit, t_is_off,    t_adv_start,
                                       t_adv_stop, t_terminate, t_notify, t_retry_arm, t_signal,
                                       t_wait,     t_now,       f_lock,   f_unlock};
static void q_lock_fn(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&qmtx);
}
static void q_unlock_fn(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&qmtx);
}
static const dsp_app_sync_t QSYNC = {NULL, q_lock_fn, q_unlock_fn};

/* The simulated NimBLE host task: connections only while advertising (as the stack), writes,
 * completions, retry callouts, terminations. */
static void *host_task(void *arg)
{
    (void)arg;
    unsigned seed = 1234;
    uint8_t frag[20];
    const uint16_t handle = 0; /* always the same handle: reuse is the dangerous case */
    bool open = false;
    while (!atomic_load(&stop_host)) {
        int r = rand_r(&seed) % 100;
        if (atomic_exchange(&term_req, 0) || (open && r < 3)) {
            if (open) {
                open = false;
                atomic_store(&cur_link, 0);
                dsp_ble_tr_on_disconnect(&T, handle);
            }
        } else if (!open && atomic_load(&adv_flag) && r < 30) {
            atomic_store(&adv_flag, 0); /* the stack stops advertising on a connection */
            open = true;
            dsp_ble_tr_on_connect(&T, handle, true);
            f_lock(NULL);
            uint32_t g = T.conn && T.handle == handle ? T.gen : 0; /* 0: refused link */
            f_unlock(NULL);
            atomic_store(&cur_link, g);
            dsp_ble_tr_on_mtu(&T, handle, 23);
            dsp_ble_tr_on_subscribe(&T, handle, true);
        } else if (open && r < 60) {
            frag[0] = 0xC0;
            memset(frag + 1, (uint8_t)r, sizeof frag - 1);
            (void)dsp_ble_tr_on_write(&T, handle, frag, sizeof frag);
        } else if (r < 80) {
            dsp_ble_tr_on_tx_done(&T, handle);
        } else if (atomic_exchange(&t_retry_armed, 0)) {
            dsp_ble_tr_on_retry(&T);
        }
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_two_threads(void)
{
    reset_all();
    acfg.sync = &QSYNC;
    dsp_ble_tr_init(&T, &TRADIO, &app, &P);
    atomic_store(&stop_host, 0);
    atomic_store(&violations, 0);
    atomic_store(&frames_ok, 0);
    atomic_store(&cur_link, 0);
    DSP_CHECK(P.start(P.ctx));
    P.publish_info(P.ctx, INFO, ADV);
    pthread_t th;
    pthread_create(&th, NULL, host_task, NULL);
    uint8_t frame[60];
    uint32_t end = real_ms() + 1500;
    int sent = 0, suspends = 0;
    while ((int32_t)(real_ms() - end) < 0) {
        /* the application task: the newest generation it knows (snapshot) */
        q_lock_fn(NULL);
        uint32_t gen = app.lnk.up ? app.lnk.gen : 0;
        app.q_head = 0;
        app.q_count = 0;
        q_unlock_fn(NULL);
        if (gen != 0) {
            memset(frame, 0, sizeof frame);
            frame[1] = (uint8_t)gen; /* byte 0 of the frame is fragment data byte 1 */
            frame[2] = (uint8_t)(gen >> 8);
            frame[3] = (uint8_t)(gen >> 16);
            frame[4] = (uint8_t)(gen >> 24);
            sent += P.send_frame(P.ctx, gen, frame, sizeof frame);
            if ((sent & 63) == 7) {
                P.disconnect(P.ctx, gen);
            }
        }
        if ((rand() & 255) == 0) {
            P.suspend(P.ctx);
            suspends++;
            (void)P.resume(P.ctx);
        }
        (void)P.connected(P.ctx);
        P.wait(P.ctx, 1);
    }
    atomic_store(&stop_host, 1);
    pthread_join(th, NULL);
    P.stop(P.ctx);
    printf("threads: %d frames queued, %d delivered, %d suspends, %u links\n", sent,
           atomic_load(&frames_ok), suspends, T.gen_counter);
    DSP_CHECK_EQ_INT(0, atomic_load(&violations));
    DSP_CHECK(atomic_load(&frames_ok) > 0);
    DSP_CHECK(T.gen_counter > 3);
}

int main(void)
{
    DSP_RUN(test_fragmentation_all_mtus);
    DSP_RUN(test_mtu_change_between_frames);
    DSP_RUN(test_rx_faults);
    DSP_RUN(test_fragment_fuzz);
    DSP_RUN(test_queue_refusal_no_plaintext);
    DSP_RUN(test_tx_backpressure);
    DSP_RUN(test_disconnect_during_tx_and_stale);
    DSP_RUN(test_lifecycle);
    DSP_RUN(test_close_after_final_reply);
    DSP_RUN(test_suspend_drains_a_just_sent_reply);
    DSP_RUN(test_close_grace_semantics);
    DSP_RUN(test_two_threads);
    return dsp_test_finish();
}
