#include "dsp_link.h"

#include <string.h>

#include "dsp_bleproto.h"

void dsp_link_open(dsp_link_t *l, uint32_t gen, uint64_t now_ms)
{
    if (l == NULL) {
        return;
    }
    memset(l, 0, sizeof *l);
    l->state = DSP_LINK_OPEN;
    l->gen = gen;
    l->opened_ms = now_ms;
}

void dsp_link_close(dsp_link_t *l)
{
    if (l != NULL) {
        uint32_t gen = l->gen;
        memset(l, 0, sizeof *l);
        l->gen = gen; /* stale frames of this generation stay recognizable */
    }
}

bool dsp_link_is_open(const dsp_link_t *l)
{
    return l != NULL && l->state != DSP_LINK_IDLE;
}

bool dsp_link_expired(const dsp_link_t *l, uint64_t now_ms)
{
    return dsp_link_is_open(l) && (now_ms < l->opened_ms || now_ms - l->opened_ms >= DSP_LINK_UNAUTH_MS);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

size_t dsp_link_error_frame(uint8_t type, uint16_t txn, uint8_t code, uint32_t detail,
                            uint32_t aux, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < DSP_BLE_HEADER_LEN + 9) {
        return 0;
    }
    out[0] = DSP_BLE_MAJOR;
    out[1] = DSP_BLE_MINOR;
    out[2] = type;
    out[3] = DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR;
    put16(out + 4, txn);
    put16(out + 6, 9);
    out[8] = code;
    put32(out + 9, detail);
    put32(out + 13, aux);
    return DSP_BLE_HEADER_LEN + 9;
}

static dsp_link_result_t error_reply(dsp_link_t *l, dsp_link_result_t r, uint16_t txn, uint8_t code,
                                     uint32_t detail, uint8_t *out, size_t cap)
{
    r.out_len = dsp_link_error_frame(r.type, txn, code, detail, 0, out, cap);
    r.actions |= r.out_len > 0 ? DSP_LINK_ACT_REPLY : 0;
    r.error = code;
    if (++l->errors >= DSP_LINK_ERRORS_MAX) {
        r.actions |= DSP_LINK_ACT_CLOSE;
    }
    return r;
}

static bool encode_reply(const dsp_ble_msg_t *m, uint8_t *out, size_t cap, size_t *len)
{
    return dsp_ble_encode(m, out, cap, len) == DSP_BLE_OK;
}

dsp_link_result_t dsp_link_on_frame(dsp_link_t *l, const uint8_t *frame, size_t len,
                                    const dsp_link_hello_t *hello, uint8_t *out, size_t cap)
{
    dsp_link_result_t r;
    memset(&r, 0, sizeof r);
    if (l == NULL || !dsp_link_is_open(l) || frame == NULL || out == NULL || hello == NULL) {
        r.actions = DSP_LINK_ACT_CLOSE;
        return r;
    }
    uint16_t txn = 0;
    if (len >= 6) {
        txn = (uint16_t)(frame[4] | (frame[5] << 8));
    }
    r.type = len >= 3 ? frame[2] : 0;
    if (len >= 4 && (frame[3] & DSP_BLE_F_ENCRYPTED) != 0) {
        /* no security session exists: an encrypted frame is a security error */
        r.actions = DSP_LINK_ACT_CLOSE;
        return r;
    }
    dsp_ble_header_t h;
    const uint8_t *body = NULL;
    dsp_ble_result_t pr = dsp_ble_parse_header(frame, len, &h, &body);
    if (pr == DSP_BLE_ERR_BAD_MAJOR) {
        return error_reply(l, r, txn, DSP_BLE_E_UNSUPPORTED_VERSION,
                           DSP_LINK_PROTO_MIN | (DSP_LINK_PROTO_MAX << 8), out, cap);
    }
    if (pr == DSP_BLE_ERR_UNKNOWN_TYPE) {
        return error_reply(l, r, txn, DSP_BLE_E_UNKNOWN_TYPE, 0, out, cap);
    }
    if (pr != DSP_BLE_OK || (h.flags & (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR | DSP_BLE_F_EVENT)) != 0) {
        return error_reply(l, r, txn, DSP_BLE_E_MALFORMED, 0, out, cap);
    }
    dsp_ble_msg_t m;
    memset(&m, 0, sizeof m);
    switch (h.type) {
    case DSP_BLE_T_HELLO: {
        dsp_ble_body_t b;
        if (l->state != DSP_LINK_OPEN ||
            dsp_ble_decode_body(&h, body, h.len, DSP_BLE_MINOR, &b) != DSP_BLE_OK) {
            return error_reply(l, r, txn, DSP_BLE_E_MALFORMED, 0, out, cap);
        }
        uint8_t major = 0;
        if (!dsp_ble_negotiate(DSP_LINK_PROTO_MIN, DSP_LINK_PROTO_MAX, b.hello_req.proto_min,
                               b.hello_req.proto_max, &major)) {
            return error_reply(l, r, txn, DSP_BLE_E_UNSUPPORTED_VERSION,
                               DSP_LINK_PROTO_MIN | (DSP_LINK_PROTO_MAX << 8), out, cap);
        }
        m.h.major = DSP_BLE_MAJOR;
        m.h.minor = DSP_BLE_MINOR;
        m.h.type = DSP_BLE_T_HELLO;
        m.h.flags = DSP_BLE_F_RESPONSE;
        m.h.txn = txn;
        m.b.hello_rsp.proto = major;
        m.b.hello_rsp.claim_state = hello->claim_state;
        m.b.hello_rsp.flags = (uint8_t)(hello->flags & DSP_BLE_HELLO_F_ALL);
        m.b.hello_rsp.caps = hello->caps;
        m.b.hello_rsp.session_id = hello->session_id;
        memcpy(m.b.hello_rsp.device_id_hash, hello->device_id_hash, 8);
        if (!encode_reply(&m, out, cap, &r.out_len)) {
            r.actions = DSP_LINK_ACT_CLOSE;
            return r;
        }
        l->state = DSP_LINK_NEGOTIATED;
        l->proto = major;
        r.actions = DSP_LINK_ACT_REPLY;
        return r;
    }
    case DSP_BLE_T_END_SESSION:
        m.h.major = DSP_BLE_MAJOR;
        m.h.minor = DSP_BLE_MINOR;
        m.h.type = DSP_BLE_T_END_SESSION;
        m.h.flags = DSP_BLE_F_RESPONSE;
        m.h.txn = txn;
        /* C10: an unauthenticated END_SESSION ends only this connection, never the service
         * window (ending that needs an authorized owner: dsp_app_end_service_session) */
        r.actions = DSP_LINK_ACT_CLOSE;
        if (encode_reply(&m, out, cap, &r.out_len)) {
            r.actions |= DSP_LINK_ACT_REPLY;
        }
        return r;
    case DSP_BLE_T_HS:
        if (l->state != DSP_LINK_NEGOTIATED) {
            return error_reply(l, r, txn, DSP_BLE_E_MALFORMED, 0, out, cap);
        }
        /* M1.10c.1: no security layer, no handshake can complete */
        r.out_len = dsp_link_error_frame(r.type, txn, DSP_BLE_E_HANDSHAKE_FAILED, 0, 0, out, cap);
        r.error = DSP_BLE_E_HANDSHAKE_FAILED;
        r.actions = (r.out_len > 0 ? DSP_LINK_ACT_REPLY : 0) | DSP_LINK_ACT_CLOSE;
        return r;
    default:
        /* every other request type is privileged: never decoded, never dispatched */
        return error_reply(l, r, txn, DSP_BLE_E_UNAUTHENTICATED, 0, out, cap);
    }
}
