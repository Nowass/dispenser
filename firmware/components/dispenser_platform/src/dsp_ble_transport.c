#include "dsp_ble_transport.h"

#include <string.h>

/* T14: the transport's fault limit is a backstop and never closes a connection that the
 * application's policy (DSP_LINK_ERRORS_MAX before a handshake, one error after) keeps open. */
_Static_assert(DSP_BLE_TR_RX_FAULT_LIMIT > DSP_LINK_ERRORS_MAX,
               "the transport's fault backstop must exceed the application's limit");
_Static_assert(DSP_BLE_TR_CLOSE_WAIT_MS > DSP_BLE_TR_TX_DEADLINE_MS + DSP_BLE_TR_CLOSE_GRACE_MS,
               "suspend/stop must outlast a bounded drain before synthesizing the disconnect");

/* docs/BLE_TRANSPORT_DESIGN.md sections 2, 5-9. Pure C: no ESP-IDF headers. */

enum { WIN_OFF = 0, WIN_ACTIVE, WIN_SUSPENDED };

static void lock(dsp_ble_transport_t *t)
{
    t->radio->lock(t->radio->ctx);
}

static void unlock(dsp_ble_transport_t *t)
{
    t->radio->unlock(t->radio->ctx);
}

static uint32_t now_ms(dsp_ble_transport_t *t)
{
    return t->radio->now_ms(t->radio->ctx);
}

static bool reached(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0; /* wrap-safe */
}

/* Locked: drops every queued frame (their buffers are free again). */
static void tx_flush(dsp_ble_transport_t *t)
{
    t->tx_head = 0;
    t->tx_count = 0;
    t->tx_epoch++;
}

/* Locked: forgets the connection record. */
static void conn_clear(dsp_ble_transport_t *t)
{
    t->conn = false;
    t->closing = false;
    t->term_pending = false;
    t->term_sent = false;
    t->term_at_ms = 0;
    t->handle = DSP_BLE_TR_NO_HANDLE;
    t->gen = 0;
    t->mtu = DSP_BLE_ATT_MTU_MIN;
    t->subscribed = false;
    t->rx_faults = 0;
    tx_flush(t);
}

/* ---- TX pump (either task; one owner at a time) ---------------------------------------- */

static void pump(dsp_ble_transport_t *t)
{
    const dsp_ble_radio_t *r = t->radio;
    uint16_t term = DSP_BLE_TR_NO_HANDLE;
    uint32_t arm_ms = 0;
    bool progressed = false; /* a frame completed: wake the app task (suspend/close waits) */
    lock(t);
    if (t->pumping) {
        t->pump_again = true; /* the owner runs once more (e.g. a completion arrived) */
        unlock(t);
        return;
    }
    t->pumping = true;
    for (;;) {
        t->pump_again = false;
        if (!t->conn || t->tx_count == 0) {
            if (t->conn && t->closing && t->term_pending && !t->term_sent) {
                uint32_t now = now_ms(t);
                if (t->term_at_ms == 0) {
                    t->term_at_ms = now + DSP_BLE_TR_CLOSE_GRACE_MS;
                    if (t->term_at_ms == 0) {
                        t->term_at_ms = 1;
                    }
                    arm_ms = DSP_BLE_TR_CLOSE_GRACE_MS;
                } else if (reached(now, t->term_at_ms)) {
                    t->term_sent = true;
                    term = t->handle;
                } else {
                    arm_ms = t->term_at_ms - now;
                }
            }
            if (t->pump_again) {
                continue;
            }
            break;
        }
        dsp_ble_tr_tx_t *s = &t->tx[t->tx_head];
        bool fail = false;
        if (s->gen != t->gen) {
            fail = true; /* cannot happen (flushed at every change); never sent to another link */
        } else {
            if (!s->started) {
                s->started = true;
                s->mtu = t->mtu; /* T8: the negotiated MTU when the frame starts */
                size_t n = dsp_ble_frag_count(s->len, s->mtu);
                s->count = (uint8_t)n;
                fail = n == 0 || n > 64;
            }
            if (!fail && reached(now_ms(t), s->deadline_ms)) {
                fail = true; /* T5: absolute deadline */
            }
        }
        if (fail) {
            t->st.tx_failures++;
            tx_flush(t);
            t->closing = true;
            if (!t->term_sent) {
                t->term_sent = true;
                term = t->handle;
            }
            break;
        }
        size_t n = dsp_ble_frag_get(s->bytes, s->len, s->mtu, s->index, t->frag, sizeof t->frag);
        uint16_t handle = t->handle;
        uint32_t epoch = t->tx_epoch;
        unlock(t);
        int rc = n > 0 ? r->notify(r->ctx, handle, t->frag, n) : DSP_BLE_NOTIFY_ERR;
        lock(t);
        if (epoch != t->tx_epoch || !t->conn) {
            continue; /* the connection ended meanwhile: the ring was flushed */
        }
        if (rc == DSP_BLE_NOTIFY_OK) {
            s->busy = 0;
            if (++s->index >= s->count) {
                t->tx_head = (uint8_t)((t->tx_head + 1u) % DSP_BLE_TR_TX_SLOTS);
                t->tx_count--;
                t->st.tx_frames++;
                progressed = true;
            }
            continue;
        }
        if (rc == DSP_BLE_NOTIFY_BUSY && s->busy < DSP_BLE_TR_RETRY_MAX) {
            s->busy++;
            t->st.tx_busy++;
            if (t->pump_again) {
                continue; /* a completion freed a buffer while we tried */
            }
            arm_ms = DSP_BLE_TR_RETRY_MS; /* asynchronous retry: nothing waits here */
            break;
        }
        t->st.tx_failures++; /* retry limit or an error: the client is gone or not draining */
        tx_flush(t);
        t->closing = true;
        if (!t->term_sent) {
            t->term_sent = true;
            term = t->handle;
        }
        break;
    }
    t->pumping = false;
    unlock(t);
    if (arm_ms != 0) {
        r->retry_arm(r->ctx, arm_ms);
    }
    if (term != DSP_BLE_TR_NO_HANDLE) {
        r->terminate(r->ctx, term);
    }
    if (progressed || term != DSP_BLE_TR_NO_HANDLE) {
        r->signal(r->ctx);
    }
}

/* ---- Advertising (app task only: the stale-delivery invariant) -------------------------- */

static void adv_run(dsp_ble_transport_t *t, bool reset_fast)
{
    const dsp_ble_radio_t *r = t->radio;
    uint8_t advert[DSP_BLE_ADVERT_LEN];
    lock(t);
    bool go = t->win == WIN_ACTIVE && !t->conn && t->inited;
    uint32_t now = now_ms(t);
    if (go) {
        if (reset_fast) {
            t->fast_until_ms = now + DSP_BLE_TR_FAST_ADV_MS;
            t->adv_fast = true;
        } else if (t->adv_fast && reached(now, t->fast_until_ms)) {
            t->adv_fast = false;
        }
        t->advertising = true;
        t->adv_pending = false;
        t->adv_seq++;
        memcpy(advert, t->advert, sizeof advert);
    } else {
        t->adv_pending = t->adv_pending && t->win == WIN_ACTIVE;
    }
    uint32_t seq = t->adv_seq;
    bool fast = t->adv_fast;
    unlock(t);
    if (!go) {
        return;
    }
    bool ok = r->adv_start(r->ctx, advert, fast);
    lock(t);
    if (ok) {
        t->st.adv_starts++;
    } else {
        t->st.adv_failures++;
        if (t->adv_seq == seq && !t->conn) {
            t->advertising = false;
            t->adv_pending = t->win == WIN_ACTIVE; /* retried at the next wait() */
        }
    }
    unlock(t);
}

static void adv_halt(dsp_ble_transport_t *t)
{
    lock(t);
    bool was = t->advertising;
    t->advertising = false;
    t->adv_pending = false;
    t->adv_seq++;
    unlock(t);
    if (was) {
        t->radio->adv_stop(t->radio->ctx);
    }
}

/* ---- Host-task callbacks ---------------------------------------------------------------- */

void dsp_ble_tr_on_connect(dsp_ble_transport_t *t, uint16_t handle, bool ok)
{
    uint32_t gen = 0;
    bool refuse = false;
    lock(t);
    t->advertising = false; /* the stack stops advertising on a connection or a failure */
    t->adv_seq++;
    if (!ok) {
        t->adv_pending = t->win == WIN_ACTIVE;
    } else if (t->conn || t->win != WIN_ACTIVE) {
        refuse = true; /* single connection (T1) / not accepting: close the new link at once */
        t->st.refused_conns++;
        if (!t->conn) {
            conn_clear(t);
            t->conn = true;
            t->closing = true;
            t->term_sent = true;
            t->handle = handle; /* gen 0: never reported to the application */
        }
    } else {
        conn_clear(t);
        if (++t->gen_counter == 0) {
            t->gen_counter = 1;
        }
        gen = t->gen_counter;
        t->conn = true;
        t->handle = handle;
        t->gen = gen;
        t->st.connections++;
    }
    unlock(t);
    if (refuse) {
        t->radio->terminate(t->radio->ctx, handle);
        return;
    }
    if (gen != 0) {
        (void)dsp_app_post_conn_event(t->app, true, gen, DSP_BLE_ATT_MTU_MIN);
    }
    t->radio->signal(t->radio->ctx);
}

void dsp_ble_tr_on_disconnect(dsp_ble_transport_t *t, uint16_t handle)
{
    uint32_t gen = 0;
    bool mine;
    lock(t);
    mine = t->conn && t->handle == handle;
    if (mine) {
        gen = t->gen;
        conn_clear(t); /* the TX ring and its buffers are released */
        t->st.disconnects++;
        t->adv_pending = t->win == WIN_ACTIVE; /* restarted by the app task (invariant) */
    }
    unlock(t);
    if (!mine) {
        return; /* stale handle */
    }
    if (gen != 0) {
        (void)dsp_app_post_conn_event(t->app, false, gen, 0);
    }
    t->radio->signal(t->radio->ctx);
}

void dsp_ble_tr_on_adv_stopped(dsp_ble_transport_t *t)
{
    lock(t);
    bool restart = t->advertising && !t->conn && t->win == WIN_ACTIVE;
    t->advertising = false;
    t->adv_seq++;
    if (restart) {
        t->adv_pending = true;
    }
    unlock(t);
    if (restart) {
        t->radio->signal(t->radio->ctx);
    }
}

void dsp_ble_tr_on_mtu(dsp_ble_transport_t *t, uint16_t handle, uint16_t mtu)
{
    lock(t);
    if (t->conn && t->handle == handle && mtu >= DSP_BLE_ATT_MTU_MIN) {
        t->mtu = mtu > DSP_BLE_TR_MTU_PREFERRED ? (uint16_t)DSP_BLE_TR_MTU_PREFERRED : mtu;
    }
    unlock(t);
}

void dsp_ble_tr_on_subscribe(dsp_ble_transport_t *t, uint16_t handle, bool notify)
{
    lock(t);
    if (t->conn && t->handle == handle) {
        t->subscribed = notify;
    }
    unlock(t);
}

/* Closes the current connection from the host task (fault limit). */
static void close_now(dsp_ble_transport_t *t, uint32_t gen)
{
    uint16_t term = DSP_BLE_TR_NO_HANDLE;
    lock(t);
    if (t->conn && t->gen == gen && !t->term_sent) {
        t->closing = true;
        t->term_sent = true;
        tx_flush(t);
        term = t->handle;
    }
    unlock(t);
    if (term != DSP_BLE_TR_NO_HANDLE) {
        t->radio->terminate(t->radio->ctx, term);
    }
}

int dsp_ble_tr_on_write(dsp_ble_transport_t *t, uint16_t handle, const uint8_t *data, size_t len)
{
    lock(t);
    bool mine = t->conn && t->handle == handle && !t->closing && t->gen != 0;
    uint32_t gen = t->gen;
    uint16_t mtu = t->mtu;
    if (!mine) {
        t->st.rx_ignored++;
    }
    unlock(t);
    if (!mine) {
        return DSP_BLE_WRITE_OK; /* the link is going away: the fragment is dropped */
    }
    if (len > (size_t)mtu - 3u) {
        return DSP_BLE_WRITE_BAD_LEN; /* only single-fragment writes (T10) */
    }
    if (t->reasm_gen != gen) {
        dsp_ble_reasm_init(&t->reasm); /* a new connection never continues an old frame */
        t->reasm_gen = gen;
    }
    bool complete = false;
    dsp_ble_result_t rr = dsp_ble_reasm_push(&t->reasm, data, len, now_ms(t), &complete);
    if (rr != DSP_BLE_OK) {
        lock(t);
        t->st.rx_faults++;
        uint8_t n = t->rx_faults < UINT8_MAX ? ++t->rx_faults : t->rx_faults;
        unlock(t);
        (void)dsp_app_post_rx_fault(t->app, gen); /* T3: the application decides the reply */
        if (n >= DSP_BLE_TR_RX_FAULT_LIMIT) {
            lock(t);
            t->st.rx_fault_closes++;
            unlock(t);
            close_now(t, gen);
        }
        t->radio->signal(t->radio->ctx);
        return DSP_BLE_WRITE_OK;
    }
    if (complete) {
        bool posted = dsp_app_post_frame(t->app, gen, t->reasm.buf, t->reasm.len);
        dsp_ble_reasm_init(&t->reasm);
        lock(t);
        if (posted) {
            t->st.rx_frames++;
        } else {
            t->st.rx_refused++; /* dropped: the client retries (response cache, C4) */
        }
        unlock(t);
        t->radio->signal(t->radio->ctx);
    }
    return DSP_BLE_WRITE_OK;
}

void dsp_ble_tr_on_info_read(dsp_ble_transport_t *t, uint8_t out[DSP_BLE_INFO_LEN])
{
    lock(t);
    memcpy(out, t->info, DSP_BLE_INFO_LEN);
    unlock(t);
}

void dsp_ble_tr_on_tx_done(dsp_ble_transport_t *t, uint16_t handle)
{
    (void)handle; /* a stale completion only re-runs the pump, which checks the generation */
    pump(t);
}

void dsp_ble_tr_on_retry(dsp_ble_transport_t *t)
{
    pump(t);
}

void dsp_ble_tr_stats(dsp_ble_transport_t *t, dsp_ble_tr_stats_t *out)
{
    lock(t);
    *out = t->st;
    unlock(t);
}

/* ---- dsp_ble_port_t (app task) ---------------------------------------------------------- */

static bool p_start(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    if (!t->inited) {
        if (!t->radio->init(t->radio->ctx)) {
            return false;
        }
        t->inited = true;
    }
    lock(t);
    conn_clear(t);
    memset(&t->st, 0, sizeof t->st);
    t->win = WIN_ACTIVE;
    t->adv_pending = true; /* advertising starts with the first published payload or wait() */
    t->advertising = false;
    t->fast_until_ms = now_ms(t) + DSP_BLE_TR_FAST_ADV_MS;
    t->adv_fast = true;
    unlock(t);
    return true;
}

static bool p_connected(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    bool c = t->conn && !t->closing && t->gen != 0;
    unlock(t);
    return c;
}

/* Requests the close of the current connection: replies already queued are sent first (with a
 * short grace), unless the frame deadline expires (T5). */
static void close_after_drain(dsp_ble_transport_t *t, uint32_t gen)
{
    lock(t);
    bool go = t->conn && !t->closing && (gen == 0 || t->gen == gen);
    if (go) {
        t->closing = true;
        t->term_pending = true;
    }
    unlock(t);
    if (go) {
        pump(t);
    }
}

/* Waits (app task, no lock held) until the connection is gone or the limit passed; then the
 * transport reports the disconnect itself and forces the terminate. */
static void close_and_wait(dsp_ble_transport_t *t, bool report)
{
    lock(t);
    bool c = t->conn;
    uint32_t gen = t->gen;
    unlock(t);
    if (!c) {
        return;
    }
    close_after_drain(t, 0);
    uint32_t start = now_ms(t);
    for (;;) {
        lock(t);
        bool still = t->conn;
        unlock(t);
        uint32_t el = now_ms(t) - start;
        if (!still || el >= DSP_BLE_TR_CLOSE_WAIT_MS) {
            break;
        }
        t->radio->wait(t->radio->ctx, DSP_BLE_TR_CLOSE_WAIT_MS - el);
    }
    uint16_t term = DSP_BLE_TR_NO_HANDLE;
    bool post = false;
    lock(t);
    if (t->conn && t->gen == gen) {
        term = t->handle;
        post = report && gen != 0;
        conn_clear(t);
        t->st.synth_disconnects++;
    }
    unlock(t);
    if (term != DSP_BLE_TR_NO_HANDLE) {
        t->radio->terminate(t->radio->ctx, term);
    }
    if (post) {
        (void)dsp_app_post_conn_event(t->app, false, gen, 0);
    }
}

static void p_suspend(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    if (t->win == WIN_ACTIVE) {
        t->win = WIN_SUSPENDED; /* no new connection is accepted from here on */
    }
    unlock(t);
    adv_halt(t);
    close_and_wait(t, true); /* T12: the controller stays enabled; no RF activity */
}

static bool p_resume(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    bool ok = t->inited && t->win == WIN_SUSPENDED;
    if (ok) {
        t->win = WIN_ACTIVE;
    }
    unlock(t);
    if (!ok) {
        return false;
    }
    adv_run(t, true);
    lock(t);
    ok = t->advertising || t->conn;
    unlock(t);
    return ok;
}

static void p_stop(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    t->win = WIN_OFF;
    unlock(t);
    adv_halt(t);
    close_and_wait(t, false); /* the queue is closed already: nothing to report */
    lock(t);
    conn_clear(t);
    unlock(t);
    if (t->inited) {
        t->radio->deinit(t->radio->ctx); /* controller disabled before Wi-Fi or sleep */
        t->inited = false;
    }
}

static bool p_is_off(void *ctx)
{
    dsp_ble_transport_t *t = ctx;
    return !t->inited && t->radio->is_off(t->radio->ctx);
}

static void p_wait(void *ctx, uint32_t max_ms)
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    uint32_t now = now_ms(t);
    bool restart = t->win == WIN_ACTIVE && !t->conn &&
                   (t->adv_pending || (t->advertising && t->adv_fast && reached(now, t->fast_until_ms)));
    bool reset_fast = t->adv_pending;
    unlock(t);
    if (restart) {
        adv_run(t, reset_fast); /* T4: fast after start and after every disconnect, then slow */
    }
    lock(t);
    now = now_ms(t);
    uint32_t slice = max_ms;
    if (t->advertising && t->adv_fast && !reached(now, t->fast_until_ms)) {
        uint32_t left = t->fast_until_ms - now;
        slice = left < slice ? left : slice;
    }
    unlock(t);
    t->radio->wait(t->radio->ctx, slice);
}

static bool p_send_frame(void *ctx, uint32_t gen, const uint8_t *frame, size_t len)
{
    dsp_ble_transport_t *t = ctx;
    if (frame == NULL || len == 0 || len > DSP_BLE_FRAME_MAX) {
        return false;
    }
    lock(t);
    bool ok = t->win == WIN_ACTIVE && t->conn && !t->closing && gen != 0 && gen == t->gen &&
              t->subscribed && t->tx_count < DSP_BLE_TR_TX_SLOTS;
    if (ok) {
        dsp_ble_tr_tx_t *s = &t->tx[(t->tx_head + t->tx_count) % DSP_BLE_TR_TX_SLOTS];
        s->gen = gen;
        s->len = (uint16_t)len;
        s->index = 0;
        s->count = 0;
        s->busy = 0;
        s->started = false;
        s->deadline_ms = now_ms(t) + DSP_BLE_TR_TX_DEADLINE_MS;
        memcpy(s->bytes, frame, len);
        t->tx_count++;
    } else {
        t->st.tx_refused++;
        if (t->conn && gen == t->gen && !t->subscribed) {
            t->st.tx_unsubscribed++;
        }
    }
    unlock(t);
    if (ok) {
        pump(t);
    }
    return ok;
}

static void p_disconnect(void *ctx, uint32_t gen)
{
    dsp_ble_transport_t *t = ctx;
    if (gen != 0) {
        close_after_drain(t, gen); /* a final reply (e.g. END_SESSION) is sent first */
    }
}

static void p_publish_info(void *ctx, const uint8_t info[DSP_BLE_INFO_LEN],
                           const uint8_t advert[DSP_BLE_ADVERT_LEN])
{
    dsp_ble_transport_t *t = ctx;
    lock(t);
    memcpy(t->info, info, DSP_BLE_INFO_LEN);
    bool changed = memcmp(t->advert, advert, DSP_BLE_ADVERT_LEN) != 0;
    memcpy(t->advert, advert, DSP_BLE_ADVERT_LEN);
    bool restart = t->win == WIN_ACTIVE && !t->conn && (t->adv_pending || (changed && t->advertising));
    unlock(t);
    if (restart) {
        adv_run(t, false);
    }
}

void dsp_ble_tr_init(dsp_ble_transport_t *t, const dsp_ble_radio_t *radio, dsp_app_t *app,
                     dsp_ble_port_t *port)
{
    memset(t, 0, sizeof *t);
    t->radio = radio;
    t->app = app;
    t->handle = DSP_BLE_TR_NO_HANDLE;
    t->mtu = DSP_BLE_ATT_MTU_MIN;
    memset(port, 0, sizeof *port);
    port->ctx = t;
    port->start = p_start;
    port->connected = p_connected;
    port->suspend = p_suspend;
    port->resume = p_resume;
    port->stop = p_stop;
    port->is_off = p_is_off;
    port->wait = p_wait;
    port->send_frame = p_send_frame;
    port->disconnect = p_disconnect;
    port->publish_info = p_publish_info;
}
