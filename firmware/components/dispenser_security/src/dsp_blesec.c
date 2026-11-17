/*
 * BLE security session (M1.10c.2a). See dsp_blesec.h and docs/SECURITY_INTEGRATION_DESIGN.md.
 * The only includer of the restricted dsp_app_sec.h besides dsp_app.c (sec_header_guard.cmake).
 */
#include "dsp_blesec.h"

#include <string.h>

#include <noise/protocol.h>
#include <sodium.h>

#include "dsp_app_sec.h"
#include "dsp_auth.h"
#include "dsp_dispatch.h"
#include "dsp_secderive.h"
#include "dsp_text.h"

#if defined(ESP_PLATFORM) && defined(DSP_TEST_DETERMINISTIC_RNG)
#error "deterministic randomness is a host-test facility; it must never be built into firmware"
#endif

_Static_assert(sizeof(dsp_app_hs_t) <= sizeof(((dsp_blesec_t *)0)->hs_handle),
               "dsp_blesec_t.hs_handle too small for dsp_app_hs_t");

#define PROTO_XX "Noise_XXpsk3_25519_ChaChaPoly_SHA256"
#define PROTO_IK "Noise_IK_25519_ChaChaPoly_SHA256"

/* ---- Helpers ------------------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wipe(void *p, size_t n)
{
    sodium_memzero(p, n);
}

/* Zeroizes and frees everything of the current connection's security session. */
static void session_reset(dsp_blesec_t *s)
{
    if (s->hs != NULL) {
        noise_handshakestate_free(s->hs); /* noise-c clears its objects before freeing */
        s->hs = NULL;
    }
    if (s->tx_cs != NULL) {
        noise_cipherstate_free(s->tx_cs);
        s->tx_cs = NULL;
    }
    if (s->rx_cs != NULL) {
        noise_cipherstate_free(s->rx_cs);
        s->rx_cs = NULL;
    }
    dsp_rescache_clear(&s->cache);
    wipe(&s->auth, sizeof s->auth);
    wipe(s->hs_handle, sizeof s->hs_handle);
    wipe(s->new_priv, sizeof s->new_priv);
    wipe(s->new_pub, sizeof s->new_pub);
    wipe(s->phone_pub, sizeof s->phone_pub);
    wipe(s->name, sizeof s->name);
    wipe(s->transcript, sizeof s->transcript);
    wipe(s->plain, sizeof s->plain);
    wipe(s->out, sizeof s->out);
    wipe(&s->req, sizeof s->req);
    wipe(&s->rsp, sizeof s->rsp);
    wipe(&s->hello, sizeof s->hello);
    dsp_link_close(&s->link);
    s->authed = false;
    s->name_len = 0;
    s->state = DSP_BLESEC_IDLE;
    s->purpose = 0;
    s->proto = 0;
    s->hs_txn = 0;
    s->step_deadline_ms = 0;
    s->tx_count = s->rx_count = 0;
    s->closing_sent = false;
    s->conn_count++;
}

/* Closes the connection (the window continues, C10). on_connection(false) resets the session. */
static void drop(dsp_blesec_t *s, dsp_app_t *app)
{
    dsp_app_sec_drop(app);
    if (s->state != DSP_BLESEC_IDLE) {
        session_reset(s); /* in case the transport was already gone */
    }
}

static void send_plain_error(dsp_app_t *app, dsp_blesec_t *s, uint8_t type, uint16_t txn, uint8_t code)
{
    size_t n = dsp_link_error_frame(type, txn, code, 0, 0, s->out, sizeof s->out);
    if (n > 0) {
        (void)dsp_app_sec_send(app, s->out, n);
    }
}

#define hs_fail(s, app, txn) hs_fail_at((s), (app), (txn), __LINE__)
static void hs_fail_at(dsp_blesec_t *s, dsp_app_t *app, uint16_t txn, int line)
{
    s->fail_line = (uint16_t)line;
    s->hs_failures++;
    send_plain_error(app, s, DSP_BLE_T_HS, txn, DSP_BLE_E_HANDSHAKE_FAILED);
    dsp_app_sec_handshake_failed(app); /* durable diagnostic, at most once per window */
    drop(s, app);
}

/* Encrypts and sends a plaintext body as one frame (header = AD). false: closed. */
static bool send_encrypted(dsp_blesec_t *s, dsp_app_t *app, uint8_t type, uint8_t flags, uint16_t txn,
                           const uint8_t *body, size_t len)
{
    if (s->tx_cs == NULL || len > DSP_BLE_PLAIN_BODY_MAX) {
        drop(s, app);
        return false;
    }
    if (s->tx_count >= DSP_BLESEC_MSG_LIMIT) {
        drop(s, app); /* nonce limit reached: close (design 3) */
        return false;
    }
    uint8_t *f = s->out;
    f[0] = DSP_BLE_MAJOR;
    f[1] = DSP_BLE_MINOR;
    f[2] = type;
    f[3] = (uint8_t)(flags | DSP_BLE_F_ENCRYPTED);
    put16(f + 4, txn);
    put16(f + 6, (uint16_t)(len + DSP_BLE_TAG_LEN));
    if (len > 0 && body != f + DSP_BLE_HEADER_LEN) {
        memmove(f + DSP_BLE_HEADER_LEN, body, len);
    }
    NoiseBuffer buf;
    noise_buffer_set_inout(buf, f + DSP_BLE_HEADER_LEN, len, DSP_BLE_FRAME_MAX - DSP_BLE_HEADER_LEN);
    if (noise_cipherstate_encrypt_with_ad(s->tx_cs, f, DSP_BLE_HEADER_LEN, &buf) != NOISE_ERROR_NONE ||
        buf.size != len + DSP_BLE_TAG_LEN) {
        drop(s, app);
        return false;
    }
    s->tx_count++;
    bool ok = dsp_app_sec_send(app, f, DSP_BLE_HEADER_LEN + buf.size);
    wipe(f, sizeof s->out);
    return ok;
}

static void send_encrypted_error(dsp_blesec_t *s, dsp_app_t *app, uint8_t type, uint16_t txn,
                                 uint8_t code, uint32_t detail, uint32_t aux)
{
    uint8_t b[9];
    b[0] = code;
    for (int i = 0; i < 4; i++) {
        b[1 + i] = (uint8_t)(detail >> (8 * i));
        b[5 + i] = (uint8_t)(aux >> (8 * i));
    }
    (void)send_encrypted(s, app, type, DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR, txn, b, sizeof b);
}

/* Encodes msg's plain body into s->plain; returns the length or -1. */
static int encode_body(dsp_blesec_t *s, const dsp_ble_msg_t *m)
{
    size_t n = 0;
    if (dsp_ble_encode_body(m, s->plain, DSP_BLE_PLAIN_BODY_MAX, &n) != DSP_BLE_OK) {
        return -1;
    }
    return (int)n;
}

static bool entropy_ok(const dsp_blesec_t *s)
{
    return s->cfg.entropy_ready != NULL && s->cfg.entropy_ready(s->cfg.ctx);
}

/* ---- Handshakes --------------------------------------------------------------------------- */

static int local_identity(dsp_blesec_t *s, dsp_app_t *app, const dsp_app_hs_t *hh)
{
    NoiseDHState *dh = noise_handshakestate_get_local_keypair_dh(s->hs);
    if (dh == NULL) {
        return -1;
    }
    if (s->purpose == DSP_SEC_CLAIM || s->purpose == DSP_SEC_RECOVERY) {
        /* a new identity key pair (RF on: hardware entropy) */
        if (noise_dhstate_generate_keypair(dh) != NOISE_ERROR_NONE ||
            noise_dhstate_get_keypair(dh, s->new_priv, 32, s->new_pub, 32) != NOISE_ERROR_NONE) {
            return -1;
        }
        return 0;
    }
    uint8_t pub[32], priv[32], derived[32];
    int rc = -1;
    if (dsp_app_sec_identity(app, hh, pub, priv).status == DSP_OP_OK &&
        noise_dhstate_set_keypair_private(dh, priv, 32) == NOISE_ERROR_NONE &&
        noise_dhstate_get_public_key(dh, derived, 32) == NOISE_ERROR_NONE) {
        if (sodium_memcmp(derived, pub, 32) == 0) {
            s->key_checked = true;
            rc = 0;
        } else {
            (void)dsp_app_sec_key_fault(app); /* dosing inhibited at once (M1.10b.3) */
        }
    }
    wipe(priv, sizeof priv);
    wipe(derived, sizeof derived);
    wipe(pub, sizeof pub);
    return rc;
}

static int set_psk(dsp_blesec_t *s, dsp_app_t *app, const dsp_app_hs_t *hh)
{
    if (s->purpose == DSP_SEC_SESSION) {
        return 0; /* IK uses no PSK */
    }
    uint8_t setup[DSP_SETUP_CODE_LEN], token[DSP_INVITE_TOKEN_LEN], psk[DSP_PSK_LEN];
    int rc = -1;
    bool have_token = true;
    if (s->purpose == DSP_SEC_ADD) {
        have_token = dsp_app_sec_invite_token(app, hh, token).status == DSP_OP_OK;
    }
    if (have_token && s->cfg.read_setup_code != NULL && s->cfg.read_setup_code(s->cfg.ctx, setup) &&
        dsp_psk_derive(s->purpose, setup, s->purpose == DSP_SEC_ADD ? token : NULL, psk) &&
        noise_handshakestate_set_pre_shared_key(s->hs, psk, sizeof psk) == NOISE_ERROR_NONE) {
        rc = 0;
    }
    wipe(setup, sizeof setup);
    wipe(token, sizeof token);
    wipe(psk, sizeof psk);
    return rc;
}

/* Sends the HS response {msg_index 1, noise} (plain). */
static bool send_hs_response(dsp_blesec_t *s, dsp_app_t *app, uint16_t txn, const uint8_t *noise,
                             size_t len)
{
    dsp_ble_msg_t *m = &s->rsp;
    memset(m, 0, sizeof *m);
    m->h.major = DSP_BLE_MAJOR;
    m->h.minor = DSP_BLE_MINOR;
    m->h.type = DSP_BLE_T_HS;
    m->h.flags = DSP_BLE_F_RESPONSE;
    m->h.txn = txn;
    m->b.hs_rsp.msg_index = 1;
    m->b.hs_rsp.noise.len = (uint16_t)len;
    memcpy(m->b.hs_rsp.noise.b, noise, len);
    size_t n = 0;
    bool ok = dsp_ble_encode(m, s->out, sizeof s->out, &n) == DSP_BLE_OK && dsp_app_sec_send(app, s->out, n);
    wipe(m, sizeof *m);
    return ok;
}

static bool split(dsp_blesec_t *s)
{
    if (noise_handshakestate_get_action(s->hs) != NOISE_ACTION_SPLIT ||
        noise_handshakestate_get_handshake_hash(s->hs, s->transcript, sizeof s->transcript) !=
            NOISE_ERROR_NONE ||
        noise_handshakestate_split(s->hs, &s->tx_cs, &s->rx_cs) != NOISE_ERROR_NONE) {
        return false;
    }
    noise_handshakestate_free(s->hs);
    s->hs = NULL;
    s->tx_count = s->rx_count = 0;
    return true;
}

static bool remote_static(dsp_blesec_t *s)
{
    NoiseDHState *dh = noise_handshakestate_get_remote_public_key_dh(s->hs);
    return dh != NULL && noise_dhstate_get_public_key(dh, s->phone_pub, 32) == NOISE_ERROR_NONE;
}

static void send_enroll_result(dsp_blesec_t *s, dsp_app_t *app)
{
    uint8_t slot = s->auth.slot;
    if (send_encrypted(s, app, DSP_BLE_T_ENROLL_RESULT, DSP_BLE_F_RESPONSE, s->hs_txn, &slot, 1)) {
        s->state = DSP_BLESEC_AUTH;
        s->authed = true;
    }
}

/* HS message 0 (NEGOTIATED). */
static void hs_first(dsp_blesec_t *s, dsp_app_t *app, uint16_t txn, const dsp_ble_hs_req_t *r)
{
    if (r->msg_index != 0 || r->purpose < DSP_SEC_SESSION || r->purpose > DSP_SEC_RECOVERY ||
        !entropy_ok(s)) {
        hs_fail(s, app, txn);
        return;
    }
    s->purpose = r->purpose;
    dsp_app_hs_t hh;
    memset(&hh, 0, sizeof hh);
    if (dsp_app_sec_admit(app, (dsp_sec_purpose_t)s->purpose, &hh).status != DSP_OP_OK ||
        noise_handshakestate_new_by_name(&s->hs, s->purpose == DSP_SEC_SESSION ? PROTO_IK : PROTO_XX,
                                         NOISE_ROLE_RESPONDER) != NOISE_ERROR_NONE) {
        wipe(&hh, sizeof hh);
        hs_fail(s, app, txn);
        return;
    }
    uint8_t pro[DSP_PROLOGUE_LEN];
    dsp_prologue_build(s->proto, DSP_BLE_MINOR, s->purpose, s->hello.device_id_hash,
                       s->hello.session_id, pro);
    if (noise_handshakestate_set_prologue(s->hs, pro, sizeof pro) != NOISE_ERROR_NONE ||
        local_identity(s, app, &hh) != 0 || set_psk(s, app, &hh) != 0 ||
        noise_handshakestate_start(s->hs) != NOISE_ERROR_NONE) {
        wipe(&hh, sizeof hh);
        hs_fail(s, app, txn);
        return;
    }
    /* read message 1 (payload must be empty) */
    uint8_t *work = s->plain;
    uint8_t payload[DSP_BLE_PLAIN_BODY_MAX];
    memcpy(work, r->noise.b, r->noise.len);
    NoiseBuffer mb, pb;
    noise_buffer_set_input(mb, work, r->noise.len);
    noise_buffer_set_output(pb, payload, sizeof payload);
    if (noise_handshakestate_read_message(s->hs, &mb, &pb) != NOISE_ERROR_NONE || pb.size != 0) {
        wipe(payload, sizeof payload);
        wipe(&hh, sizeof hh);
        hs_fail(s, app, txn);
        return;
    }
    wipe(payload, sizeof payload);
    if (s->purpose == DSP_SEC_SESSION) {
        /* IK: the phone's static key is known now; authorize BEFORE answering */
        if (!remote_static(s) ||
            dsp_app_sec_lookup(app, &hh, s->phone_pub, &s->auth).status != DSP_OP_OK) {
            wipe(&hh, sizeof hh);
            hs_fail(s, app, txn); /* unknown or revoked key: no hint which input was wrong */
            return;
        }
    }
    if (s->purpose != DSP_SEC_SESSION) {
        memcpy(s->hs_handle, &hh, sizeof hh); /* needed for the commit after message 3 */
    }
    wipe(&hh, sizeof hh);
    NoiseBuffer ob;
    noise_buffer_set_output(ob, work, DSP_BLE_PLAIN_BODY_MAX - 2);
    if (noise_handshakestate_write_message(s->hs, &ob, NULL) != NOISE_ERROR_NONE ||
        !send_hs_response(s, app, txn, work, ob.size)) {
        hs_fail(s, app, txn);
        return;
    }
    if (s->purpose == DSP_SEC_SESSION) {
        if (!split(s)) {
            drop(s, app);
            return;
        }
        s->state = DSP_BLESEC_AUTH;
        s->authed = true;
        return;
    }
    s->state = DSP_BLESEC_HANDSHAKING;
    s->step_deadline_ms = dsp_app_sec_now_ms(app) + DSP_BLESEC_STEP_MS;
}

/* HS message 2 (XX message 3, HANDSHAKING). */
static void hs_third(dsp_blesec_t *s, dsp_app_t *app, uint16_t txn, const dsp_ble_hs_req_t *r)
{
    if (r->msg_index != 2 || r->purpose != s->purpose || s->hs == NULL) {
        hs_fail(s, app, txn);
        return;
    }
    uint8_t *work = s->plain;
    uint8_t payload[DSP_BLE_PLAIN_BODY_MAX];
    memcpy(work, r->noise.b, r->noise.len);
    NoiseBuffer mb, pb;
    noise_buffer_set_input(mb, work, r->noise.len);
    noise_buffer_set_output(pb, payload, sizeof payload);
    bool ok = noise_handshakestate_read_message(s->hs, &mb, &pb) == NOISE_ERROR_NONE &&
              /* payload: name str(0..24, text) = len u8 || bytes, nothing else */
              pb.size >= 1 && payload[0] <= DSP_BLESEC_NAME_MAX && pb.size == 1u + payload[0] &&
              dsp_utf8_valid(payload + 1, payload[0], true) && remote_static(s);
    if (ok) {
        s->name_len = payload[0];
        memcpy(s->name, payload + 1, s->name_len);
    }
    wipe(payload, sizeof payload);
    if (!ok || !split(s)) {
        hs_fail(s, app, txn);
        return;
    }
    s->hs_txn = txn;
    s->step_deadline_ms = 0;
    /* the handshake evidence handle issued at admission (message 0); single use */
    dsp_app_hs_t hh;
    memcpy(&hh, s->hs_handle, sizeof hh);
    wipe(s->hs_handle, sizeof s->hs_handle);
    dsp_app_op_result_t res;
    if (s->purpose == DSP_SEC_RECOVERY) {
        res = dsp_app_sec_recovery_begin(app, &hh, s->transcript);
        wipe(&hh, sizeof hh);
        if (res.status == DSP_OP_OK) {
            const uint8_t ev[2] = {1 /* RECOVERY */, (uint8_t)(DSP_CONFIRM_WINDOW_MS / 1000u)};
            if (send_encrypted(s, app, DSP_BLE_T_EV_CONFIRM_ON_DEVICE, DSP_BLE_F_EVENT, 0, ev, sizeof ev)) {
                s->state = DSP_BLESEC_CONFIRMING;
            }
            return;
        }
    } else {
        res = dsp_app_sec_enroll(app, &hh, s->phone_pub, (const char *)s->name, s->name_len,
                                 s->purpose == DSP_SEC_CLAIM ? s->new_priv : NULL,
                                 s->purpose == DSP_SEC_CLAIM ? s->new_pub : NULL, &s->auth);
        wipe(&hh, sizeof hh);
        wipe(s->new_priv, sizeof s->new_priv);
        if (res.status == DSP_OP_OK) {
            send_enroll_result(s, app);
            return;
        }
    }
    /* the commit was refused (preconditions changed, slots full, storage): encrypted error */
    send_encrypted_error(s, app, DSP_BLE_T_ENROLL_RESULT, txn, dsp_dispatch_error_code(res.status),
                         res.detail, res.aux);
    dsp_app_sec_handshake_failed(app);
    drop(s, app);
}

/* ---- Encrypted frames --------------------------------------------------------------------- */

static void handle_encrypted(dsp_blesec_t *s, dsp_app_t *app, const uint8_t *frame, size_t len,
                             const dsp_ble_header_t *h)
{
    if ((s->state != DSP_BLESEC_AUTH && s->state != DSP_BLESEC_CONFIRMING) || s->rx_cs == NULL ||
        s->rx_count >= DSP_BLESEC_MSG_LIMIT) {
        drop(s, app); /* encrypted frame without a session, or nonce limit: security error */
        return;
    }
    size_t clen = len - DSP_BLE_HEADER_LEN;
    memcpy(s->plain, frame + DSP_BLE_HEADER_LEN, clen);
    NoiseBuffer buf;
    noise_buffer_set_input(buf, s->plain, clen);
    if (noise_cipherstate_decrypt_with_ad(s->rx_cs, frame, DSP_BLE_HEADER_LEN, &buf) != NOISE_ERROR_NONE) {
        drop(s, app); /* modified, truncated, replayed, reordered or wrong key */
        return;
    }
    s->rx_count++;
    size_t plen = buf.size;
    dsp_ble_class_t cls = DSP_BLE_CLASS_PLAIN;
    dsp_ble_header_t ph = *h;
    ph.flags = (uint8_t)(h->flags & ~DSP_BLE_F_ENCRYPTED);
    ph.len = (uint16_t)plen;
    s->req.h = ph;
    if (ph.flags != 0 || dsp_ble_layout_for(ph.type, ph.flags, &cls) == NULL ||
        cls == DSP_BLE_CLASS_PLAIN ||
        dsp_ble_decode_body(&ph, s->plain, plen, DSP_BLE_MINOR, &s->req.b) != DSP_BLE_OK) {
        /* C6: encrypted MALFORMED, then close */
        send_encrypted_error(s, app, h->type, h->txn, DSP_BLE_E_MALFORMED, 0, 0);
        drop(s, app);
        return;
    }
    if (s->state == DSP_BLESEC_CONFIRMING) {
        if (ph.type == DSP_BLE_T_END_SESSION) {
            /* the recovering phone is not authorized: only its connection ends (C10) */
            (void)send_encrypted(s, app, DSP_BLE_T_END_SESSION, DSP_BLE_F_RESPONSE, ph.txn, NULL, 0);
            drop(s, app);
        } else {
            send_encrypted_error(s, app, ph.type, ph.txn, DSP_BLE_E_BUSY, DSP_OP_B_CONFIRMING, 0);
        }
        return;
    }
    /* AUTHENTICATED: the response cache first (C4) */
    dsp_rescache_bind_t bind = {s->hello.session_id, s->conn_count, dsp_app_sec_epoch(app), s->auth.slot};
    dsp_rescache_purge_epoch(&s->cache, bind.epoch);
    const dsp_rescache_entry_t *hit = NULL;
    dsp_rescache_result_t cr = dsp_rescache_lookup(&s->cache, &bind, ph.type, ph.txn, s->plain, plen, &hit);
    if (cr == DSP_RESCACHE_HIT) {
        (void)send_encrypted(s, app, hit->type, hit->rsp_flags, hit->txn, hit->rsp, hit->rsp_len);
        return;
    }
    if (cr == DSP_RESCACHE_CONFLICT) {
        send_encrypted_error(s, app, ph.type, ph.txn, DSP_BLE_E_MALFORMED, 0, 0); /* not executed */
        return;
    }
    /* keep the request plaintext for the cache digest (dispatch may reuse s->plain) */
    uint8_t reqcopy[DSP_BLE_PLAIN_BODY_MAX];
    memcpy(reqcopy, s->plain, plen);
    uint32_t conn = s->conn_count;
    dsp_dispatch_fx_t fx;
    dsp_app_auth_t auth = s->auth;
    dsp_dispatch(app, &auth, &s->req, &s->rsp, &fx);
    wipe(&auth, sizeof auth);
    if (s->conn_count != conn || s->state != DSP_BLESEC_AUTH) {
        wipe(reqcopy, sizeof reqcopy); /* the operation closed the connection (BLE suspended) */
        return;
    }
    int n = encode_body(s, &s->rsp);
    if (n < 0) {
        wipe(reqcopy, sizeof reqcopy);
        send_encrypted_error(s, app, ph.type, ph.txn, DSP_BLE_E_DEGRADED, 0, 0);
        return;
    }
    bind.epoch = dsp_app_sec_epoch(app); /* an ownership change binds to the new epoch */
    dsp_rescache_purge_epoch(&s->cache, bind.epoch);
    if (ph.type != DSP_BLE_T_END_SESSION) {
        dsp_rescache_store(&s->cache, &bind, ph.type, ph.txn, reqcopy, plen, s->rsp.h.flags, s->plain,
                           (size_t)n);
    }
    wipe(reqcopy, sizeof reqcopy);
    bool sent = send_encrypted(s, app, s->rsp.h.type, s->rsp.h.flags, s->rsp.h.txn, s->plain, (size_t)n);
    if (sent && fx.close_after) {
        drop(s, app); /* own slot removed: the session ends after the response */
    }
}

/* ---- Hooks -------------------------------------------------------------------------------- */

static void on_connection(void *ctx, dsp_app_t *app, bool open)
{
    dsp_blesec_t *s = ctx;
    if (s->state != DSP_BLESEC_IDLE || s->hs != NULL || s->tx_cs != NULL) {
        session_reset(s);
    }
    if (open) {
        dsp_link_open(&s->link, 0, dsp_app_sec_now_ms(app));
        s->state = DSP_BLESEC_OPEN;
    }
}

static void on_frame(void *ctx, dsp_app_t *app, const uint8_t *frame, size_t len)
{
    dsp_blesec_t *s = ctx;
    if (s->state == DSP_BLESEC_IDLE || frame == NULL) {
        return;
    }
    dsp_ble_header_t h;
    const uint8_t *body = NULL;
    dsp_ble_result_t pr = dsp_ble_parse_header(frame, len, &h, &body);
    bool enc = len >= 4 && (frame[3] & DSP_BLE_F_ENCRYPTED) != 0;
    if (enc) {
        if (pr != DSP_BLE_OK || (h.flags & (DSP_BLE_F_RESPONSE | DSP_BLE_F_EVENT)) != 0) {
            drop(s, app); /* malformed encrypted header: security error */
            return;
        }
        handle_encrypted(s, app, frame, len, &h);
        return;
    }
    if (s->state == DSP_BLESEC_AUTH || s->state == DSP_BLESEC_CONFIRMING) {
        /* after authentication only a plain END_SESSION is accepted: it closes this connection */
        if (pr == DSP_BLE_OK && h.type == DSP_BLE_T_END_SESSION && h.flags == 0 && h.len == 0) {
            dsp_ble_msg_t *m = &s->rsp;
            memset(m, 0, sizeof *m);
            m->h.major = DSP_BLE_MAJOR;
            m->h.type = DSP_BLE_T_END_SESSION;
            m->h.flags = DSP_BLE_F_RESPONSE;
            m->h.txn = h.txn;
            size_t n = 0;
            if (dsp_ble_encode(m, s->out, sizeof s->out, &n) == DSP_BLE_OK) {
                (void)dsp_app_sec_send(app, s->out, n);
            }
        }
        drop(s, app); /* any other plain frame after authentication: security error */
        return;
    }
    if (pr == DSP_BLE_OK && h.type == DSP_BLE_T_HS && h.flags == 0 &&
        (s->state == DSP_BLESEC_NEGOTIATED || s->state == DSP_BLESEC_HANDSHAKING)) {
        if (dsp_ble_decode_body(&h, body, h.len, DSP_BLE_MINOR, &s->req.b) != DSP_BLE_OK) {
            hs_fail(s, app, h.txn);
            return;
        }
        dsp_ble_hs_req_t r = s->req.b.hs_req;
        if (s->state == DSP_BLESEC_NEGOTIATED) {
            hs_first(s, app, h.txn, &r);
        } else {
            hs_third(s, app, h.txn, &r);
        }
        wipe(&r, sizeof r);
        return;
    }
    if (s->state == DSP_BLESEC_HANDSHAKING) {
        hs_fail(s, app, h.txn); /* anything but the next handshake message */
        return;
    }
    /* OPEN / NEGOTIATED: the M1.10c.1 gate (HELLO, END_SESSION, errors) */
    dsp_link_hello_t hello;
    dsp_app_sec_hello(app, &hello);
    uint8_t before = s->link.state;
    dsp_link_result_t lr = dsp_link_on_frame(&s->link, frame, len, &hello, s->out, sizeof s->out);
    if (lr.actions & DSP_LINK_ACT_REPLY) {
        (void)dsp_app_sec_send(app, s->out, lr.out_len);
    }
    if (before == DSP_LINK_OPEN && s->link.state == DSP_LINK_NEGOTIATED) {
        s->hello = hello; /* the values the prologue binds */
        s->proto = s->link.proto;
        s->state = DSP_BLESEC_NEGOTIATED;
    }
    if (lr.actions & DSP_LINK_ACT_CLOSE) {
        drop(s, app);
    }
}

static uint64_t on_tick(void *ctx, dsp_app_t *app, uint64_t now)
{
    dsp_blesec_t *s = ctx;
    switch (s->state) {
    case DSP_BLESEC_OPEN:
    case DSP_BLESEC_NEGOTIATED:
        if (dsp_link_expired(&s->link, now)) {
            drop(s, app); /* unauthenticated for 30 s */
            return 0;
        }
        return s->link.opened_ms + DSP_LINK_UNAUTH_MS;
    case DSP_BLESEC_HANDSHAKING:
        if (now >= s->step_deadline_ms || dsp_link_expired(&s->link, now)) {
            dsp_app_sec_handshake_failed(app);
            drop(s, app); /* step timeout */
            return 0;
        }
        return s->step_deadline_ms;
    case DSP_BLESEC_CONFIRMING: {
        dsp_confirm_state_t c = dsp_app_sec_recovery_state(app);
        if (c == DSP_CONFIRM_CONFIRMED) {
            dsp_app_op_result_t r = dsp_app_sec_recovery_apply(app, s->transcript, s->phone_pub,
                                                               (const char *)s->name, s->name_len,
                                                               s->new_priv, s->new_pub, &s->auth);
            wipe(s->new_priv, sizeof s->new_priv);
            if (r.status == DSP_OP_OK) {
                send_enroll_result(s, app);
            } else {
                send_encrypted_error(s, app, DSP_BLE_T_ENROLL_RESULT, s->hs_txn,
                                     dsp_dispatch_error_code(r.status), r.detail, r.aux);
                drop(s, app);
            }
        } else if (c != DSP_CONFIRM_PENDING) {
            /* aborted, wrong count or timed out: encrypted REFUSED, then close */
            send_encrypted_error(s, app, DSP_BLE_T_ENROLL_RESULT, s->hs_txn, DSP_BLE_E_REFUSED, 0, 0);
            drop(s, app);
        }
        return 0; /* the Application Service polls a pending confirmation every 200 ms */
    }
    case DSP_BLESEC_AUTH: {
        uint32_t left = dsp_app_sec_window_seconds_left(app);
        if (!s->closing_sent && left <= DSP_BLESEC_CLOSING_NOTICE_S) {
            uint8_t ev[3] = {2 /* MAX */, (uint8_t)left, (uint8_t)(left >> 8)};
            s->closing_sent = send_encrypted(s, app, DSP_BLE_T_EV_SESSION_CLOSING, DSP_BLE_F_EVENT, 0, ev,
                                             sizeof ev);
            return 0;
        }
        return left > DSP_BLESEC_CLOSING_NOTICE_S
                   ? now + (uint64_t)(left - DSP_BLESEC_CLOSING_NOTICE_S) * 1000u
                   : 0;
    }
    default:
        return 0;
    }
}

static void on_suspend(void *ctx, dsp_app_t *app)
{
    dsp_blesec_t *s = ctx;
    if (s->state == DSP_BLESEC_AUTH) {
        const uint8_t ev[2] = {0, 0}; /* seconds_until 0: suspending now */
        (void)send_encrypted(s, app, DSP_BLE_T_EV_SUSPENDING_FOR_DOSE, DSP_BLE_F_EVENT, 0, ev, sizeof ev);
    }
}

static bool sec_random(void *ctx, uint8_t *out, size_t len)
{
    dsp_blesec_t *s = ctx;
    return entropy_ok(s) && s->cfg.random != NULL && s->cfg.random(s->cfg.ctx, out, len);
}

static bool sec_id_hash(void *ctx, const uint8_t *id_pub, uint8_t out[8])
{
    dsp_blesec_t *s = ctx;
    dsp_id_hash(id_pub, s->cfg.device_id, out);
    return true;
}

void dsp_blesec_init(dsp_blesec_t *s, const dsp_blesec_config_t *cfg, dsp_app_security_t *sec)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    memset(sec, 0, sizeof *sec);
    sec->ctx = s;
    sec->random = sec_random;
    sec->device_id_hash = sec_id_hash;
    sec->on_connection = on_connection;
    sec->on_frame = on_frame;
    sec->on_tick = on_tick;
    sec->on_suspend = on_suspend;
}
