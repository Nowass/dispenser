/*
 * M1.10c.2a: the BLE security session end to end on the host. A "phone" built from the same pinned
 * noise-c (initiator) talks to dsp_blesec (responder) through the real Application Service, inside
 * the test_app_ops harness (complete wakes, fake storage and pump, simulated time). Randomness is
 * the deterministic TEST implementation registered with libsodium (support/test_rng.c); firmware
 * never contains it.
 *
 * This is host interoperability and logic verification. It says nothing about the target RNG,
 * BLE runtime, timing, stack or heap on the ESP32-C3 (M1.10c.2b hardware acceptance).
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#include <noise/protocol.h>
#include <sodium.h>

#include "dsp_blesec.h"
#include "dsp_secderive.h"
#include "test_rng.h"

static op_t hk(void (*h)(int), int a, int b)
{
    op_t o = mk(OP_HOOK);
    o.hook = h;
    o.a = a;
    o.b = b;
    return o;
}

/* ---- Device side: the security layer under test ------------------------------------------- */

static dsp_blesec_t SECS;
static dsp_app_security_t BSEC;
static bool g_entropy = true;
static uint8_t g_dev_setup[16];
static const uint8_t DEV_ID[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

static bool cfg_entropy(void *ctx)
{
    (void)ctx;
    return g_entropy;
}
static bool cfg_setup(void *ctx, uint8_t out[16])
{
    (void)ctx;
    memcpy(out, g_dev_setup, 16);
    return true;
}

/* ---- Transport capture ------------------------------------------------------------------------ */

#define INBOX 96
static struct {
    uint8_t f[INBOX][DSP_BLE_FRAME_MAX];
    size_t n[INBOX];
    int head, count;
} inbox;

static void phone_on_disconnect(void);

/* T13: frames accepted for sending but lost on the air (a notification has no ATT
 * acknowledgement). Through the transport, the transport behaves exactly as for delivered ones. */
static bool g_air_loss;
static bool cap_send(void *ctx, uint32_t gen, const uint8_t *f, size_t n)
{
    if (!ble_send_frame(ctx, gen, f, n)) {
        return false;
    }
    if (g_air_loss) {
        return true;
    }
    if (inbox.count < INBOX) {
        int i = (inbox.head + inbox.count) % INBOX;
        memcpy(inbox.f[i], f, n);
        inbox.n[i] = n;
        inbox.count++;
    }
    return true;
}
static void cap_disconnect(void *ctx, uint32_t gen)
{
    phone_on_disconnect();
    ble_disconnect(ctx, gen); /* the harness reconnects at once (new generation) */
}
static dsp_ble_port_t BLE2;

#ifdef DSP_VIA_TRANSPORT
/* ---- M1.10d.1 end to end: the whole suite through the BLE byte transport --------------------
 * The phone's frames are fragmented at the negotiated MTU (DSP_E2E_MTU) into ATT writes; the
 * device's frames come back as notifications and are reassembled by the phone. The fake radio
 * connects the phone whenever the transport starts advertising (app task, as the invariant
 * requires) and delivers the GAP disconnect when the transport terminates the link. */
#include <stdlib.h>

#include "dsp_ble_transport.h"

static dsp_ble_transport_t TR;
static dsp_ble_port_t TRP;
static uint16_t e2e_mtu = 23;
static bool e2e_conn;
static bool e2e_quiet;      /* suspend/stop: a radio-side close, not a protocol drop */
static uint64_t e2e_extra;  /* milliseconds added by fired callouts */
static bool e2e_armed;
static uint32_t e2e_arm_ms;
static dsp_ble_reasm_t e2e_rx;
static int e2e_frags_out, e2e_writes;

static uint32_t e2e_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(mono_ms + e2e_extra);
}
static bool e2e_init(void *ctx)
{
    (void)ctx;
    return true;
}
static void e2e_deinit(void *ctx)
{
    (void)ctx;
}
static bool e2e_is_off(void *ctx)
{
    (void)ctx;
    return true;
}
static bool e2e_adv_start(void *ctx, const uint8_t advert[4], bool fast)
{
    (void)ctx;
    (void)advert;
    (void)fast;
    if (!e2e_conn) { /* the phone is in range and connects at once */
        e2e_conn = true;
        dsp_ble_reasm_init(&e2e_rx);
        dsp_ble_tr_on_connect(&TR, 1, true);
        dsp_ble_tr_on_mtu(&TR, 1, e2e_mtu);
        dsp_ble_tr_on_subscribe(&TR, 1, true);
        sc.gen = TR.gen;
    }
    return true;
}
static void e2e_adv_stop(void *ctx)
{
    (void)ctx;
}
static bool e2e_term_ignored; /* the controller never reports the GAP disconnect (T14 wait) */
static int e2e_terms_ignored;
static void e2e_terminate(void *ctx, uint16_t handle)
{
    if (!e2e_conn) {
        return;
    }
    if (e2e_term_ignored) {
        e2e_terms_ignored++;
        return;
    }
    if (!e2e_quiet) {
        sc.disconnects++;
        phone_on_disconnect();
    }
    e2e_conn = false;
    dsp_ble_tr_on_disconnect(&TR, handle);
    (void)ctx;
}
static int e2e_notify(void *ctx, uint16_t handle, const uint8_t *frag, size_t len)
{
    if (!e2e_conn || handle != 1 || len > (size_t)e2e_mtu - 3u) {
        return DSP_BLE_NOTIFY_ERR;
    }
    e2e_frags_out++;
    bool complete = false;
    if (dsp_ble_reasm_push(&e2e_rx, frag, len, e2e_now(NULL), &complete) == DSP_BLE_OK && complete) {
        (void)cap_send(ctx, sc.gen, e2e_rx.buf, e2e_rx.len);
        dsp_ble_reasm_init(&e2e_rx);
    }
    return DSP_BLE_NOTIFY_OK;
}
static void e2e_retry_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    e2e_armed = true;
    e2e_arm_ms = ms;
}
static void e2e_signal(void *ctx)
{
    (void)ctx;
}
static void e2e_wait(void *ctx, uint32_t max_ms)
{
    if (e2e_armed) { /* the host task's callout first (the phone reconnects at once after) */
        e2e_armed = false;
        e2e_extra += e2e_arm_ms;
        dsp_ble_tr_on_retry(&TR);
        return;
    }
    ble_wait(ctx, max_ms);
}
static void e2e_lock(void *ctx)
{
    (void)ctx;
}
static const dsp_ble_radio_t E2E_RADIO = {NULL,          e2e_init,      e2e_deinit, e2e_is_off,
                                          e2e_adv_start, e2e_adv_stop,  e2e_terminate,
                                          e2e_notify,    e2e_retry_arm, e2e_signal, e2e_wait,
                                          e2e_now,       e2e_lock,      e2e_lock};
/* the harness's own accounting around the transport's port */
static bool e2e_start(void *ctx)
{
    sc.starts++;
    bool ok = TRP.start(ctx);
    ble_on = ok;
    return ok;
}
static void e2e_suspend(void *ctx)
{
    sc.suspends++;
    e2e_quiet = true;
    TRP.suspend(ctx);
    e2e_quiet = false;
    ble_on = false;
}
static bool e2e_resume(void *ctx)
{
    sc.resumes++;
    bool ok = TRP.resume(ctx);
    ble_on = ok;
    return ok;
}
static void e2e_stop(void *ctx)
{
    e2e_quiet = true;
    TRP.stop(ctx);
    e2e_quiet = false;
    ble_on = false;
}

static void e2e_post(const uint8_t *f, size_t n)
{
    size_t cnt = dsp_ble_frag_count(n, e2e_mtu);
    if (!e2e_conn || cnt == 0 || cnt > 64 || n > DSP_BLE_FRAME_MAX) {
        return; /* nothing a phone could send */
    }
    uint8_t frag[DSP_BLE_TR_MTU_PREFERRED];
    for (size_t i = 0; i < cnt; i++) {
        size_t fl = dsp_ble_frag_get(f, n, e2e_mtu, i, frag, sizeof frag);
        e2e_writes++;
        (void)dsp_ble_tr_on_write(&TR, 1, frag, fl);
    }
}

static void e2e_setup(void)
{
    const char *m = getenv("DSP_E2E_MTU");
    e2e_mtu = (uint16_t)(m != NULL ? atoi(m) : 23);
    e2e_conn = false;
    e2e_armed = false;
    dsp_ble_tr_init(&TR, &E2E_RADIO, &app, &TRP);
    BLE2 = TRP;
    BLE2.start = e2e_start;
    BLE2.suspend = e2e_suspend;
    BLE2.resume = e2e_resume;
    BLE2.stop = e2e_stop;
}
#endif

/* ---- Phone ------------------------------------------------------------------------------------ */

typedef struct {
    uint8_t priv[32], pub[32];
    uint8_t dev_pub[32];
    uint8_t setup[16];
    uint8_t token[16];
    bool use_token;
    char name[32];
    /* per connection */
    uint32_t gen;
    NoiseHandshakeState *hs;
    NoiseCipherState *tx, *rx;
    uint8_t purpose;
    uint32_t session_id;
    uint8_t id_hash[8];
    uint8_t claim_state, hello_flags;
    /* test manipulations */
    int32_t prologue_sid_delta;
    int msg_index_override; /* -1 = none */
} phone_t;

static phone_t PA, PB, *P = &PA;

typedef struct {
    uint8_t type, flags;
    uint16_t txn;
    bool enc;
    bool decrypt_failed;
    bool decoded;
    size_t plen;
    uint8_t plain[DSP_BLE_FRAME_MAX];
    dsp_ble_body_t b;
} logent_t;

#define LOG_MAX 96
static logent_t g_log[LOG_MAX];
static int g_nlog;
static int g_disconnects_seen;

static void phone_free(phone_t *p)
{
    if (p->hs != NULL) {
        noise_handshakestate_free(p->hs);
        p->hs = NULL;
    }
    if (p->tx != NULL) {
        noise_cipherstate_free(p->tx);
        p->tx = NULL;
    }
    if (p->rx != NULL) {
        noise_cipherstate_free(p->rx);
        p->rx = NULL;
    }
}

/* Decrypt / decode every captured frame in order (nonces are sequential). */
static void drain(void)
{
    while (inbox.count > 0) {
        uint8_t *f = inbox.f[inbox.head];
        size_t n = inbox.n[inbox.head];
        inbox.head = (inbox.head + 1) % INBOX;
        inbox.count--;
        if (g_nlog >= LOG_MAX) {
            continue;
        }
        logent_t *e = &g_log[g_nlog++];
        memset(e, 0, sizeof *e);
        dsp_ble_header_t h;
        const uint8_t *body = NULL;
        if (dsp_ble_parse_header(f, n, &h, &body) != DSP_BLE_OK) {
            e->type = 0xFF;
            e->plen = n < sizeof e->plain ? n : sizeof e->plain; /* raw frame, for checks */
            memcpy(e->plain, f, e->plen);
            continue;
        }
        e->type = h.type;
        e->flags = h.flags;
        e->txn = h.txn;
        e->enc = (h.flags & DSP_BLE_F_ENCRYPTED) != 0;
        if (e->enc) {
            if (P->rx == NULL) {
                e->decrypt_failed = true;
                continue;
            }
            memcpy(e->plain, body, h.len);
            NoiseBuffer b;
            noise_buffer_set_input(b, e->plain, h.len);
            if (noise_cipherstate_decrypt_with_ad(P->rx, f, DSP_BLE_HEADER_LEN, &b) != NOISE_ERROR_NONE) {
                e->decrypt_failed = true;
                continue;
            }
            e->plen = b.size;
            dsp_ble_header_t ph = h;
            ph.flags = (uint8_t)(h.flags & ~DSP_BLE_F_ENCRYPTED);
            ph.len = (uint16_t)b.size;
            e->decoded = dsp_ble_decode_body(&ph, e->plain, b.size, DSP_BLE_MINOR, &e->b) == DSP_BLE_OK;
        } else {
            memcpy(e->plain, body, h.len);
            e->plen = h.len;
            e->decoded = dsp_ble_decode_body(&h, body, h.len, DSP_BLE_MINOR, &e->b) == DSP_BLE_OK;
        }
    }
}

static void phone_on_disconnect(void)
{
    drain();
    phone_free(P);
    g_disconnects_seen++;
}

/* the phone notices a new connection (resume after a pump run, reconnect) */
static void phone_sync_conn(void)
{
    drain();
    if (P->gen != sc.gen) {
        phone_free(P);
        P->gen = sc.gen;
    }
}

static const logent_t *last_of(uint8_t type)
{
    for (int i = g_nlog - 1; i >= 0; i--) {
        if (g_log[i].type == type) {
            return &g_log[i];
        }
    }
    return NULL;
}
static const logent_t *by_txn(uint16_t txn)
{
    for (int i = g_nlog - 1; i >= 0; i--) {
        if (g_log[i].txn == txn && (g_log[i].flags & DSP_BLE_F_RESPONSE)) {
            return &g_log[i];
        }
    }
    return NULL;
}
static int count_type(uint8_t type)
{
    int c = 0;
    for (int i = 0; i < g_nlog; i++) {
        c += g_log[i].type == type;
    }
    return c;
}

static void post(const uint8_t *f, size_t n)
{
#ifdef DSP_VIA_TRANSPORT
    e2e_post(f, n);
#else
    (void)dsp_app_post_frame(&app, sc.gen, f, n);
#endif
}

static void post_msg(dsp_ble_msg_t *m)
{
    uint8_t f[DSP_BLE_FRAME_MAX];
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode(m, f, sizeof f, &n));
    post(f, n);
}

static void post_enc(uint8_t type, uint16_t txn, const uint8_t *plain, size_t len)
{
    uint8_t f[DSP_BLE_FRAME_MAX];
    DSP_CHECK(P->tx != NULL);
    if (P->tx == NULL) {
        return;
    }
    f[0] = DSP_BLE_MAJOR;
    f[1] = DSP_BLE_MINOR;
    f[2] = type;
    f[3] = DSP_BLE_F_ENCRYPTED;
    f[4] = (uint8_t)txn;
    f[5] = (uint8_t)(txn >> 8);
    f[6] = (uint8_t)(len + 16);
    f[7] = (uint8_t)((len + 16) >> 8);
    memcpy(f + 8, plain, len);
    NoiseBuffer b;
    noise_buffer_set_inout(b, f + 8, len, sizeof f - 8);
    DSP_CHECK_EQ_INT(NOISE_ERROR_NONE, noise_cipherstate_encrypt_with_ad(P->tx, f, 8, &b));
    post(f, 8 + b.size);
}

/* encrypted request from a message (header type/txn, plain body) */
static void post_enc_msg(const dsp_ble_msg_t *m)
{
    uint8_t body[DSP_BLE_PLAIN_BODY_MAX];
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode_body(m, body, sizeof body, &n));
    post_enc(m->h.type, m->h.txn, body, n);
}

/* ---- Phone hooks (run inside the wake, application task) -------------------------------------- */

static uint16_t g_txn = 100;

static void h_hello(int i)
{
    (void)i;
    phone_sync_conn();
    const uint8_t hello[10] = {1, 0, 1, 0, (uint8_t)g_txn, (uint8_t)(g_txn >> 8), 2, 0, 1, 1};
    g_txn++;
    post(hello, sizeof hello);
}

static bool g_hs_built;
static void h_hs1(int step_i)
{
    const int purpose = sc.op[step_i].a;
    phone_sync_conn();
    g_hs_built = false;
    const logent_t *h = last_of(DSP_BLE_T_HELLO);
    if (h == NULL || !h->decoded || (h->flags & DSP_BLE_F_ERROR)) {
        return;
    }
    P->session_id = h->b.hello_rsp.session_id;
    memcpy(P->id_hash, h->b.hello_rsp.device_id_hash, 8);
    P->claim_state = h->b.hello_rsp.claim_state;
    P->hello_flags = h->b.hello_rsp.flags;
    P->purpose = (uint8_t)purpose;
    phone_free(P);
    const char *name = purpose == DSP_SEC_SESSION ? "Noise_IK_25519_ChaChaPoly_SHA256"
                                                  : "Noise_XXpsk3_25519_ChaChaPoly_SHA256";
    if (noise_handshakestate_new_by_name(&P->hs, name, NOISE_ROLE_INITIATOR) != NOISE_ERROR_NONE) {
        return;
    }
    uint8_t pro[DSP_PROLOGUE_LEN];
    dsp_prologue_build(1, 0, (uint8_t)purpose, P->id_hash,
                       (uint32_t)((int64_t)P->session_id + P->prologue_sid_delta), pro);
    noise_handshakestate_set_prologue(P->hs, pro, sizeof pro);
    noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(P->hs), P->priv, 32);
    if (purpose == DSP_SEC_SESSION) {
        noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(P->hs), P->dev_pub, 32);
    } else {
        uint8_t psk[32];
        DSP_CHECK(dsp_psk_derive((uint8_t)purpose, P->setup, P->use_token ? P->token : NULL, psk));
        noise_handshakestate_set_pre_shared_key(P->hs, psk, 32);
        sodium_memzero(psk, sizeof psk);
    }
    if (noise_handshakestate_start(P->hs) != NOISE_ERROR_NONE) {
        return;
    }
    dsp_ble_msg_t m;
    memset(&m, 0, sizeof m);
    m.h.major = 1;
    m.h.type = DSP_BLE_T_HS;
    m.h.txn = g_txn++;
    m.b.hs_req.purpose = (uint8_t)purpose;
    m.b.hs_req.msg_index = (uint8_t)(P->msg_index_override >= 0 ? P->msg_index_override : 0);
    NoiseBuffer ob;
    noise_buffer_set_output(ob, m.b.hs_req.noise.b, sizeof m.b.hs_req.noise.b);
    if (noise_handshakestate_write_message(P->hs, &ob, NULL) != NOISE_ERROR_NONE) {
        return;
    }
    m.b.hs_req.noise.len = (uint16_t)ob.size;
    post_msg(&m);
    g_hs_built = true;
}

static bool g_split;
static uint8_t g_msg3_index = 2;
static void h_hs3(int step_i)
{
    const int name_len_override = sc.op[step_i].a;
    phone_sync_conn();
    g_split = false;
    const logent_t *r = last_of(DSP_BLE_T_HS);
    if (r == NULL || !r->decoded || (r->flags & DSP_BLE_F_ERROR) || P->hs == NULL) {
        return;
    }
    uint8_t work[DSP_BLE_PLAIN_BODY_MAX], pay[DSP_BLE_PLAIN_BODY_MAX];
    memcpy(work, r->b.hs_rsp.noise.b, r->b.hs_rsp.noise.len);
    NoiseBuffer mb, pb;
    noise_buffer_set_input(mb, work, r->b.hs_rsp.noise.len);
    noise_buffer_set_output(pb, pay, sizeof pay);
    if (noise_handshakestate_read_message(P->hs, &mb, &pb) != NOISE_ERROR_NONE) {
        return;
    }
    if (P->purpose != DSP_SEC_SESSION) {
        noise_dhstate_get_public_key(noise_handshakestate_get_remote_public_key_dh(P->hs), P->dev_pub, 32);
        size_t nl = name_len_override > 0 ? (size_t)name_len_override : strlen(P->name);
        uint8_t payload[64];
        memset(payload, 'x', sizeof payload);
        payload[0] = (uint8_t)nl;
        memcpy(payload + 1, P->name, nl <= strlen(P->name) ? nl : strlen(P->name));
        NoiseBuffer in;
        noise_buffer_set_input(in, payload, 1 + nl);
        dsp_ble_msg_t m;
        memset(&m, 0, sizeof m);
        m.h.major = 1;
        m.h.type = DSP_BLE_T_HS;
        m.h.txn = g_txn++;
        m.b.hs_req.purpose = P->purpose;
        m.b.hs_req.msg_index = g_msg3_index;
        NoiseBuffer ob;
        noise_buffer_set_output(ob, m.b.hs_req.noise.b, sizeof m.b.hs_req.noise.b);
        if (noise_handshakestate_write_message(P->hs, &ob, &in) != NOISE_ERROR_NONE) {
            return;
        }
        m.b.hs_req.noise.len = (uint16_t)ob.size;
        post_msg(&m);
    }
    if (noise_handshakestate_get_action(P->hs) == NOISE_ACTION_SPLIT &&
        noise_handshakestate_split(P->hs, &P->tx, &P->rx) == NOISE_ERROR_NONE) {
        noise_handshakestate_free(P->hs);
        P->hs = NULL;
        g_split = true;
    }
}

/* scripted encrypted requests */
#define REQS 16
static dsp_ble_msg_t g_req[REQS];
static void h_req(int step_i)
{
    phone_sync_conn();
    post_enc_msg(&g_req[sc.op[step_i].a]);
}
static void h_drain(int i)
{
    (void)i;
    phone_sync_conn();
}
static void h_entropy(int step_i)
{
    g_entropy = sc.op[step_i].a != 0;
}
static void h_plain_end(int i)
{
    (void)i;
    phone_sync_conn();
    const uint8_t f[8] = {1, 0, DSP_BLE_T_END_SESSION, 0, 0x55, 0, 0, 0};
    post(f, sizeof f);
}
static uint8_t g_raw[DSP_BLE_FRAME_MAX];
static size_t g_raw_len;
static void h_raw(int i)
{
    (void)i;
    phone_sync_conn();
    post(g_raw, g_raw_len);
}
static dsp_app_op_result_t g_owner_end;
static void h_forged_end(int i)
{
    (void)i;
    const dsp_app_auth_t forged = {DSP_APP_ROLE_OWNER, 0, app.ss.session_id, 1};
    g_owner_end = dsp_app_end_service_session(&app, &forged);
}

/* ---- Setup ------------------------------------------------------------------------------------ */

static void req_init(int i, uint8_t type)
{
    memset(&g_req[i], 0, sizeof g_req[i]);
    g_req[i].h.major = 1;
    g_req[i].h.type = type;
    g_req[i].h.txn = (uint16_t)(1000 + i);
}

static void put_auth(const dsp_auth_t *a)
{
    uint8_t b[DSP_AUTH_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(a, b, sizeof b));
    fake_kv_put_raw(&kv, "auth", b, sizeof b);
}

static void phone_keys(phone_t *p, uint8_t seed)
{
    phone_free(p);
    memset(p, 0, sizeof *p);
    NoiseDHState *dh;
    noise_dhstate_new_by_name(&dh, "25519");
    noise_dhstate_generate_keypair(dh);
    noise_dhstate_get_keypair(dh, p->priv, 32, p->pub, 32);
    noise_dhstate_free(dh);
    memset(p->setup, seed, 16);
    snprintf(p->name, sizeof p->name, "Phone %c", 'A' + (seed & 7));
    p->msg_index_override = -1;
}

/* A provisioned (ready) device, unclaimed, with the security layer registered. */
static void fresh_unclaimed(void)
{
    provision(true);
    dsp_auth_t a;
    dsp_auth_provisioned(&a);
    put_auth(&a);
    memset(g_dev_setup, 0x5C, sizeof g_dev_setup);
    g_entropy = true;
    dsp_blesec_config_t c;
    memset(&c, 0, sizeof c);
    c.entropy_ready = cfg_entropy;
    c.random = test_rng_bytes;
    c.read_setup_code = cfg_setup;
    memcpy(c.device_id, DEV_ID, 6);
    dsp_blesec_init(&SECS, &c, &BSEC);
    BLE2 = BLE;
    BLE2.send_frame = cap_send;
    BLE2.disconnect = cap_disconnect;
#ifdef DSP_VIA_TRANSPORT
    e2e_setup();
#endif
    CFG.ble = &BLE2;
    CFG.security = &BSEC;
    phone_keys(&PA, 0x5C);
    phone_keys(&PB, 0x5C);
    P = &PA;
}

static void window_begin(void)
{
    clear_script();
    advance(g_true + MINUTE);
    inbox.head = inbox.count = 0;
    g_nlog = 0;
    g_disconnects_seen = 0;
    g_air_loss = false;
    PA.gen = PB.gen = 0;
    phone_free(&PA);
    phone_free(&PB);
}

/* CLAIM by phone A: HELLO, XX 1-2-3, ENROLL_RESULT; then steps continue at *t. */
static void script_handshake(dsp_utc_t *t, int purpose)
{
    step((*t)++, hk(h_hello, 0, 0));
    step((*t)++, hk(h_hs1, purpose, 0));
    step((*t)++, hk(h_hs3, 0, 0));
    if (purpose != DSP_SEC_SESSION) {
        step((*t)++, hk(h_drain, 0, 0));
    }
}

static bool all_zero(const void *p, size_t n)
{
    return sodium_is_zero(p, n) == 1;
}

static void check_zeroized(void);
/* T12/T14: called from inside the pump run. Whatever the suspension took (a timely GAP
 * disconnect or the bounded wait), the connection and its security session are gone. */
static int g_pump_probed;
static void probe_no_session_during_pump(void)
{
    g_pump_probed++;
    DSP_CHECK(!app.ss.conn_active);
    DSP_CHECK(!app.ble_active);
#ifdef DSP_VIA_TRANSPORT
    DSP_CHECK(!TR.conn);
    DSP_CHECK(!TRP.connected(TRP.ctx));
#endif
    check_zeroized();
}

static void check_zeroized(void)
{
    DSP_CHECK_EQ_INT(DSP_BLESEC_IDLE, SECS.state);
    DSP_CHECK(SECS.hs == NULL && SECS.tx_cs == NULL && SECS.rx_cs == NULL);
    DSP_CHECK(all_zero(SECS.new_priv, sizeof SECS.new_priv));
    DSP_CHECK(all_zero(SECS.phone_pub, sizeof SECS.phone_pub));
    DSP_CHECK(all_zero(SECS.name, sizeof SECS.name));
    DSP_CHECK(all_zero(SECS.transcript, sizeof SECS.transcript));
    DSP_CHECK(all_zero(SECS.hs_handle, sizeof SECS.hs_handle));
    DSP_CHECK(all_zero(&SECS.auth, sizeof SECS.auth));
    DSP_CHECK(all_zero(SECS.plain, sizeof SECS.plain));
    DSP_CHECK(all_zero(SECS.out, sizeof SECS.out));
    DSP_CHECK(all_zero(&SECS.cache, sizeof SECS.cache));
}

static dsp_auth_t durable_auth(void)
{
    dsp_auth_t a;
    memset(&a, 0, sizeof a);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "auth");
    DSP_CHECK(e != NULL);
    if (e != NULL) {
        DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_auth(e->value, e->len, &a));
    }
    return a;
}

/* CLAIM over XXpsk3 in one window, then end by timeout; returns with A enrolled in slot 0. */
static void do_claim(void)
{
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_CLAIM);
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
}

/* ==== Tests ================================================================================= */

static void test_claim_session_dispatch(void)
{
    fresh_unclaimed();
    uint64_t rng0 = test_rng_calls();
    do_claim();
    DSP_CHECK(test_rng_calls() > rng0); /* libsodium (noise-c) drew from the registered source */
    const logent_t *hello = last_of(DSP_BLE_T_HELLO);
    DSP_CHECK(hello != NULL && hello->decoded);
    DSP_CHECK_EQ_INT(0, PA.claim_state);
    uint8_t want_hash[8];
    dsp_id_hash(NULL, DEV_ID, want_hash);
    DSP_CHECK(memcmp(PA.id_hash, want_hash, 8) == 0); /* device form while unclaimed */
    const logent_t *er = last_of(DSP_BLE_T_ENROLL_RESULT);
    DSP_CHECK(er != NULL && er->enc && !er->decrypt_failed && er->decoded);
    DSP_CHECK(er != NULL && !(er->flags & DSP_BLE_F_ERROR));
    DSP_CHECK_EQ_INT(0, er != NULL ? er->b.slot.slot : 99);
    dsp_auth_t a = durable_auth();
    DSP_CHECK_EQ_INT(DSP_AUTH_CLAIMED, a.claim_state);
    DSP_CHECK(memcmp(a.id_pub, PA.dev_pub, 32) == 0);   /* the device static the phone saw */
    DSP_CHECK(memcmp(a.slot[0].pub, PA.pub, 32) == 0);  /* the phone's static key */
    DSP_CHECK(memcmp(a.slot[0].name, PA.name, strlen(PA.name)) == 0); /* the enrolled name */
    dsp_auth_wipe(&a);
    check_zeroized();

    /* SESSION (IK) in a new window: authorized operations through the dispatcher */
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_GET_STATUS);
    req_init(1, DSP_BLE_T_GET_CONFIG);
    req_init(2, DSP_BLE_T_PHONES_LIST);
    req_init(3, DSP_BLE_T_WIFI_GET);      /* recognized, not provided */
    req_init(4, DSP_BLE_T_INVITE_CREATE);
    req_init(5, DSP_BLE_T_LIST_SCHEDULES);
    for (int i = 0; i <= 5; i++) {
        step(t++, hk(h_req, i, 0));
    }
    step(t++, hk(h_drain, 0, 0));
    req_init(6, DSP_BLE_T_END_SESSION); /* encrypted, authorized: ends the window (C10) */
    step(t++, hk(h_req, 6, 0));
    step(t + 5, mk(OP_END_TIMEOUT));    /* not reached if the owner's END works */
    DSP_CHECK(session());
    drain();
    DSP_CHECK(g_split);
    const logent_t *st = by_txn(1000);
    DSP_CHECK(st != NULL && st->decoded && !(st->flags & DSP_BLE_F_ERROR));
    DSP_CHECK(st != NULL && st->b.status.status.readiness == DSP_READINESS_READY);
    const logent_t *cf = by_txn(1001);
    DSP_CHECK(cf != NULL && cf->decoded && cf->b.config.config.max_dose_ul > 0);
    const logent_t *pl = by_txn(1002);
    DSP_CHECK(pl != NULL && pl->decoded && pl->b.phones_list_rsp.items.n == 1 &&
              pl->b.phones_list_rsp.items.items[0].is_self);
    const logent_t *wf = by_txn(1003);
    DSP_CHECK(wf != NULL && (wf->flags & DSP_BLE_F_ERROR) && wf->b.error.code == DSP_BLE_E_UNAVAILABLE);
    const logent_t *iv = by_txn(1004);
    DSP_CHECK(iv != NULL && iv->decoded && !(iv->flags & DSP_BLE_F_ERROR) &&
              !all_zero(iv->b.invite_create_rsp.token, 16));
    const logent_t *ls = by_txn(1005);
    DSP_CHECK(ls != NULL && ls->decoded && !(ls->flags & DSP_BLE_F_ERROR));
    const logent_t *en = by_txn(1006);
    DSP_CHECK(en != NULL && en->enc && !(en->flags & DSP_BLE_F_ERROR));
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end); /* the owner ended the window */
    for (int i = 0; i < g_nlog; i++) {
        if (g_log[i].enc) {
            DSP_CHECK(!g_log[i].decrypt_failed);
        }
    }
    check_zeroized();
    invariants();
}

/* in-window helpers for the ADD flow */
static void h_use_b(int step_i)
{
    (void)step_i;
    phone_sync_conn();
    const logent_t *iv = by_txn(1000);
    if (iv != NULL && iv->decoded && !(iv->flags & DSP_BLE_F_ERROR)) {
        memcpy(PB.token, iv->b.invite_create_rsp.token, 16);
        PB.use_token = true;
    }
    memcpy(PB.dev_pub, PA.dev_pub, 32);
    P = &PB;
    P->gen = sc.gen;
}
static void h_use_a(int step_i)
{
    (void)step_i;
    drain();
    P = &PA;
    P->gen = sc.gen;
}

static void test_add_second_phone(void)
{
    fresh_unclaimed();
    do_claim();
    /* One window, one connection at a time: A (IK) creates an invite and leaves with a PLAIN
     * END_SESSION, which closes only A's connection (C10); B then joins in the same window with
     * XXpsk3(setup code, invite token) and ends the window with its authorized encrypted END. */
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_INVITE_CREATE);
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_plain_end, 0, 0));
    step(t++, hk(h_use_b, 0, 0));
    script_handshake(&t, DSP_SEC_ADD);
    req_init(1, DSP_BLE_T_PHONES_LIST);
    step(t++, hk(h_req, 1, 0));
    step(t++, hk(h_drain, 0, 0));
    req_init(2, DSP_BLE_T_END_SESSION);
    step(t++, hk(h_req, 2, 0));
    step(t++, hk(h_use_a, 0, 0));
    step(t + 5, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *pe = last_of(DSP_BLE_T_END_SESSION);
    DSP_CHECK(pe != NULL);
    const logent_t *er = last_of(DSP_BLE_T_ENROLL_RESULT);
    if (er == NULL) {
        fprintf(stderr, "ADD failed: blesec fail_line %u, invite valid %u\n", SECS.fail_line,
                app.retained.invite.valid);
    }
    DSP_CHECK(er != NULL && er->decoded && !(er->flags & DSP_BLE_F_ERROR) && er->b.slot.slot == 1);
    const logent_t *pl = by_txn(1001);
    DSP_CHECK(pl != NULL && pl->decoded && pl->b.phones_list_rsp.items.n == 2);
    DSP_CHECK(g_disconnects_seen >= 1);                  /* A's plain END closed A only */
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);  /* B (authorized) ended the window */
    dsp_auth_t a = durable_auth();
    DSP_CHECK(memcmp(a.slot[1].pub, PB.pub, 32) == 0);
    DSP_CHECK(memcmp(a.slot[0].pub, PA.pub, 32) == 0);
    dsp_auth_wipe(&a);
    /* the invite was single use: a third phone with the same token fails */
    uint8_t used[16];
    memcpy(used, PB.token, 16);
    phone_keys(&PB, 0x5C);
    memcpy(PB.token, used, 16);
    PB.use_token = true;
    memcpy(PB.dev_pub, PA.dev_pub, 32);
    P = &PB;
    window_begin();
    t = g_true;
    script_handshake(&t, DSP_SEC_ADD);
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *hs = last_of(DSP_BLE_T_HS);
    DSP_CHECK(hs != NULL && (hs->flags & DSP_BLE_F_ERROR) && hs->b.error.code == DSP_BLE_E_HANDSHAKE_FAILED);
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) == NULL);
    P = &PA;
    check_zeroized();
    invariants();
}

/* each handshake negative: HANDSHAKE_FAILED (plain), no ENROLL_RESULT, no ownership change
 * (except a persisted KEY_FAULT, which raises the epoch by design: g_epoch_may_change) */
static bool g_epoch_may_change;
static void expect_hs_failure(const char *what, int purpose)
{
    uint8_t before[DSP_AUTH_RECORD_BYTES];
    const fake_kv_entry_t *e0 = fake_kv_find(&kv, "auth");
    size_t l0 = e0 != NULL ? e0->len : 0;
    if (e0 != NULL) {
        memcpy(before, e0->value, l0);
    }
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, purpose);
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *hs = last_of(DSP_BLE_T_HS);
    bool failed = hs != NULL && (hs->flags & DSP_BLE_F_ERROR) && hs->b.error.code == DSP_BLE_E_HANDSHAKE_FAILED;
    if (!failed) {
        fprintf(stderr, "negative '%s' did not fail the handshake\n", what);
    }
    DSP_CHECK(failed);
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) == NULL);
    const fake_kv_entry_t *e1 = fake_kv_find(&kv, "auth");
    DSP_CHECK(e1 != NULL && e1->len == l0);
    /* the record may differ only in the failed_sessions diagnostic counter */
    dsp_auth_t a0, a1;
    if (e1 != NULL && l0 == e1->len && dsp_codec_decode_auth(before, l0, &a0) == DSP_CODEC_OK &&
        dsp_codec_decode_auth(e1->value, e1->len, &a1) == DSP_CODEC_OK) {
        DSP_CHECK_EQ_INT(a0.claim_state, a1.claim_state);
        DSP_CHECK(memcmp(a0.slot, a1.slot, sizeof a0.slot) == 0);
        DSP_CHECK(a1.owner_epoch == a0.owner_epoch || (g_epoch_may_change && a1.owner_epoch == a0.owner_epoch + 1));
        dsp_auth_wipe(&a0);
        dsp_auth_wipe(&a1);
    }
    check_zeroized();
}

static void test_handshake_negatives(void)
{
    fresh_unclaimed();
    /* wrong setup code (CLAIM) */
    memset(PA.setup, 0x11, 16);
    expect_hs_failure("wrong setup code", DSP_SEC_CLAIM);
    memset(PA.setup, 0x5C, 16);
    /* prologue mismatch: a session ID from another connection */
    PA.prologue_sid_delta = 1;
    expect_hs_failure("prologue session id", DSP_SEC_CLAIM);
    PA.prologue_sid_delta = 0;
    /* message index out of order */
    PA.msg_index_override = 2;
    expect_hs_failure("msg_index", DSP_SEC_CLAIM);
    PA.msg_index_override = -1;
    /* no entropy source: no handshake starts (C2) */
    window_begin();
    dsp_utc_t t = g_true;
    g_entropy = false; /* no hardware entropy when the connection opens */
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_hs1, DSP_SEC_CLAIM, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    g_entropy = true;
    drain();
    const logent_t *hl = last_of(DSP_BLE_T_HELLO);
    DSP_CHECK(hl != NULL && hl->decoded && hl->b.hello_rsp.session_id == 0); /* no session ID */
    const logent_t *hs = last_of(DSP_BLE_T_HS);
    DSP_CHECK(hs != NULL && (hs->flags & DSP_BLE_F_ERROR));
    /* now claim for real; then SESSION negatives */
    do_claim();
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) != NULL);
    /* unknown phone key (IK) */
    phone_free(&PA); /* no live session objects in the copy */
    phone_t saved = PA;
    memset(&PA, 0, sizeof PA);
    phone_keys(&PA, 0x5C);
    memcpy(PA.dev_pub, saved.dev_pub, 32);
    expect_hs_failure("unknown phone key", DSP_SEC_SESSION);
    phone_free(&PA);
    PA = saved;
    /* wrong device identity expected by the phone (IK to another key) */
    uint8_t keep[32];
    memcpy(keep, PA.dev_pub, 32);
    PA.dev_pub[0] ^= 1;
    expect_hs_failure("wrong device identity", DSP_SEC_SESSION);
    memcpy(PA.dev_pub, keep, 32);
    /* CLAIM on a claimed device: admission refused */
    expect_hs_failure("claim when claimed", DSP_SEC_CLAIM);
    /* name payload too long (25) in an ADD-less XX: use RECOVERY? not armed -> admission refuses */
    expect_hs_failure("recovery not armed", DSP_SEC_RECOVERY);
    /* handshake step timeout: message 3 never arrives */
    window_begin();
    t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_hs1, DSP_SEC_ADD, 0)); /* no invite: refused at admission anyway */
    step(t + 15, hk(h_drain, 0, 0));
    step(t + 16, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    check_zeroized();
    invariants();
}

/* Manipulated frames, built inside the window from the phone's live cipher state. */
static int g_mode;
static void h_manip(int i)
{
    (void)i;
    phone_sync_conn();
    if (P->tx == NULL) {
        return;
    }
    uint8_t body[DSP_BLE_PLAIN_BODY_MAX];
    size_t n = 0;
    dsp_ble_msg_t m;
    memset(&m, 0, sizeof m);
    m.h.major = 1;
    m.h.type = g_mode == 7 ? DSP_BLE_T_HELLO : DSP_BLE_T_GET_STATUS;
    m.h.txn = 2000;
    if (g_mode == 7) {
        m.b.hello_req.proto_min = 1;
        m.b.hello_req.proto_max = 1;
    }
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode_body(&m, body, sizeof body, &n));
    uint8_t f[DSP_BLE_FRAME_MAX];
    f[0] = 1;
    f[1] = 0;
    f[2] = m.h.type;
    f[3] = DSP_BLE_F_ENCRYPTED;
    f[4] = (uint8_t)m.h.txn;
    f[5] = (uint8_t)(m.h.txn >> 8);
    f[6] = (uint8_t)(n + 16);
    f[7] = 0;
    memcpy(f + 8, body, n);
    NoiseBuffer b;
    noise_buffer_set_inout(b, f + 8, n, sizeof f - 8);
    noise_cipherstate_encrypt_with_ad(P->tx, f, 8, &b);
    size_t len = 8 + b.size;
    switch (g_mode) {
    case 1:
        f[8] ^= 0x01;
        break;
    case 2:
        f[len - 1] ^= 0x80;
        break;
    case 3:
        f[4] ^= 0x01; /* txn is part of the AD */
        break;
    case 4:
        len -= 1;
        f[6] = (uint8_t)(f[6] - 1);
        break;
    case 5:
        post(f, len); /* valid once ... */
        break;      /* ... then the identical frame again (replay) */
    case 6: {
        const uint8_t plain[8] = {1, 0, DSP_BLE_T_GET_STATUS, 0, 0x34, 0x12, 0, 0};
        post(plain, sizeof plain);
        return;
    }
    default:
        break;
    }
    post(f, len);
}

static void test_transport_manipulations(void)
{
    fresh_unclaimed();
    do_claim();
    for (g_mode = 1; g_mode <= 7; g_mode++) {
        window_begin();
        dsp_utc_t t = g_true;
        script_handshake(&t, DSP_SEC_SESSION);
        step(t++, hk(h_manip, 0, 0));
        step(t++, hk(h_drain, 0, 0));
        step(t + 2, mk(OP_END_TIMEOUT));
        DSP_CHECK(session());
        drain();
        DSP_CHECK(g_split);
        int dis = g_disconnects_seen;
        if (g_mode == 5) {
            /* the first copy is answered, the replay closes the connection */
            const logent_t *r = by_txn(2000);
            DSP_CHECK(r != NULL && r->enc && !(r->flags & DSP_BLE_F_ERROR));
        } else if (g_mode == 7) {
            /* decrypts, but HELLO is not an encrypted type: encrypted MALFORMED, then close */
            const logent_t *r = by_txn(2000);
            DSP_CHECK(r != NULL && r->enc && (r->flags & DSP_BLE_F_ERROR) && r->b.error.code == DSP_BLE_E_MALFORMED);
        } else {
            DSP_CHECK(by_txn(2000) == NULL && by_txn(0x1234) == NULL); /* no answer */
        }
        if (dis < 1) {
            fprintf(stderr, "manipulation mode %d did not close the connection\n", g_mode);
        }
        DSP_CHECK(dis >= 1); /* every security error closes the connection */
        DSP_CHECK(rep.ble_end != DSP_BLE_END_COMMAND); /* never the whole window */
        check_zeroized();
    }
    invariants();
}

static uint8_t g_active_invite[16];
static uint8_t g_active_valid;
static void h_snap_invite(int step_i)
{
    (void)step_i;
    g_active_valid = app.retained.invite.valid;
    memcpy(g_active_invite, app.retained.invite.token, 16);
}

/* C4: identical duplicate -> original response, no second execution; different content ->
 * MALFORMED, not executed; a new connection has an empty cache. */
static void test_response_cache(void)
{
    fresh_unclaimed();
    do_claim();
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_INVITE_CREATE); /* not idempotent: the cache must hold it */
    g_req[1] = g_req[0];                  /* identical duplicate (same txn, same body) */
    req_init(2, DSP_BLE_T_SET_CONFIG);
    g_req[2].h.txn = 1000;                /* same txn as the invite, different content */
    req_init(3, DSP_BLE_T_GET_STATUS);
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_req, 1, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_req, 2, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_req, 3, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_snap_invite, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *rs[3] = {NULL, NULL, NULL};
    int k = 0;
    for (int i = 0; i < g_nlog && k < 3; i++) {
        if (g_log[i].txn == 1000 && (g_log[i].flags & DSP_BLE_F_RESPONSE)) {
            rs[k++] = &g_log[i];
        }
    }
    DSP_CHECK_EQ_INT(3, k);
    if (k == 3) {
        DSP_CHECK(!(rs[0]->flags & DSP_BLE_F_ERROR));
        DSP_CHECK(rs[0]->plen == rs[1]->plen && memcmp(rs[0]->plain, rs[1]->plain, rs[0]->plen) == 0);
        DSP_CHECK((rs[2]->flags & DSP_BLE_F_ERROR) && rs[2]->b.error.code == DSP_BLE_E_MALFORMED);
        /* the active invite is the token delivered (the duplicate did not create a second one) */
        DSP_CHECK_EQ_INT(1, g_active_valid);
        DSP_CHECK(memcmp(g_active_invite, rs[0]->b.invite_create_rsp.token, 16) == 0);
    }
    const logent_t *st = by_txn(1003);
    DSP_CHECK(st != NULL && !(st->flags & DSP_BLE_F_ERROR)); /* the connection stayed open */
    invariants();
}

/* C10 with a security layer: plain END closes only the connection; a forged authorized end is
 * refused; failed handshakes never end the window. */
static void test_c10_with_security(void)
{
    fresh_unclaimed();
    do_claim();
    window_begin();
    dsp_utc_t t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_plain_end, 0, 0));
    step(t++, hk(h_forged_end, 0, 0));
    for (int k = 0; k < 4; k++) { /* failed handshakes */
        step(t++, hk(h_hello, 0, 0));
        step(t++, hk(h_hs1, DSP_SEC_CLAIM, 0));
    }
    t += 2;
    script_handshake(&t, DSP_SEC_SESSION); /* the owner still gets in */
    req_init(0, DSP_BLE_T_GET_STATUS);
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    req_init(1, DSP_BLE_T_END_SESSION);
    step(t++, hk(h_req, 1, 0));
    step(t + 5, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, g_owner_end.status);
    const logent_t *st = by_txn(1000);
    DSP_CHECK(st != NULL && !(st->flags & DSP_BLE_F_ERROR));
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end); /* ended only by the owner's encrypted END */
    invariants();
}

/* Lifecycle: EV_SESSION_CLOSING near the window maximum; manual dose suspends BLE (event, then the
 * session is gone; the reconnect needs a new handshake); reboot leaves nothing. */
static void test_lifecycle_events(void)
{
    fresh_unclaimed();
    do_claim();
    CFG.service_max_s = 120;
    CFG.service_idle_s = 60; /* (the maximum is never below the idle timeout) */
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    step(t + 100, hk(h_drain, 0, 0));
    step(t + 130, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *ev = last_of(DSP_BLE_T_EV_SESSION_CLOSING);
    if (ev == NULL) {
        fprintf(stderr, "no EV_SESSION_CLOSING: log %d entries, ble_end %d, state %u\n", g_nlog,
                rep.ble_end, SECS.state);
        for (int i = 0; i < g_nlog; i++) {
            fprintf(stderr, "  log %d type 0x%02x flags 0x%02x txn %u dec_fail %d\n", i, g_log[i].type,
                    g_log[i].flags, g_log[i].txn, g_log[i].decrypt_failed);
        }
    }
    DSP_CHECK(ev != NULL && ev->enc && ev->decoded && ev->b.ev_session_closing.seconds_left <= 30);
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    CFG.service_max_s = 0;
    CFG.service_idle_s = 0;
    check_zeroized();
    /* manual dose */
    window_begin();
    t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_MANUAL_DOSE);
    g_req[0].b.manual_dose_req.seq = 1;
    g_req[0].b.manual_dose_req.volume_ul = 1000;
    step(t++, hk(h_req, 0, 0));
    step(t + 3, hk(h_drain, 0, 0));
    step(t + 4, mk(OP_END_TIMEOUT));
    int pumps_before = pump.calls;
    int violations_before = pump.violations;
    g_pump_probed = 0;
    pump_hook = probe_no_session_during_pump;
    manual_via_ble = true; /* only "no scripted OP_MANUAL step"; the durable-decision checks stay */
    DSP_CHECK(session());
    manual_via_ble = false;
    DSP_CHECK(pump.calls > pumps_before);                    /* the oracle was exercised */
    DSP_CHECK_EQ_INT(violations_before, pump.violations);    /* durable decision before the pump */
    DSP_CHECK_EQ_INT(1, g_pump_probed); /* T12/T14: no security session while the pump ran */
    drain();
    DSP_CHECK(last_of(DSP_BLE_T_EV_SUSPENDING_FOR_DOSE) != NULL); /* best effort, sent */
    DSP_CHECK(by_txn(1000) == NULL);                               /* no response (suspended) */
    DSP_CHECK(sc.suspends >= 1);
    check_zeroized();
    invariants();
}

/* The stored identity key pair is inconsistent (public key no longer matches the private key):
 * the first handshake that uses it fails and KEY_FAULT inhibits dosing at once (M1.10b.3). */
static void test_identity_key_fault(void)
{
    fresh_unclaimed();
    do_claim();
    dsp_auth_t a = durable_auth();
    a.id_pub[0] ^= 0x01; /* still a valid record encoding, but the pair is inconsistent */
    put_auth(&a);
    dsp_auth_wipe(&a);
    memcpy(PA.dev_pub, durable_auth().id_pub, 32); /* the phone even expects the stored key */
    g_epoch_may_change = true;
    expect_hs_failure("inconsistent identity key", DSP_SEC_SESSION);
    g_epoch_may_change = false;
    dsp_auth_t b = durable_auth();
    DSP_CHECK(b.flags & DSP_AUTH_F_KEY_FAULT); /* persisted for later wakes */
    dsp_auth_wipe(&b);
    invariants();
}

/* An enrollment name longer than 24 bytes in XX message 3 fails the handshake. */
static void test_enroll_name_too_long(void)
{
    fresh_unclaimed();
    snprintf(PA.name, sizeof PA.name, "%s", "abcdefghijklmnopqrstuvwxyz"); /* 26 bytes */
    window_begin();
    dsp_utc_t t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_hs1, DSP_SEC_CLAIM, 0));
    step(t++, hk(h_hs3, 25, 0)); /* name length byte 25 */
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *hs = last_of(DSP_BLE_T_HS);
    DSP_CHECK(hs != NULL && (hs->flags & DSP_BLE_F_ERROR) && hs->b.error.code == DSP_BLE_E_HANDSHAKE_FAILED);
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) == NULL);
    dsp_auth_t a2 = durable_auth();
    DSP_CHECK_EQ_INT(DSP_AUTH_UNCLAIMED, a2.claim_state);
    dsp_auth_wipe(&a2);
    check_zeroized();
    invariants();
}

/* XX message 3 with a wrong msg_index fails the handshake (no enrollment). */
static void test_hs_msg3_wrong_index(void)
{
    fresh_unclaimed();
    for (uint8_t idx = 0; idx <= 1; idx++) {
        g_msg3_index = idx;
        expect_hs_failure("msg3 index", DSP_SEC_CLAIM);
    }
    g_msg3_index = 2;
    dsp_auth_t a = durable_auth();
    DSP_CHECK_EQ_INT(DSP_AUTH_UNCLAIMED, a.claim_state);
    dsp_auth_wipe(&a);
    invariants();
}

/* C4 details: the same txn and type with a different body is rejected (digest), and an ownership
 * change (epoch) purges older entries: the same request after PHONE_REMOVE executes again. */
static void test_cache_digest_and_epoch(void)
{
    fresh_unclaimed();
    do_claim();
    /* enroll B so that A can remove a phone (epoch + 1) */
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_INVITE_CREATE);
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_plain_end, 0, 0));
    step(t++, hk(h_use_b, 0, 0));
    script_handshake(&t, DSP_SEC_ADD);
    req_init(1, DSP_BLE_T_END_SESSION);
    step(t++, hk(h_req, 1, 0));
    step(t++, hk(h_use_a, 0, 0));
    step(t + 5, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) != NULL);
    /* A: GET_RESULT txn 1000 (seq 1), then the same txn with seq 2 -> MALFORMED (not executed);
     * INVITE_CREATE txn 1002, PHONE_REMOVE slot 1 (epoch + 1), INVITE_CREATE txn 1002 again
     * -> executed anew (a different token) */
    P = &PA;
    window_begin();
    t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_GET_RESULT);
    g_req[0].b.seq.seq = 1;
    g_req[1] = g_req[0];
    g_req[1].b.seq.seq = 2;
    req_init(2, DSP_BLE_T_INVITE_CREATE);
    req_init(3, DSP_BLE_T_PHONE_REMOVE);
    g_req[3].b.slot.slot = 1;
    g_req[4] = g_req[2];
    for (int i = 0; i <= 4; i++) {
        step(t++, hk(h_req, i, 0));
        step(t++, hk(h_drain, 0, 0));
    }
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    const logent_t *r1[2] = {NULL, NULL}, *iv[2] = {NULL, NULL};
    int n1 = 0, n2 = 0;
    for (int i = 0; i < g_nlog; i++) {
        if (!(g_log[i].flags & DSP_BLE_F_RESPONSE)) {
            continue;
        }
        if (g_log[i].txn == 1000 && n1 < 2) {
            r1[n1++] = &g_log[i];
        }
        if (g_log[i].txn == 1002 && n2 < 2) {
            iv[n2++] = &g_log[i];
        }
    }
    DSP_CHECK(n1 == 2 && n2 == 2);
    if (n1 == 2 && n2 == 2) {
        DSP_CHECK(!(r1[0]->flags & DSP_BLE_F_ERROR));
        DSP_CHECK((r1[1]->flags & DSP_BLE_F_ERROR) && r1[1]->b.error.code == DSP_BLE_E_MALFORMED);
        DSP_CHECK(!(iv[0]->flags & DSP_BLE_F_ERROR) && !(iv[1]->flags & DSP_BLE_F_ERROR));
        DSP_CHECK(memcmp(iv[0]->b.invite_create_rsp.token, iv[1]->b.invite_create_rsp.token, 16) != 0);
    }
    const logent_t *rm = by_txn(1003);
    DSP_CHECK(rm != NULL && !(rm->flags & DSP_BLE_F_ERROR));
    invariants();
}

static uint8_t g_arm_aborts;
static void h_snap_arm(int step_i)
{
    (void)step_i;
    g_arm_aborts = app.retained.arm.aborts;
}

/* C10 in CONFIRMING: the recovering phone's END_SESSION (encrypted) closes only its connection and
 * aborts the confirmation; the window continues. */
static void test_end_during_confirmation(void)
{
    fresh_unclaimed();
    do_claim();
    phone_keys(&PB, 0x5C);
    P = &PB;
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_RECOVERY);
    req_init(0, DSP_BLE_T_END_SESSION);
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_snap_arm, 0, 0));
    step(t + 3, mk(OP_END_TIMEOUT));
    g_arm_aborts = 0;
    DSP_CHECK(session());
    CFG.recovery_hold_ms = 0;
    drain();
    DSP_CHECK(last_of(DSP_BLE_T_EV_CONFIRM_ON_DEVICE) != NULL);
    const logent_t *en = by_txn(1000);
    DSP_CHECK(en != NULL && en->enc && !(en->flags & DSP_BLE_F_ERROR));
    DSP_CHECK(g_disconnects_seen >= 1);
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end); /* not ended by the unauthorized END */
    DSP_CHECK(last_of(DSP_BLE_T_ENROLL_RESULT) == NULL);
    DSP_CHECK(g_arm_aborts >= 1);                   /* the confirmation counted as failed */
    dsp_auth_t a = durable_auth();
    DSP_CHECK(memcmp(a.slot[0].pub, PA.pub, 32) == 0); /* ownership unchanged */
    dsp_auth_wipe(&a);
    P = &PA;
    check_zeroized();
    invariants();
}

static void h_press3(int step_i)
{
    (void)step_i;
    phone_sync_conn();
    uint64_t base = mono_ms + 400;
    for (int k = 0; k < 3; k++) {
        dsp_button_event_t e = {DSP_BUTTON_SHORT, base + (uint64_t)k * 400, base + (uint64_t)k * 400 + 100};
        (void)dsp_app_post_button(&app, &e);
    }
}

/* RECOVERY over BLE: an armed device (10 s hold at the button boot), XXpsk3 with the setup code,
 * CONFIRMING (encrypted BUSY 2 for anything but END_SESSION), three presses, ENROLL_RESULT{0};
 * the new identity and the new phone replace the old ownership, readiness SERVICE_REQUIRED. */
static void test_recovery_flow(void)
{
    fresh_unclaimed();
    do_claim();
    uint8_t old_dev[32];
    memcpy(old_dev, PA.dev_pub, 32);
    phone_keys(&PB, 0x5C); /* the replacement phone */
    P = &PB;
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_RECOVERY);
    req_init(0, DSP_BLE_T_GET_STATUS); /* while confirming: BUSY detail 2 */
    step(t++, hk(h_req, 0, 0));
    step(t++, hk(h_press3, 0, 0));
    step(t + 3, hk(h_drain, 0, 0));
    req_init(1, DSP_BLE_T_GET_STATUS);
    step(t + 4, hk(h_req, 1, 0));
    step(t + 5, hk(h_drain, 0, 0));
    step(t + 7, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    CFG.recovery_hold_ms = 0;
    drain();
    const logent_t *hl = last_of(DSP_BLE_T_HELLO);
    DSP_CHECK(hl != NULL && hl->decoded && (hl->b.hello_rsp.flags & DSP_BLE_HELLO_F_RECOVERY_ARMED));
    const logent_t *ev = last_of(DSP_BLE_T_EV_CONFIRM_ON_DEVICE);
    DSP_CHECK(ev != NULL && ev->enc && ev->decoded && ev->b.ev_confirm.action == 1);
    const logent_t *busy = by_txn(1000);
    DSP_CHECK(busy != NULL && (busy->flags & DSP_BLE_F_ERROR) && busy->b.error.code == DSP_BLE_E_BUSY &&
              busy->b.error.detail == DSP_OP_B_CONFIRMING);
    const logent_t *er = last_of(DSP_BLE_T_ENROLL_RESULT);
    if (er == NULL) {
        fprintf(stderr, "recovery: no ENROLL_RESULT (blesec state %u, fail_line %u)\n", SECS.state,
                SECS.fail_line);
    }
    DSP_CHECK(er != NULL && er->decoded && !(er->flags & DSP_BLE_F_ERROR) && er->b.slot.slot == 0);
    const logent_t *st = by_txn(1001);
    DSP_CHECK(st != NULL && st->decoded && !(st->flags & DSP_BLE_F_ERROR) &&
              st->b.status.status.readiness == DSP_READINESS_SERVICE_REQUIRED);
    dsp_auth_t a = durable_auth();
    DSP_CHECK(memcmp(a.id_pub, PB.dev_pub, 32) == 0);   /* the new identity */
    DSP_CHECK(memcmp(a.id_pub, old_dev, 32) != 0);
    DSP_CHECK(memcmp(a.slot[0].pub, PB.pub, 32) == 0);  /* the recovering phone */
    DSP_CHECK(!a.slot[1].used && !a.slot[2].used);      /* everyone else removed */
    dsp_auth_wipe(&a);
    P = &PA;
    check_zeroized();
    invariants();
}

/* Random frames in every pre-authentication state: no crash, no operation, no write. */
/* M1.10d (decision T3): a fragment error reported by the transport. Before the handshake: a
 * plain MALFORMED (type 0, txn 0), the connection stays. In an XX handshake or after
 * authentication: the connection is closed with no reply at all (never a plaintext error). */
static void h_rx_fault(int i)
{
    (void)i;
    phone_sync_conn();
#ifdef DSP_VIA_TRANSPORT
    /* a real fragment error: a continuation fragment with no partial frame, through the
     * transport's reassembly (its own counter and the application's are both exercised) */
    const uint8_t bad[2] = {0x01, 0xEE};
    DSP_CHECK(e2e_conn);
    e2e_writes++;
    DSP_CHECK_EQ_INT(DSP_BLE_WRITE_OK, dsp_ble_tr_on_write(&TR, 1, bad, sizeof bad));
#else
    DSP_CHECK(dsp_app_post_rx_fault(&app, sc.gen));
#endif
}
static void h_air_loss(int step_i)
{
    g_air_loss = sc.op[step_i].a != 0;
}
/* T14: the application's limit is the effective one; the transport's backstop never fired. */
static void check_no_transport_fault_close(void)
{
#ifdef DSP_VIA_TRANSPORT
    DSP_CHECK_EQ_INT(0, (int)TR.st.rx_fault_closes);
#endif
}
static int plain_fault_errors(void)
{
    int n = 0;
    for (int k = 0; k < g_nlog; k++) {
        /* type 0 (no complete frame header was received, as for an unparsable header in the
         * M1.10c.1 gate): the codec rejects the type, so the raw frame is checked */
        const uint8_t *f = g_log[k].plain;
        n += g_log[k].type == 0xFF && g_log[k].plen == 17 && f[2] == 0 &&
             (f[3] & DSP_BLE_F_ERROR) && !(f[3] & DSP_BLE_F_ENCRYPTED) && f[4] == 0 && f[5] == 0 &&
             f[8] == DSP_BLE_E_MALFORMED;
    }
    return n;
}
static void test_rx_fault_policy(void)
{
    /* pre-authentication (after HELLO) */
    fresh_unclaimed();
    window_begin();
    dsp_utc_t t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK_EQ_INT(1, plain_fault_errors());
    DSP_CHECK_EQ_INT(0, g_disconnects_seen);
    DSP_CHECK_EQ_INT(1, (int)rep.rx_faults);
    check_no_transport_fault_close();
    check_zeroized();
    /* pre-authentication, three errors: three plain MALFORMED, the third closes the connection
     * (DSP_LINK_ERRORS_MAX, application side), long before the transport's backstop */
    fresh_unclaimed();
    window_begin();
    t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK_EQ_INT(3, plain_fault_errors());
    DSP_CHECK(g_disconnects_seen >= 1);
    DSP_CHECK_EQ_INT(3, (int)rep.rx_faults);
    check_no_transport_fault_close();
    check_zeroized();
    /* XX handshake in progress (CLAIM message 2 answered, message 3 pending) */
    fresh_unclaimed();
    window_begin();
    t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_hs1, DSP_SEC_CLAIM, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK(last_of(DSP_BLE_T_HS) != NULL); /* message 2 was sent */
    DSP_CHECK_EQ_INT(0, plain_fault_errors());
    DSP_CHECK(count_type(DSP_BLE_T_ENROLL_RESULT) == 0);
    DSP_CHECK(g_disconnects_seen >= 1);
    DSP_CHECK_EQ_INT(DSP_AUTH_UNCLAIMED, durable_auth().claim_state);
    check_no_transport_fault_close();
    check_zeroized();
    /* authenticated (IK SESSION): closed without any reply, plain or encrypted */
    fresh_unclaimed();
    do_claim();
    window_begin();
    t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK(g_split);
    int errors = 0;
    for (int k = 0; k < g_nlog; k++) {
        errors += (g_log[k].flags & DSP_BLE_F_ERROR) != 0;
    }
    DSP_CHECK_EQ_INT(0, errors);
    DSP_CHECK(g_disconnects_seen >= 1);
    DSP_CHECK(rep.ble_end != DSP_BLE_END_COMMAND); /* the window continues (C10) */
    check_no_transport_fault_close();
    check_zeroized();
    invariants();
}

#ifdef DSP_VIA_TRANSPORT
/* the controller drops the link later, while the pump runs */
static void probe_pump_after_forced_close(void)
{
    probe_no_session_during_pump();
    e2e_term_ignored = false;
    e2e_conn = false;
}

/* T14: suspension for a manual dose while the controller never reports the GAP disconnect. The
 * transport waits DSP_BLE_TR_CLOSE_WAIT_MS at most, synthesizes the disconnect, and the pump runs
 * with no connection and no security session; the session's deadlines still apply afterwards. */
static void test_suspend_without_gap_disconnect(void)
{
    fresh_unclaimed();
    do_claim();
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    req_init(0, DSP_BLE_T_MANUAL_DOSE);
    g_req[0].b.manual_dose_req.seq = 1;
    g_req[0].b.manual_dose_req.volume_ul = 1000;
    step(t++, hk(h_req, 0, 0));
    step(t + 3, hk(h_drain, 0, 0));
    step(t + 4, mk(OP_END_TIMEOUT));
    int pumps_before = pump.calls;
    int violations_before = pump.violations;
    g_pump_probed = 0;
    e2e_terms_ignored = 0;
    e2e_term_ignored = true;
    pump_hook = probe_pump_after_forced_close;
    manual_via_ble = true;
    DSP_CHECK(session());
    manual_via_ble = false;
    e2e_term_ignored = false;
    DSP_CHECK(pump.calls > pumps_before);
    DSP_CHECK_EQ_INT(violations_before, pump.violations);
    DSP_CHECK_EQ_INT(1, g_pump_probed);
    DSP_CHECK(e2e_terms_ignored >= 1);
    DSP_CHECK(TR.st.synth_disconnects >= 1);
    DSP_CHECK(sc.suspends >= 1);
    check_zeroized();
    invariants();
}
#endif

/* T13: the final replies before a close are best effort. Whether the encrypted END_SESSION reply
 * or the third plain MALFORMED reaches the phone or not, the device's outcome is the same: the
 * window ends (END_SESSION) or the connection closes (MALFORMED), every security state is
 * zeroized, and the next connection starts from nothing (its counters reset). */
static void test_lost_final_replies(void)
{
    fresh_unclaimed();
    do_claim();
    window_begin();
    dsp_utc_t t = g_true;
    script_handshake(&t, DSP_SEC_SESSION);
    step(t++, hk(h_drain, 0, 0));
    req_init(0, DSP_BLE_T_END_SESSION);
    step(t++, hk(h_air_loss, 1, 0));
    step(t++, hk(h_req, 0, 0));
    step(t + 5, mk(OP_END_TIMEOUT)); /* not reached: the END was executed */
    DSP_CHECK(session());
    g_air_loss = false;
    drain();
    DSP_CHECK(by_txn(1000) == NULL);                    /* the phone never saw the reply */
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end); /* the device ended the window anyway */
    check_zeroized();

    /* pre-authentication: the third MALFORMED is lost; the connection closes anyway and a new
     * connection is served from a clean state (HELLO answered, no error counted against it) */
    fresh_unclaimed();
    window_begin();
    t = g_true;
    step(t++, hk(h_hello, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_air_loss, 1, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_rx_fault, 0, 0));
    step(t++, hk(h_air_loss, 0, 0));
    step(t++, hk(h_drain, 0, 0));
    step(t++, hk(h_hello, 0, 0)); /* on the next connection */
    step(t++, hk(h_drain, 0, 0));
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    drain();
    DSP_CHECK_EQ_INT(0, plain_fault_errors());
    DSP_CHECK(g_disconnects_seen >= 1);
    DSP_CHECK_EQ_INT(2, count_type(DSP_BLE_T_HELLO));
    DSP_CHECK(rep.links_opened >= 2);
    check_no_transport_fault_close();
    check_zeroized();
    invariants();
}

static void test_fuzz_pre_auth(void)
{
    fresh_unclaimed();
    do_claim();
    uint32_t x = 0xC0FFEEu;
    for (int round = 0; round < 6; round++) {
        window_begin();
        dsp_utc_t t = g_true;
        if (round % 2 == 1) {
            step(t++, hk(h_hello, 0, 0)); /* half the rounds after HELLO */
        }
        for (int k = 0; k < 40; k++) {
            x = x * 1103515245u + 12345u;
            g_raw_len = 1 + (x >> 8) % 60;
            for (size_t i = 0; i < g_raw_len; i++) {
                x = x * 1103515245u + 12345u;
                g_raw[i] = (uint8_t)(x >> 16);
            }
            if (k % 3 == 0 && g_raw_len >= 8) {
                g_raw[0] = 1; /* plausible header */
                g_raw[1] = 0;
                g_raw[6] = (uint8_t)(g_raw_len - 8);
                g_raw[7] = 0;
            }
            step(t++, hk(h_raw, 0, 0));
        }
        step(t + 1, mk(OP_END_TIMEOUT));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(0, (int)rep.ops); /* no dsp_app_* operation was entered */
        check_zeroized();
    }
    invariants();
}

int main(void)
{
    if (!test_rng_install(0xD15EA5E5u)) {
        return 1;
    }
    DSP_RUN(test_claim_session_dispatch);
    DSP_RUN(test_add_second_phone);
    DSP_RUN(test_handshake_negatives);
    DSP_RUN(test_transport_manipulations);
    DSP_RUN(test_response_cache);
    DSP_RUN(test_c10_with_security);
    DSP_RUN(test_lifecycle_events);
    DSP_RUN(test_recovery_flow);
    DSP_RUN(test_identity_key_fault);
    DSP_RUN(test_enroll_name_too_long);
    DSP_RUN(test_hs_msg3_wrong_index);
    DSP_RUN(test_cache_digest_and_epoch);
    DSP_RUN(test_end_during_confirmation);
    DSP_RUN(test_fuzz_pre_auth);
    DSP_RUN(test_rx_fault_policy);
    DSP_RUN(test_lost_final_replies);
#ifdef DSP_VIA_TRANSPORT
    DSP_RUN(test_suspend_without_gap_disconnect);
#endif
#ifdef DSP_VIA_TRANSPORT
    printf("e2e (MTU %u): %d writes in, %d notifications out\n", e2e_mtu, e2e_writes, e2e_frags_out);
    DSP_CHECK(e2e_writes > 0 && e2e_frags_out > 0);
#endif
    return dsp_test_finish();
}
