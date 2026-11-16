/*
 * M1.10a feasibility gate probe (test code, not product code). Uses only the pinned
 * noise-c/libsodium API: no Noise or primitive logic here. Fixed TEST keys (constants below),
 * never secrets.
 *
 *   nprobe selftest            in-process XXpsk3 + IK, transport both ways, failure cases,
 *                              prints a SHA-256 digest of every byte exchanged (byte-exactness
 *                              across independently compiled builds = equal digests)
 *   nprobe init|resp PATTERN   one side of a handshake + transport over stdin/stdout (hex lines),
 *                              for cross-process interop between independently compiled binaries
 */
#include <noise/protocol.h>
#include <sodium.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NPROBE_NO_MAIN
#define NPROBE_HAS_MAIN 1
#endif

static const char *XX = "Noise_XXpsk3_25519_ChaChaPoly_SHA256";
static const char *IK = "Noise_IK_25519_ChaChaPoly_SHA256";

/* TEST keys only. */
static uint8_t k_init_static[32], k_resp_static[32], k_init_eph[32], k_resp_eph[32], k_psk[32];
static uint8_t k_resp_public[32], k_init_public[32];

/* Randomness: on ESP-IDF the integrator supplies noise_rand_bytes() (NOISE_USE_CUSTOM_RAND, the
   library default there) from the hardware RNG; the host/Android test build of noise-c uses
   libsodium's generator itself. The gate uses fixed ephemerals for the digest anyway. */
#ifdef ESP_PLATFORM
#include "esp_random.h"
void noise_rand_bytes(void *bytes, size_t size);
void noise_rand_bytes(void *bytes, size_t size)
{
    esp_fill_random(bytes, size);
}
#endif

static void fill(uint8_t *k, uint8_t base)
{
    for (int i = 0; i < 32; i++) {
        k[i] = (uint8_t)(base + i);
    }
}

static void keys_init(void)
{
    fill(k_init_static, 0x10);
    fill(k_resp_static, 0x40);
    fill(k_init_eph, 0x70);
    fill(k_resp_eph, 0xA0);
    fill(k_psk, 0xD0);
    crypto_scalarmult_curve25519_base(k_resp_public, k_resp_static);
    crypto_scalarmult_curve25519_base(k_init_public, k_init_static);
}

static crypto_hash_sha256_state g_digest;
static int g_fail;

#define CHECK(c, msg)                                                                  \
    do {                                                                               \
        if (!(c)) {                                                                    \
            printf("FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__);                      \
            g_fail++;                                                                  \
        }                                                                              \
    } while (0)

typedef struct {
    int wrong_psk;      /* responder uses a different PSK */
    int wrong_static;   /* IK: initiator expects a different responder static key */
    int fixed_eph;      /* deterministic ephemerals (digest comparison) */
} variant_t;

static int setup(NoiseHandshakeState **hs, const char *name, int role, const variant_t *v)
{
    int err = noise_handshakestate_new_by_name(hs, name, role);
    if (err != NOISE_ERROR_NONE) {
        return err;
    }
    static const uint8_t prologue[] = "DSP2 BLE v1 probe";
    noise_handshakestate_set_prologue(*hs, prologue, sizeof prologue - 1);
    int init = role == NOISE_ROLE_INITIATOR;
    NoiseDHState *local = noise_handshakestate_get_local_keypair_dh(*hs);
    noise_dhstate_set_keypair_private(local, init ? k_init_static : k_resp_static, 32);
    if (noise_handshakestate_needs_remote_public_key(*hs)) {
        uint8_t pub[32];
        memcpy(pub, k_resp_public, 32);
        if (v->wrong_static) {
            pub[0] ^= 0x01;
        }
        noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(*hs), pub, 32);
    }
    if (noise_handshakestate_needs_pre_shared_key(*hs)) {
        uint8_t psk[32];
        memcpy(psk, k_psk, 32);
        if (v->wrong_psk && !init) {
            psk[31] ^= 0x80;
        }
        noise_handshakestate_set_pre_shared_key(*hs, psk, 32);
        sodium_memzero(psk, sizeof psk);
    }
    if (v->fixed_eph) {
        NoiseDHState *e = noise_handshakestate_get_fixed_ephemeral_dh(*hs);
        if (e != NULL) {
            noise_dhstate_set_keypair_private(e, init ? k_init_eph : k_resp_eph, 32);
        }
    }
    return noise_handshakestate_start(*hs);
}

/* In-process handshake; returns 0 when both sides completed, or the first error. */
static int handshake(NoiseHandshakeState *a, NoiseHandshakeState *b, int *messages)
{
    uint8_t msg[512];
    NoiseHandshakeState *w = a, *r = b;
    *messages = 0;
    for (int guard = 0; guard < 8; guard++) {
        int aa = noise_handshakestate_get_action(a);
        int ba = noise_handshakestate_get_action(b);
        if (aa == NOISE_ACTION_SPLIT && ba == NOISE_ACTION_SPLIT) {
            return 0;
        }
        if (noise_handshakestate_get_action(w) != NOISE_ACTION_WRITE_MESSAGE) {
            NoiseHandshakeState *t = w;
            w = r;
            r = t;
        }
        NoiseBuffer mbuf;
        noise_buffer_set_output(mbuf, msg, sizeof msg);
        int err = noise_handshakestate_write_message(w, &mbuf, NULL);
        if (err != NOISE_ERROR_NONE) {
            return err;
        }
        crypto_hash_sha256_update(&g_digest, msg, mbuf.size);
        (*messages)++;
        err = noise_handshakestate_read_message(r, &mbuf, NULL);
        if (err != NOISE_ERROR_NONE) {
            return err;
        }
        NoiseHandshakeState *t = w;
        w = r;
        r = t;
    }
    return -1;
}

static int transport(NoiseCipherState *send, NoiseCipherState *recv, const char *text, int corrupt,
                     int truncate, int replay)
{
    uint8_t buf[256];
    size_t n = strlen(text);
    memcpy(buf, text, n);
    NoiseBuffer b;
    noise_buffer_set_inout(b, buf, n, sizeof buf);
    if (noise_cipherstate_encrypt(send, &b) != NOISE_ERROR_NONE) {
        return -2;
    }
    crypto_hash_sha256_update(&g_digest, buf, b.size);
    uint8_t copy[256];
    size_t copy_len = b.size;
    memcpy(copy, buf, b.size);
    if (corrupt) {
        buf[3] ^= 0x01;
    }
    if (truncate) {
        b.size -= 1;
    }
    int err = noise_cipherstate_decrypt(recv, &b);
    if (err == NOISE_ERROR_NONE && (b.size != n || memcmp(buf, text, n) != 0)) {
        return -3;
    }
    if (err == NOISE_ERROR_NONE && replay) {
        NoiseBuffer again;
        noise_buffer_set_inout(again, copy, copy_len, sizeof copy);
        return noise_cipherstate_decrypt(recv, &again); /* must fail: nonce advanced */
    }
    return err;
}

static int run_case(const char *name, const variant_t *v, int corrupt, int truncate, int replay,
                    int *hs_err, int *tr_err)
{
    NoiseHandshakeState *i = NULL, *r = NULL;
    NoiseCipherState *is = NULL, *ir = NULL, *rs = NULL, *rr = NULL;
    int messages = 0;
    *tr_err = 0;
    *hs_err = setup(&i, name, NOISE_ROLE_INITIATOR, v);
    if (*hs_err == 0) {
        *hs_err = setup(&r, name, NOISE_ROLE_RESPONDER, v);
    }
    if (*hs_err == 0) {
        *hs_err = handshake(i, r, &messages);
    }
    if (*hs_err == 0) {
        noise_handshakestate_split(i, &is, &ir);
        noise_handshakestate_split(r, &rs, &rr);
        *tr_err = transport(is, rr, "phone->device STATUS", corrupt, truncate, replay);
        if (*tr_err == 0) {
            *tr_err = transport(rs, ir, "device->phone RESPONSE", 0, 0, 0);
        }
    }
    noise_cipherstate_free(is);
    noise_cipherstate_free(ir);
    noise_cipherstate_free(rs);
    noise_cipherstate_free(rr);
    noise_handshakestate_free(i);
    noise_handshakestate_free(r);
    return messages;
}

int nprobe_selftest(void);
int nprobe_selftest(void)
{
    keys_init();
    crypto_hash_sha256_init(&g_digest);
    g_fail = 0;
    const char *patterns[2] = {XX, IK};
    for (int p = 0; p < 2; p++) {
        int he, te;
        variant_t ok = {0, 0, 1};
        int m = run_case(patterns[p], &ok, 0, 0, 0, &he, &te);
        printf("%s: handshake %s (%d messages), transport both ways %s\n", patterns[p],
               he == 0 ? "ok" : "FAILED", m, te == 0 ? "ok" : "FAILED");
        CHECK(he == 0 && te == 0, "valid session");
        CHECK(m == (p == 0 ? 3 : 2), "message count");
        int ce, ct;
        run_case(patterns[p], &ok, 1, 0, 0, &ce, &ct);
        CHECK(ce == 0 && ct == NOISE_ERROR_MAC_FAILURE, "modified ciphertext rejected");
        run_case(patterns[p], &ok, 0, 1, 0, &ce, &ct);
        CHECK(ce == 0 && ct != 0, "truncated ciphertext rejected");
        run_case(patterns[p], &ok, 0, 0, 1, &ce, &ct);
        CHECK(ce == 0 && ct != 0, "replayed transport frame rejected");
        printf("  modified/truncated/replayed transport frames rejected: %s\n", g_fail ? "NO" : "yes");
    }
    int he, te;
    variant_t wrong_psk = {1, 0, 1};
    run_case(XX, &wrong_psk, 0, 0, 0, &he, &te);
    printf("%s with a wrong PSK: %s (err 0x%x)\n", XX, he != 0 ? "rejected" : "ACCEPTED", he);
    CHECK(he != 0, "wrong PSK rejected");
    variant_t wrong_static = {0, 1, 1};
    run_case(IK, &wrong_static, 0, 0, 0, &he, &te);
    printf("%s with a wrong responder static key: %s (err 0x%x)\n", IK, he != 0 ? "rejected" : "ACCEPTED", he);
    CHECK(he != 0, "wrong static key rejected");
    /* Deterministic digest of the valid sessions only (fixed keys and ephemerals). */
    crypto_hash_sha256_init(&g_digest);
    for (int p = 0; p < 2; p++) {
        variant_t ok = {0, 0, 1};
        run_case(patterns[p], &ok, 0, 0, 0, &he, &te);
    }
    uint8_t d[32];
    crypto_hash_sha256_final(&g_digest, d);
    printf("transcript digest: ");
    for (int k = 0; k < 32; k++) {
        printf("%02x", d[k]);
    }
    printf("\n%s (%d failures)\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", g_fail);
    return g_fail;
}

#if NPROBE_HAS_MAIN
/* ---- cross-process mode: hex lines over stdin/stdout ---------------------------------- */

static int read_hex(uint8_t *buf, size_t cap)
{
    char line[2048];
    if (!fgets(line, sizeof line, stdin)) {
        return -1;
    }
    size_t n = strcspn(line, "\r\n") / 2;
    if (n > cap) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(line + 2 * i, "%2x", &v);
        buf[i] = (uint8_t)v;
    }
    return (int)n;
}

static void write_hex(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");
    fflush(stdout);
}

static int peer(int initiator, const char *name)
{
    keys_init();
    variant_t v = {0, 0, 1};
    NoiseHandshakeState *hs = NULL;
    if (setup(&hs, name, initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER, &v) != 0) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    uint8_t msg[512];
    for (;;) {
        int act = noise_handshakestate_get_action(hs);
        NoiseBuffer b;
        if (act == NOISE_ACTION_WRITE_MESSAGE) {
            noise_buffer_set_output(b, msg, sizeof msg);
            if (noise_handshakestate_write_message(hs, &b, NULL) != 0) {
                return 2;
            }
            write_hex(msg, b.size);
        } else if (act == NOISE_ACTION_READ_MESSAGE) {
            int n = read_hex(msg, sizeof msg);
            if (n < 0) {
                return 3;
            }
            noise_buffer_set_input(b, msg, (size_t)n);
            if (noise_handshakestate_read_message(hs, &b, NULL) != 0) {
                fprintf(stderr, "handshake read failed\n");
                return 4;
            }
        } else {
            break;
        }
    }
    NoiseCipherState *send = NULL, *recv = NULL;
    noise_handshakestate_split(hs, &send, &recv);
    const char *mine = initiator ? "phone->device frame" : "device->phone frame";
    const char *theirs = initiator ? "device->phone frame" : "phone->device frame";
    for (int round = 0; round < 3; round++) {
        if (initiator) {
            size_t n = strlen(mine);
            memcpy(msg, mine, n);
            NoiseBuffer b;
            noise_buffer_set_inout(b, msg, n, sizeof msg);
            noise_cipherstate_encrypt(send, &b);
            write_hex(msg, b.size);
        }
        int n = read_hex(msg, sizeof msg);
        NoiseBuffer b;
        noise_buffer_set_inout(b, msg, (size_t)n, sizeof msg);
        if (n < 0 || noise_cipherstate_decrypt(recv, &b) != 0 || b.size != strlen(theirs) ||
            memcmp(msg, theirs, b.size) != 0) {
            fprintf(stderr, "transport decrypt failed in round %d\n", round);
            return 5;
        }
        if (!initiator) {
            size_t m = strlen(mine);
            memcpy(msg, mine, m);
            noise_buffer_set_inout(b, msg, m, sizeof msg);
            noise_cipherstate_encrypt(send, &b);
            write_hex(msg, b.size);
        }
    }
    fprintf(stderr, "%s %s: handshake and 3 transport rounds both ways ok\n",
            initiator ? "initiator" : "responder", name);
    noise_cipherstate_free(send);
    noise_cipherstate_free(recv);
    noise_handshakestate_free(hs);
    return 0;
}

int main(int argc, char **argv)
{
    if (sodium_init() < 0) {
        return 99;
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        return nprobe_selftest() ? 1 : 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "init") == 0 || strcmp(argv[1], "resp") == 0)) {
        const char *name = strcmp(argv[2], "IK") == 0 ? IK : XX;
        return peer(strcmp(argv[1], "init") == 0, name);
    }
    fprintf(stderr, "usage: nprobe selftest | init|resp XX|IK\n");
    return 2;
}
#endif
