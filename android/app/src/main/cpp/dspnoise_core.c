/*
 * Dispenser Android native Noise core (M1.10e.1). See dspnoise_core.h. Only noise-c and libsodium
 * calls; no primitive, no Noise logic of our own.
 */
#include "dspnoise_core.h"

#include <stdlib.h>
#include <string.h>

#include <noise/protocol.h>
#include <sodium.h>

#if defined(__ANDROID__) && defined(__OPTIMIZE__) && !defined(_FORTIFY_SOURCE)
#error "optimized Android builds of the Noise provider require FORTIFY (_FORTIFY_SOURCE)"
#endif

static const char *const PROTO[2] = {"Noise_XXpsk3_25519_ChaChaPoly_SHA256", "Noise_IK_25519_ChaChaPoly_SHA256"};
#define HS_MAGIC 0x44534e48u /* "DSNH" */
#define CS_MAGIC 0x44534e43u /* "DSNC" */

struct dspn_hs {
    uint32_t magic;
    NoiseHandshakeState *hs;
};

struct dspn_cs {
    uint32_t magic;
    NoiseCipherState *send, *recv;
};

static int g_inited;

/* noise-c's randomness hook (NOISE_USE_CUSTOM_RAND, the library default): libsodium's CSPRNG. */
void noise_rand_bytes(void *bytes, size_t size);
void noise_rand_bytes(void *bytes, size_t size)
{
    randombytes_buf(bytes, size);
}

int dspn_init(void)
{
    if (g_inited) {
        return DSPN_OK;
    }
    if (sodium_init() < 0) {
        return DSPN_E_INIT;
    }
    g_inited = 1;
    return DSPN_OK;
}

const char *dspn_rng_name(void)
{
    return g_inited ? randombytes_implementation_name() : "uninitialized";
}

int dspn_psk(uint8_t purpose, const uint8_t setup[DSPN_SETUP_LEN], const uint8_t *token, uint8_t out[DSPN_PSK_LEN])
{
    if (!g_inited) {
        return DSPN_E_INIT;
    }
    if (setup == NULL || out == NULL || purpose < 2 || purpose > 4 || (purpose == 3) != (token != NULL)) {
        return DSPN_E_ARG;
    }
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    uint8_t info[1 + DSPN_TOKEN_LEN];
    size_t info_len = 1;
    info[0] = purpose;
    if (token != NULL) {
        memcpy(info + 1, token, DSPN_TOKEN_LEN);
        info_len += DSPN_TOKEN_LEN;
    }
    static const uint8_t salt[8] = {'D', 'S', 'P', '2', ' ', 'p', 's', 'k'};
    int rc = crypto_kdf_hkdf_sha256_extract(prk, salt, sizeof salt, setup, DSPN_SETUP_LEN) == 0 &&
                     crypto_kdf_hkdf_sha256_expand(out, DSPN_PSK_LEN, (const char *)info, info_len, prk) == 0
                 ? DSPN_OK
                 : DSPN_E_NOISE;
    sodium_memzero(prk, sizeof prk);
    sodium_memzero(info, sizeof info);
    if (rc != DSPN_OK) {
        sodium_memzero(out, DSPN_PSK_LEN);
    }
    return rc;
}

int dspn_keypair(uint8_t priv[DSPN_KEY_LEN], uint8_t pub[DSPN_KEY_LEN])
{
    if (!g_inited) {
        return DSPN_E_INIT;
    }
    if (priv == NULL || pub == NULL) {
        return DSPN_E_ARG;
    }
    NoiseDHState *dh = NULL;
    if (noise_dhstate_new_by_name(&dh, "25519") != NOISE_ERROR_NONE) {
        return DSPN_E_NOMEM;
    }
    int rc = noise_dhstate_generate_keypair(dh) == NOISE_ERROR_NONE &&
                     noise_dhstate_get_keypair(dh, priv, DSPN_KEY_LEN, pub, DSPN_KEY_LEN) == NOISE_ERROR_NONE
                 ? DSPN_OK
                 : DSPN_E_NOISE;
    noise_dhstate_free(dh);
    if (rc != DSPN_OK) {
        sodium_memzero(priv, DSPN_KEY_LEN);
    }
    return rc;
}

int dspn_public_key(const uint8_t priv[DSPN_KEY_LEN], uint8_t pub[DSPN_KEY_LEN])
{
    if (!g_inited) {
        return DSPN_E_INIT;
    }
    if (priv == NULL || pub == NULL) {
        return DSPN_E_ARG;
    }
    NoiseDHState *dh = NULL;
    if (noise_dhstate_new_by_name(&dh, "25519") != NOISE_ERROR_NONE) {
        return DSPN_E_NOMEM;
    }
    int rc = noise_dhstate_set_keypair_private(dh, priv, DSPN_KEY_LEN) == NOISE_ERROR_NONE &&
                     noise_dhstate_get_public_key(dh, pub, DSPN_KEY_LEN) == NOISE_ERROR_NONE
                 ? DSPN_OK
                 : DSPN_E_NOISE;
    noise_dhstate_free(dh);
    return rc;
}

int dspn_hs_create(dspn_hs **out, int pattern, int initiator, const uint8_t *prologue, size_t prologue_len,
                   const uint8_t *priv, const uint8_t *remote_pub, const uint8_t *psk,
                   const uint8_t *fixed_ephemeral)
{
    if (out == NULL) {
        return DSPN_E_ARG;
    }
    *out = NULL;
    if (!g_inited) {
        return DSPN_E_INIT;
    }
    if ((pattern != DSPN_XXPSK3 && pattern != DSPN_IK) || priv == NULL || prologue == NULL ||
        prologue_len == 0 || prologue_len > 64) {
        return DSPN_E_ARG;
    }
#ifndef DSPN_TEST_HOOKS
    /* the APK: the phone is always the initiator and its ephemerals always come from the CSPRNG */
    if (!initiator || fixed_ephemeral != NULL) {
        return DSPN_E_ARG;
    }
#endif
    /* XXpsk3 always has a PSK and no pre-known remote key; IK has no PSK, the initiator knows rs */
    if ((pattern == DSPN_XXPSK3 && (psk == NULL || remote_pub != NULL)) ||
        (pattern == DSPN_IK && (psk != NULL || (initiator && remote_pub == NULL) || (!initiator && remote_pub != NULL)))) {
        return DSPN_E_ARG;
    }
    dspn_hs *h = calloc(1, sizeof *h);
    if (h == NULL) {
        return DSPN_E_NOMEM;
    }
    h->magic = HS_MAGIC;
    int rc = DSPN_E_NOISE;
    if (noise_handshakestate_new_by_name(&h->hs, PROTO[pattern],
                                         initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER) != NOISE_ERROR_NONE) {
        free(h);
        return DSPN_E_NOISE;
    }
    NoiseDHState *local = noise_handshakestate_get_local_keypair_dh(h->hs);
    if (noise_handshakestate_set_prologue(h->hs, prologue, prologue_len) == NOISE_ERROR_NONE && local != NULL &&
        noise_dhstate_set_keypair_private(local, priv, DSPN_KEY_LEN) == NOISE_ERROR_NONE) {
        rc = DSPN_OK;
        if (remote_pub != NULL) {
            NoiseDHState *rs = noise_handshakestate_get_remote_public_key_dh(h->hs);
            rc = rs != NULL && noise_dhstate_set_public_key(rs, remote_pub, DSPN_KEY_LEN) == NOISE_ERROR_NONE
                     ? DSPN_OK
                     : DSPN_E_NOISE;
        }
        if (rc == DSPN_OK && psk != NULL) {
            rc = noise_handshakestate_set_pre_shared_key(h->hs, psk, DSPN_PSK_LEN) == NOISE_ERROR_NONE ? DSPN_OK
                                                                                                         : DSPN_E_NOISE;
        }
#ifdef DSPN_TEST_HOOKS
        /* host test library only: fixed ephemerals for the shared transcripts */
        if (rc == DSPN_OK && fixed_ephemeral != NULL) {
            NoiseDHState *e = noise_handshakestate_get_fixed_ephemeral_dh(h->hs);
            rc = e != NULL && noise_dhstate_set_keypair_private(e, fixed_ephemeral, DSPN_KEY_LEN) == NOISE_ERROR_NONE
                     ? DSPN_OK
                     : DSPN_E_NOISE;
        }
#endif
        if (rc == DSPN_OK) {
            rc = noise_handshakestate_start(h->hs) == NOISE_ERROR_NONE ? DSPN_OK : DSPN_E_NOISE;
        }
    }
    if (rc != DSPN_OK) {
        dspn_hs_free(h);
        return rc;
    }
    *out = h;
    return DSPN_OK;
}

int dspn_hs_new(dspn_hs **out, int pattern, const uint8_t *prologue, size_t prologue_len,
                const uint8_t priv[DSPN_KEY_LEN], const uint8_t *remote_pub, const uint8_t *psk)
{
    return dspn_hs_create(out, pattern, 1, prologue, prologue_len, priv, remote_pub, psk, NULL);
}

static int hs_ok(const dspn_hs *hs)
{
    return hs != NULL && hs->magic == HS_MAGIC && hs->hs != NULL;
}

int dspn_hs_write(dspn_hs *hs, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!hs_ok(hs) || out == NULL || out_len == NULL || (payload == NULL && payload_len > 0) ||
        payload_len > DSPN_MSG_MAX) {
        return DSPN_E_ARG;
    }
    *out_len = 0;
    if (noise_handshakestate_get_action(hs->hs) != NOISE_ACTION_WRITE_MESSAGE) {
        return DSPN_E_STATE;
    }
    NoiseBuffer mb, pb;
    noise_buffer_set_output(mb, out, cap);
    int err;
    if (payload_len > 0) {
        noise_buffer_set_input(pb, (uint8_t *)payload, payload_len);
        err = noise_handshakestate_write_message(hs->hs, &mb, &pb);
    } else {
        err = noise_handshakestate_write_message(hs->hs, &mb, NULL);
    }
    if (err != NOISE_ERROR_NONE) {
        sodium_memzero(out, cap);
        return DSPN_E_NOISE;
    }
    *out_len = mb.size;
    return DSPN_OK;
}

int dspn_hs_read(dspn_hs *hs, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap, size_t *payload_len)
{
    if (!hs_ok(hs) || msg == NULL || payload == NULL || payload_len == NULL || msg_len > DSPN_MSG_MAX) {
        return DSPN_E_ARG;
    }
    *payload_len = 0;
    if (noise_handshakestate_get_action(hs->hs) != NOISE_ACTION_READ_MESSAGE) {
        return DSPN_E_STATE;
    }
    uint8_t work[DSPN_MSG_MAX];
    memcpy(work, msg, msg_len); /* noise-c decrypts in place */
    NoiseBuffer mb, pb;
    noise_buffer_set_input(mb, work, msg_len);
    noise_buffer_set_output(pb, payload, cap);
    int err = noise_handshakestate_read_message(hs->hs, &mb, &pb);
    sodium_memzero(work, sizeof work);
    if (err != NOISE_ERROR_NONE) {
        sodium_memzero(payload, cap);
        return DSPN_E_NOISE;
    }
    *payload_len = pb.size;
    return DSPN_OK;
}

int dspn_hs_finished(const dspn_hs *hs)
{
    return hs_ok(hs) && noise_handshakestate_get_action(hs->hs) == NOISE_ACTION_SPLIT;
}

int dspn_hs_remote_static(const dspn_hs *hs, uint8_t out[DSPN_KEY_LEN])
{
    if (!hs_ok(hs) || out == NULL) {
        return DSPN_E_ARG;
    }
    NoiseDHState *rs = noise_handshakestate_get_remote_public_key_dh(hs->hs);
    if (rs == NULL || !noise_dhstate_has_public_key(rs)) {
        return DSPN_E_STATE;
    }
    return noise_dhstate_get_public_key(rs, out, DSPN_KEY_LEN) == NOISE_ERROR_NONE ? DSPN_OK : DSPN_E_NOISE;
}

int dspn_hs_hash(const dspn_hs *hs, uint8_t out[DSPN_HASH_LEN])
{
    if (!hs_ok(hs) || out == NULL) {
        return DSPN_E_ARG;
    }
    if (!dspn_hs_finished(hs)) {
        return DSPN_E_STATE;
    }
    return noise_handshakestate_get_handshake_hash(hs->hs, out, DSPN_HASH_LEN) == NOISE_ERROR_NONE ? DSPN_OK
                                                                                                   : DSPN_E_NOISE;
}

int dspn_hs_split(dspn_hs *hs, dspn_cs **out)
{
    if (out == NULL) {
        return DSPN_E_ARG;
    }
    *out = NULL;
    if (!hs_ok(hs)) {
        return DSPN_E_ARG;
    }
    if (!dspn_hs_finished(hs)) {
        return DSPN_E_STATE;
    }
    dspn_cs *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return DSPN_E_NOMEM;
    }
    c->magic = CS_MAGIC;
    if (noise_handshakestate_split(hs->hs, &c->send, &c->recv) != NOISE_ERROR_NONE) {
        dspn_cs_free(c);
        return DSPN_E_NOISE;
    }
    *out = c;
    return DSPN_OK;
}

void dspn_hs_free(dspn_hs *hs)
{
    if (hs == NULL || hs->magic != HS_MAGIC) {
        return;
    }
    if (hs->hs != NULL) {
        noise_handshakestate_free(hs->hs); /* noise-c clears the state before freeing */
    }
    sodium_memzero(hs, sizeof *hs);
    free(hs);
}

static int cs_ok(const dspn_cs *cs)
{
    return cs != NULL && cs->magic == CS_MAGIC && cs->send != NULL && cs->recv != NULL;
}

int dspn_cs_encrypt(dspn_cs *cs, const uint8_t *ad, size_t ad_len, const uint8_t *plain, size_t plain_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    if (!cs_ok(cs) || ad == NULL || out == NULL || out_len == NULL || (plain == NULL && plain_len > 0) ||
        plain_len > DSPN_MSG_MAX || cap < plain_len + DSPN_TAG_LEN) {
        return DSPN_E_ARG;
    }
    *out_len = 0;
    if (plain_len > 0) {
        memmove(out, plain, plain_len);
    }
    NoiseBuffer b;
    noise_buffer_set_inout(b, out, plain_len, cap);
    if (noise_cipherstate_encrypt_with_ad(cs->send, ad, ad_len, &b) != NOISE_ERROR_NONE) {
        sodium_memzero(out, cap);
        return DSPN_E_NOISE;
    }
    *out_len = b.size;
    return DSPN_OK;
}

int dspn_cs_decrypt(dspn_cs *cs, const uint8_t *ad, size_t ad_len, const uint8_t *ct, size_t ct_len,
                    uint8_t *out, size_t cap, size_t *out_len)
{
    if (!cs_ok(cs) || ad == NULL || ct == NULL || out == NULL || out_len == NULL || ct_len < DSPN_TAG_LEN ||
        ct_len > DSPN_MSG_MAX + DSPN_TAG_LEN || cap < ct_len) {
        return DSPN_E_ARG;
    }
    *out_len = 0;
    memmove(out, ct, ct_len);
    NoiseBuffer b;
    noise_buffer_set_input(b, out, ct_len);
    if (noise_cipherstate_decrypt_with_ad(cs->recv, ad, ad_len, &b) != NOISE_ERROR_NONE) {
        sodium_memzero(out, cap);
        return DSPN_E_NOISE;
    }
    *out_len = b.size;
    return DSPN_OK;
}

void dspn_cs_free(dspn_cs *cs)
{
    if (cs == NULL || cs->magic != CS_MAGIC) {
        return;
    }
    if (cs->send != NULL) {
        noise_cipherstate_free(cs->send);
    }
    if (cs->recv != NULL) {
        noise_cipherstate_free(cs->recv);
    }
    sodium_memzero(cs, sizeof *cs);
    free(cs);
}
