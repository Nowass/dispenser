#ifndef DSP_BLE_TRANSPORT_H
#define DSP_BLE_TRANSPORT_H

/*
 * BLE byte transport (M1.10d.1; docs/BLE_TRANSPORT_DESIGN.md). Pure C, no ESP-IDF headers:
 * compiled into the firmware component and the host tests. It implements dsp_ble_port_t for the
 * Application Service over a small radio interface (dsp_ble_radio_t), implemented by the NimBLE
 * adapter (dsp_esp_ble.c) on the target and by fault-injecting fakes in the host tests.
 *
 * Boundaries: the transport moves complete frames only. RX: fragments are reassembled with the
 * shared codec (dsp_ble_reasm_*) and the frame is posted unchanged (dsp_app_post_frame); TX:
 * frames handed to send_frame are fragmented unchanged (dsp_ble_frag_*). It never parses a frame
 * body, never sends a protocol message of its own (no plaintext error or busy notice), never
 * includes the restricted security interface and never touches domain state. It reports
 * connection events and fragment errors into the Application Service's connection snapshot
 * (dsp_app_post_conn_event / dsp_app_post_rx_fault), which decides what they mean (T2, T3).
 *
 * Threads ("host task" = the NimBLE host task, "app task" = the Application Service's task):
 *  - dsp_ble_tr_on_*: host task only (GAP/GATT callbacks, the retry callout). They never block,
 *    never wait for the app task and never call the radio's blocking operations.
 *  - the dsp_ble_port_t functions: app task only.
 *  - All shared state is guarded by the radio's lock (short critical sections, never held
 *    across a radio call or a dsp_app_post_* call; the transport and queue locks never nest).
 *  - Notifications are sent by whichever task "owns the pump" (one at a time, claimed under
 *    the lock); retries after an exhausted buffer pool are asynchronous (completion event or
 *    the retry callout), bounded by a retry count and an absolute deadline per frame (T5).
 *  - Invariant (stale delivery): advertising is (re)started only from the app task. A new
 *    connection therefore cannot appear while the app task is inside a notification, and the
 *    host task processes connection events and its own notifications sequentially, so the
 *    generation check before each notification cannot be overtaken by a newer connection that
 *    reuses the connection handle.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_app.h"
#include "dsp_bleproto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_BLE_TR_MTU_PREFERRED 247u    /* T8: preferred ATT MTU; the negotiated one is used */
#define DSP_BLE_TR_TX_SLOTS 3u           /* frames queued for notification (response + events) */
#define DSP_BLE_TR_RETRY_MS 20u          /* T5: retry interval after an exhausted buffer pool */
#define DSP_BLE_TR_RETRY_MAX 25u         /* T5: consecutive refusals per frame, then close */
#define DSP_BLE_TR_TX_DEADLINE_MS 1000u  /* T5: absolute limit per frame from send_frame */
#define DSP_BLE_TR_CLOSE_GRACE_MS 100u   /* T13: best effort only: lets a final reply leave the
                                            controller before the terminate; never a delivery
                                            confirmation (notifications are unacknowledged) */
#define DSP_BLE_TR_CLOSE_WAIT_MS 1500u   /* T14: suspend/stop wait for the GAP disconnect (longer
                                            than the drain bound TX_DEADLINE + GRACE, so a close
                                            normally completes first); then synthesized */
#define DSP_BLE_TR_FAST_ADV_MS 30000u    /* T4: fast advertising after start/disconnect */
#define DSP_BLE_TR_RX_FAULT_LIMIT 8u     /* T14: backstop only. Fragment (reassembly) errors of
                                            one connection, counted on the host task, reset per
                                            connection; reached only when the application task
                                            has not acted on the earlier ones. The effective
                                            policy is the application's (3 before a handshake,
                                            1 after); this limit is greater by construction. */
#define DSP_BLE_TR_NO_HANDLE 0xFFFFu

enum {
    DSP_BLE_NOTIFY_OK = 0,   /* queued to the stack (the stack owns its buffer now); NOT a
                                delivery confirmation (T13) */
    DSP_BLE_NOTIFY_BUSY = 1, /* no buffer: retry later */
    DSP_BLE_NOTIFY_ERR = 2,  /* not connected or another failure: the frame fails */
};

/* Results of dsp_ble_tr_on_write (mapped to ATT errors by the adapter). */
enum {
    DSP_BLE_WRITE_OK = 0,
    DSP_BLE_WRITE_BAD_LEN = 1, /* longer than one fragment (e.g. a long/prepared write) */
};

typedef struct {
    void *ctx;
    bool (*init)(void *ctx);      /* controller + host enabled, GATT registered, host synced */
    void (*deinit)(void *ctx);    /* host stopped, controller disabled and its memory returned */
    bool (*is_off)(void *ctx);    /* controller disabled (radio off) */
    /* Connectable undirected legacy advertising with the 4-byte service-data payload; fast =
     * the fast interval (T4). Restarts if already advertising. */
    bool (*adv_start)(void *ctx, const uint8_t advert[DSP_BLE_ADVERT_LEN], bool fast);
    void (*adv_stop)(void *ctx);
    void (*terminate)(void *ctx, uint16_t handle); /* GAP terminate; non-blocking */
    int (*notify)(void *ctx, uint16_t handle, const uint8_t *frag, size_t len); /* non-blocking */
    /* One-shot: after ms, dsp_ble_tr_on_retry runs in the HOST TASK (re-arming replaces). */
    void (*retry_arm)(void *ctx, uint32_t ms);
    void (*signal)(void *ctx);                    /* wakes wait() */
    void (*wait)(void *ctx, uint32_t max_ms);     /* app task; returns early on signal */
    uint32_t (*now_ms)(void *ctx);                /* monotonic */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} dsp_ble_radio_t;

/* Per-window diagnostics (not persisted; contention evidence for M1.10d.2, T1). */
typedef struct {
    uint32_t connections;      /* GAP connections accepted */
    uint32_t refused_conns;    /* connections terminated at once (not accepting, second link) */
    uint32_t disconnects;
    uint32_t synth_disconnects; /* GAP did not report the disconnect in time (suspend/stop) */
    uint32_t rx_frames;        /* complete frames posted */
    uint32_t rx_refused;       /* complete frames refused by the queue (dropped) */
    uint32_t rx_faults;        /* fragment errors */
    uint32_t rx_ignored;       /* writes of a closing/unknown connection */
    uint32_t rx_fault_closes;  /* DSP_BLE_TR_RX_FAULT_LIMIT reached */
    uint32_t tx_frames;        /* frames fully handed to the stack (not delivered: T13) */
    uint32_t tx_refused;       /* send_frame refused (stale gen, closing, full, unsubscribed) */
    uint32_t tx_unsubscribed;  /* ... of which: the client had not enabled notifications */
    uint32_t tx_busy;          /* buffer-pool refusals (retried) */
    uint32_t tx_failures;      /* frames abandoned (retry limit, deadline, error): closed */
    uint32_t adv_starts;
    uint32_t adv_failures;
} dsp_ble_tr_stats_t;

typedef struct {
    uint32_t gen;
    uint16_t len;
    uint16_t mtu;      /* captured when the first fragment is sent */
    uint8_t index;
    uint8_t count;
    uint8_t busy;      /* consecutive refusals */
    bool started;
    uint32_t deadline_ms;
    uint8_t bytes[DSP_BLE_FRAME_MAX];
} dsp_ble_tr_tx_t;

typedef struct {
    const dsp_ble_radio_t *radio;
    dsp_app_t *app;
    /* window and advertising (app task writes; host task reads/clears under the lock) */
    uint8_t win;               /* internal: OFF, ACTIVE, SUSPENDED */
    bool inited;               /* radio initialized (app task only) */
    bool advertising;
    bool adv_fast;
    bool adv_pending;          /* (re)start advertising at the next app-task opportunity */
    uint32_t adv_seq;
    uint32_t fast_until_ms;
    /* connection record */
    uint32_t gen_counter;      /* generations: strictly increasing, never 0 */
    bool conn;
    bool closing;              /* no further writes or sends; terminate requested or pending */
    bool term_pending;         /* terminate once the TX ring drained (+ grace) */
    bool term_sent;
    uint32_t term_at_ms;       /* 0 = grace not started */
    uint16_t handle;
    uint32_t gen;              /* 0 = connection not reported (refused) */
    uint16_t mtu;
    bool subscribed;
    uint8_t rx_faults;
    /* RX (host task only) */
    dsp_ble_reasm_t reasm;
    uint32_t reasm_gen;
    /* TX ring */
    dsp_ble_tr_tx_t tx[DSP_BLE_TR_TX_SLOTS];
    uint8_t tx_head, tx_count;
    uint32_t tx_epoch;         /* incremented whenever the ring is flushed */
    bool pumping, pump_again;
    uint8_t frag[DSP_BLE_TR_MTU_PREFERRED - 3u];
    /* published by the application */
    uint8_t info[DSP_BLE_INFO_LEN];
    uint8_t advert[DSP_BLE_ADVERT_LEN];
    dsp_ble_tr_stats_t st;
} dsp_ble_transport_t;

/* Binds the transport to the radio and the Application Service (static storage, no
 * allocation) and fills *port for dsp_app_config_t.ble (ctx = t). */
void dsp_ble_tr_init(dsp_ble_transport_t *t, const dsp_ble_radio_t *radio, dsp_app_t *app,
                     dsp_ble_port_t *port);

/* ---- Host task (NimBLE callbacks) ---- */
void dsp_ble_tr_on_connect(dsp_ble_transport_t *t, uint16_t handle, bool ok);
void dsp_ble_tr_on_disconnect(dsp_ble_transport_t *t, uint16_t handle);
void dsp_ble_tr_on_adv_stopped(dsp_ble_transport_t *t); /* advertising ended without a link */
void dsp_ble_tr_on_mtu(dsp_ble_transport_t *t, uint16_t handle, uint16_t mtu);
void dsp_ble_tr_on_subscribe(dsp_ble_transport_t *t, uint16_t handle, bool notify);
int dsp_ble_tr_on_write(dsp_ble_transport_t *t, uint16_t handle, const uint8_t *data, size_t len);
void dsp_ble_tr_on_info_read(dsp_ble_transport_t *t, uint8_t out[DSP_BLE_INFO_LEN]);
void dsp_ble_tr_on_tx_done(dsp_ble_transport_t *t, uint16_t handle);
void dsp_ble_tr_on_retry(dsp_ble_transport_t *t);

/* Diagnostics snapshot (any task). */
void dsp_ble_tr_stats(dsp_ble_transport_t *t, dsp_ble_tr_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BLE_TRANSPORT_H */
