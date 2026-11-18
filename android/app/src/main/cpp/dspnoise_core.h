#ifndef DSPNOISE_CORE_H
#define DSPNOISE_CORE_H

/*
 * Dispenser Android native Noise core (M1.10e.1, docs/ANDROID_BLE_DESIGN.md section 3.1).
 *
 * A thin C layer over the pinned, patched noise-c + libsodium (the same sources and patches as the
 * firmware, fetched and verified by firmware/third_party/noise-c/noise_c_deps.cmake). No
 * cryptographic logic of its own: it only selects the two approved protocols, passes keys,
 * prologue and PSK to noise-c, and copies buffers. Every secret buffer it owns is cleared with
 * sodium_memzero before release; noise-c clears its own objects when freed.
 *
 * Only the INITIATOR role (the phone) with random ephemerals is available here. Responder role and
 * fixed ephemerals exist only in dspnoise_test_hooks.c, compiled into the host test library and
 * never into the APK (checked by the native symbol check of the app build).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSPN_OK = 0,
    DSPN_E_ARG = -1,     /* invalid argument (length, pattern, missing key) */
    DSPN_E_NOISE = -2,   /* noise-c refused: authentication failure, wrong state, short buffer */
    DSPN_E_STATE = -3,   /* operation not valid in the handshake's current state */
    DSPN_E_NOMEM = -4,
    DSPN_E_INIT = -5,    /* sodium_init failed or the library was not initialized */
};

enum { DSPN_XXPSK3 = 0, DSPN_IK = 1 };

#define DSPN_KEY_LEN 32
#define DSPN_PSK_LEN 32
#define DSPN_SETUP_LEN 16
#define DSPN_TOKEN_LEN 16
#define DSPN_HASH_LEN 32
#define DSPN_TAG_LEN 16
#define DSPN_MSG_MAX 512 /* maximum HS noise field and frame body of the protocol */

typedef struct dspn_hs dspn_hs;
typedef struct dspn_cs dspn_cs;

/* sodium_init (idempotent). Must succeed before any other call. */
int dspn_init(void);
/* libsodium's randombytes implementation in use (diagnostics; "internal" or "sysrandom"). */
const char *dspn_rng_name(void);

/* PSK = HKDF-SHA256(salt "DSP2 psk", ikm = setup code, info = purpose || token) (approved C1). */
int dspn_psk(uint8_t purpose, const uint8_t setup[DSPN_SETUP_LEN], const uint8_t *token,
             uint8_t out[DSPN_PSK_LEN]);
/* New X25519 key pair from libsodium's CSPRNG. */
int dspn_keypair(uint8_t priv[DSPN_KEY_LEN], uint8_t pub[DSPN_KEY_LEN]);
/* Public key of a private key. */
int dspn_public_key(const uint8_t priv[DSPN_KEY_LEN], uint8_t pub[DSPN_KEY_LEN]);

/* Initiator handshake: XXpsk3 needs psk (remote_pub NULL); IK needs remote_pub (psk NULL). */
int dspn_hs_new(dspn_hs **out, int pattern, const uint8_t *prologue, size_t prologue_len,
                const uint8_t priv[DSPN_KEY_LEN], const uint8_t *remote_pub, const uint8_t *psk);
int dspn_hs_write(dspn_hs *hs, const uint8_t *payload, size_t payload_len, uint8_t *out, size_t cap,
                  size_t *out_len);
int dspn_hs_read(dspn_hs *hs, const uint8_t *msg, size_t msg_len, uint8_t *payload, size_t cap,
                 size_t *payload_len);
/* 1 when the handshake is complete (split possible), 0 otherwise. */
int dspn_hs_finished(const dspn_hs *hs);
int dspn_hs_remote_static(const dspn_hs *hs, uint8_t out[DSPN_KEY_LEN]);
int dspn_hs_hash(const dspn_hs *hs, uint8_t out[DSPN_HASH_LEN]);
/* Split into the two transport CipherStates (send, receive for this role). */
int dspn_hs_split(dspn_hs *hs, dspn_cs **out);
void dspn_hs_free(dspn_hs *hs);

/* AEAD with the 8-byte frame header as associated data; output = ciphertext || tag. */
int dspn_cs_encrypt(dspn_cs *cs, const uint8_t *ad, size_t ad_len, const uint8_t *plain, size_t plain_len,
                    uint8_t *out, size_t cap, size_t *out_len);
int dspn_cs_decrypt(dspn_cs *cs, const uint8_t *ad, size_t ad_len, const uint8_t *ct, size_t ct_len,
                    uint8_t *out, size_t cap, size_t *out_len);
void dspn_cs_free(dspn_cs *cs);

/* internal: shared by the core and the test hooks. Without DSPN_TEST_HOOKS (every APK build) only
 * initiator = 1 and fixed_ephemeral = NULL are accepted, and the fixed-ephemeral code is not compiled. */
int dspn_hs_create(dspn_hs **out, int pattern, int initiator, const uint8_t *prologue, size_t prologue_len,
                   const uint8_t *priv, const uint8_t *remote_pub, const uint8_t *psk,
                   const uint8_t *fixed_ephemeral);

#ifdef __cplusplus
}
#endif

#endif /* DSPNOISE_CORE_H */
