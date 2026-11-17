/*
 * M1.10c.1 unit tests: the pre-authentication protocol gate (dsp_link). Every request type is
 * checked: only HELLO and END_SESSION are served, HS fails (no security layer yet), encrypted
 * frames close, every other type is UNAUTHENTICATED; malformed, unknown and unsupported frames get
 * their plain error; three errors close; the unauthenticated deadline; random frames never crash.
 */
#include "dsp_bleproto.h"
#include "dsp_link.h"
#include "dsp_test.h"

#include <stdint.h>
#include <string.h>

static uint8_t out[DSP_BLE_FRAME_MAX];
static const dsp_link_hello_t HELLO_STATE = {1, DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED, 0x02,
                                             0xA1B2C3D4u, {1, 2, 3, 4, 5, 6, 7, 8}};

static size_t frame(uint8_t *f, uint8_t type, uint8_t flags, uint16_t txn, const uint8_t *body,
                    uint16_t len)
{
    f[0] = 1;
    f[1] = 0;
    f[2] = type;
    f[3] = flags;
    f[4] = (uint8_t)txn;
    f[5] = (uint8_t)(txn >> 8);
    f[6] = (uint8_t)len;
    f[7] = (uint8_t)(len >> 8);
    if (len > 0) {
        memcpy(f + 8, body, len);
    }
    return 8u + len;
}

static dsp_link_result_t feed(dsp_link_t *l, const uint8_t *f, size_t n)
{
    memset(out, 0xEE, sizeof out);
    return dsp_link_on_frame(l, f, n, &HELLO_STATE, out, sizeof out);
}

static void expect_error(dsp_link_result_t r, uint8_t code, uint16_t txn)
{
    DSP_CHECK(r.actions & DSP_LINK_ACT_REPLY);
    DSP_CHECK_EQ_INT(code, r.error);
    dsp_ble_msg_t m;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(out, r.out_len, DSP_BLE_MINOR, &m));
    DSP_CHECK_EQ_INT(DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR, m.h.flags);
    DSP_CHECK_EQ_INT(txn, m.h.txn);
    DSP_CHECK_EQ_INT(code, m.b.error.code);
    DSP_CHECK_EQ_INT(0, m.b.error.aux); /* nothing secret-dependent */
}

static void test_hello_and_negotiation(void)
{
    dsp_link_t l;
    dsp_link_open(&l, 7, 1000);
    uint8_t f[64];
    /* no common major: UNSUPPORTED_VERSION with the device range, stays OPEN */
    const uint8_t v23[2] = {2, 3};
    dsp_link_result_t r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 5, v23, 2));
    expect_error(r, DSP_BLE_E_UNSUPPORTED_VERSION, 5);
    DSP_CHECK_EQ_INT(DSP_LINK_OPEN, l.state);
    /* HELLO 1..3: negotiated major 1, the public state reported */
    const uint8_t v13[2] = {1, 3};
    r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 6, v13, 2));
    DSP_CHECK_EQ_INT(DSP_LINK_ACT_REPLY, r.actions);
    dsp_ble_msg_t m;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(out, r.out_len, DSP_BLE_MINOR, &m));
    DSP_CHECK_EQ_INT(DSP_BLE_F_RESPONSE, m.h.flags);
    DSP_CHECK_EQ_INT(6, m.h.txn);
    DSP_CHECK_EQ_INT(1, m.b.hello_rsp.proto);
    DSP_CHECK_EQ_INT(1, m.b.hello_rsp.claim_state);
    DSP_CHECK_EQ_INT(DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED, m.b.hello_rsp.flags);
    DSP_CHECK_EQ_INT(0x02, m.b.hello_rsp.caps);
    DSP_CHECK(m.b.hello_rsp.session_id == 0xA1B2C3D4u);
    DSP_CHECK(memcmp(m.b.hello_rsp.device_id_hash, HELLO_STATE.device_id_hash, 8) == 0);
    DSP_CHECK_EQ_INT(DSP_LINK_NEGOTIATED, l.state);
    /* a second HELLO is malformed (OPEN only) */
    r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 7, v13, 2));
    expect_error(r, DSP_BLE_E_MALFORMED, 7);
    /* a bad HELLO body */
    dsp_link_open(&l, 8, 1000);
    const uint8_t bad[3] = {1, 1, 0};
    r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 9, bad, 3));
    expect_error(r, DSP_BLE_E_MALFORMED, 9);
}

static void test_handshake_fails_without_security_layer(void)
{
    dsp_link_t l;
    dsp_link_open(&l, 1, 0);
    uint8_t f[96];
    uint8_t hs[34] = {2, 0};
    /* before HELLO: malformed */
    dsp_link_result_t r = feed(&l, f, frame(f, DSP_BLE_T_HS, 0, 3, hs, sizeof hs));
    expect_error(r, DSP_BLE_E_MALFORMED, 3);
    DSP_CHECK(!(r.actions & DSP_LINK_ACT_CLOSE));
    const uint8_t v[2] = {1, 1};
    (void)feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 4, v, 2));
    r = feed(&l, f, frame(f, DSP_BLE_T_HS, 0, 5, hs, sizeof hs));
    expect_error(r, DSP_BLE_E_HANDSHAKE_FAILED, 5);
    DSP_CHECK(r.actions & DSP_LINK_ACT_CLOSE);
}

/* Every request type other than HELLO, HS and END_SESSION: UNAUTHENTICATED, never decoded. */
static void test_every_privileged_type_unauthenticated(void)
{
    int checked = 0;
    for (int t = 0; t < 256; t++) {
        /* a known request type: the header of a body-less request parses */
        uint8_t probe[8] = {1, 0, (uint8_t)t, 0, 0, 0, 0, 0};
        dsp_ble_header_t ph;
        const uint8_t *pb;
        if (dsp_ble_parse_header(probe, sizeof probe, &ph, &pb) != DSP_BLE_OK ||
            t == DSP_BLE_T_HELLO || t == DSP_BLE_T_HS || t == DSP_BLE_T_END_SESSION) {
            continue;
        }
        dsp_link_t l;
        dsp_link_open(&l, 1, 0);
        uint8_t f[16];
        /* even a body the decoder would reject: the gate refuses before decoding */
        const uint8_t junk[3] = {0xFF, 0xFF, 0xFF};
        dsp_link_result_t r = feed(&l, f, frame(f, (uint8_t)t, 0, (uint16_t)t, junk, 3));
        expect_error(r, DSP_BLE_E_UNAUTHENTICATED, (uint16_t)t);
        checked++;
    }
    DSP_CHECK_EQ_INT(24, checked); /* 32 types - 4 events - ENROLL_RESULT - HELLO, HS, END_SESSION */
}

static void test_encrypted_and_malformed(void)
{
    dsp_link_t l;
    dsp_link_open(&l, 1, 0);
    uint8_t f[64];
    uint8_t ct[20];
    memset(ct, 0x5A, sizeof ct);
    /* ENCRYPTED without a session: close, no reply */
    dsp_link_result_t r = feed(&l, f, frame(f, 0x10, DSP_BLE_F_ENCRYPTED, 1, ct, sizeof ct));
    DSP_CHECK_EQ_INT(DSP_LINK_ACT_CLOSE, r.actions);
    DSP_CHECK_EQ_INT(0, r.out_len);
    /* unknown type, bad major, response/event flags from the client, truncated */
    dsp_link_open(&l, 2, 0);
    r = feed(&l, f, frame(f, 0x55, 0, 2, NULL, 0));
    /* the reply echoes the unknown type (the sender's codec knows it): checked byte by byte */
    DSP_CHECK(r.actions & DSP_LINK_ACT_REPLY);
    DSP_CHECK(r.out_len == 17 && out[2] == 0x55 && out[3] == (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR) &&
              out[4] == 2 && out[8] == DSP_BLE_E_UNKNOWN_TYPE);
    size_t n = frame(f, DSP_BLE_T_HELLO, 0, 3, (const uint8_t[]){1, 1}, 2);
    f[0] = 2;
    r = feed(&l, f, n);
    expect_error(r, DSP_BLE_E_UNSUPPORTED_VERSION, 3);
    DSP_CHECK(!(r.actions & DSP_LINK_ACT_CLOSE)); /* second error */
    r = feed(&l, f, frame(f, 0x10, 0, 4, NULL, 0));
    expect_error(r, DSP_BLE_E_UNAUTHENTICATED, 4);
    DSP_CHECK(r.actions & DSP_LINK_ACT_CLOSE); /* third error */
    dsp_link_open(&l, 3, 0);
    r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, DSP_BLE_F_RESPONSE, 4, NULL, 0));
    expect_error(r, DSP_BLE_E_MALFORMED, 4);
    r = feed(&l, f, 5);
    DSP_CHECK(r.actions & DSP_LINK_ACT_REPLY);
    DSP_CHECK_EQ_INT(DSP_BLE_E_MALFORMED, r.error);
    r = feed(&l, f, frame(f, 0x20, 0, 9, NULL, 0));
    DSP_CHECK(r.actions & DSP_LINK_ACT_CLOSE);
    /* a closed link refuses everything */
    dsp_link_close(&l);
    r = feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 1, (const uint8_t[]){1, 1}, 2));
    DSP_CHECK_EQ_INT(DSP_LINK_ACT_CLOSE, r.actions);
    DSP_CHECK_EQ_INT(3, l.gen); /* the generation stays recognizable */
}

static void test_end_session_and_deadline(void)
{
    dsp_link_t l;
    dsp_link_open(&l, 1, 5000);
    uint8_t f[16];
    dsp_link_result_t r = feed(&l, f, frame(f, DSP_BLE_T_END_SESSION, 0, 0x77, NULL, 0));
    /* C10: reply, then only this connection closes (the gate has no way to end the window) */
    DSP_CHECK_EQ_INT(DSP_LINK_ACT_REPLY | DSP_LINK_ACT_CLOSE, r.actions);
    dsp_ble_msg_t m;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(out, r.out_len, DSP_BLE_MINOR, &m));
    DSP_CHECK_EQ_INT(DSP_BLE_T_END_SESSION, m.h.type);
    DSP_CHECK_EQ_INT(0x77, m.h.txn);
    DSP_CHECK(!dsp_link_expired(&l, 5000 + DSP_LINK_UNAUTH_MS - 1));
    DSP_CHECK(dsp_link_expired(&l, 5000 + DSP_LINK_UNAUTH_MS));
    DSP_CHECK(dsp_link_expired(&l, 4999)); /* clock went back */
    dsp_link_close(&l);
    DSP_CHECK(!dsp_link_expired(&l, 1000000));
}

static uint32_t rng = 12345u;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void test_fuzz(void)
{
    int replies = 0, closes = 0;
    for (int i = 0; i < 100000; i++) {
        dsp_link_t l;
        dsp_link_open(&l, 1, 0);
        if (rnd() % 2) {
            uint8_t f[16];
            (void)feed(&l, f, frame(f, DSP_BLE_T_HELLO, 0, 1, (const uint8_t[]){1, 1}, 2));
        }
        uint8_t f[DSP_BLE_FRAME_MAX + 8];
        size_t n = rnd() % sizeof f;
        for (size_t k = 0; k < n; k++) {
            f[k] = (uint8_t)rnd();
        }
        if (n >= 8 && rnd() % 2) { /* mostly well-formed headers */
            f[0] = 1;
            f[3] &= 0x0F;
            uint16_t len = (uint16_t)(n - 8);
            f[6] = (uint8_t)len;
            f[7] = (uint8_t)(len >> 8);
        }
        dsp_link_result_t r = feed(&l, f, n);
        DSP_CHECK(r.out_len <= DSP_BLE_FRAME_MAX);
        DSP_CHECK((r.actions & ~(DSP_LINK_ACT_REPLY | DSP_LINK_ACT_CLOSE)) == 0);
        if (r.actions & DSP_LINK_ACT_REPLY) {
            DSP_CHECK(r.out_len >= 8 && out[0] == 1 && (out[3] & DSP_BLE_F_RESPONSE) != 0);
            replies++;
        }
        closes += (r.actions & DSP_LINK_ACT_CLOSE) != 0;
    }
    DSP_CHECK(replies > 1000 && closes > 1000);
}

int main(void)
{
    DSP_RUN(test_hello_and_negotiation);
    DSP_RUN(test_handshake_fails_without_security_layer);
    DSP_RUN(test_every_privileged_type_unauthenticated);
    DSP_RUN(test_encrypted_and_malformed);
    DSP_RUN(test_end_session_and_deadline);
    DSP_RUN(test_fuzz);
    return dsp_test_finish();
}
