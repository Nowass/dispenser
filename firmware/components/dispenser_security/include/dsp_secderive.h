#ifndef DSP_SECDERIVE_H
#define DSP_SECDERIVE_H

/*
 * Session-security derivations (docs/BLE_PROTOCOL.md section 11; M1.10c.2a, decision C1).
 * Independent reference vectors: test-vectors/ble_session_v1.txt (Python standard library).
 *
 *   PSK      = HKDF-SHA256(salt = "DSP2 psk", ikm = setup_code(16),
 *                          info = purpose u8 || invite_token(16, ADD only), L = 32)
 *   prologue = "DSP2 BLE v1" || major u8 || minor u8 || purpose u8 || device_id_hash(8)
 *              || session_id u32 LE
 *   device_id_hash = SHA-256("DSP2 id" || id_pub(32))[0..8)     (claimed)
 *                  = SHA-256("DSP2 dev" || device_id(6))[0..8)  (unclaimed / damaged)
 *
 * HKDF provider (no own primitive): mbedTLS / TF-PSA-Crypto PSA on the ESP32-C3 (and the ESP-IDF
 * linux-target check, test/psk_linux); libsodium's crypto_kdf_hkdf_sha256 in the host tests (the
 * Android provider). SHA-256: libsodium. Every intermediate secret is cleared on every path.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_PSK_LEN 32
#define DSP_SETUP_CODE_LEN 16
#define DSP_INVITE_TOKEN_LEN 16
#define DSP_PROLOGUE_LEN 26 /* 11 + 3 + 8 + 4 */
#define DSP_ID_HASH_LEN 8

/* RFC 5869 HKDF-SHA256 (extract + expand) by the build's vetted provider. false = failure
 * (out cleared). Implemented in dsp_hkdf_psa.c (ESP-IDF) or dsp_hkdf_sodium.c (host). */
bool dsp_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

/* token NULL for CLAIM and RECOVERY (and SESSION, which uses no PSK). false = invalid purpose or
 * provider failure; *psk is cleared then. The caller clears psk after use. */
bool dsp_psk_derive(uint8_t purpose, const uint8_t setup_code[DSP_SETUP_CODE_LEN],
                    const uint8_t *token, uint8_t psk[DSP_PSK_LEN]);

void dsp_prologue_build(uint8_t major, uint8_t minor, uint8_t purpose,
                        const uint8_t id_hash[DSP_ID_HASH_LEN], uint32_t session_id,
                        uint8_t out[DSP_PROLOGUE_LEN]);

/* id_pub NULL -> device form over device_id (6 bytes). */
void dsp_id_hash(const uint8_t *id_pub, const uint8_t device_id[6], uint8_t out[DSP_ID_HASH_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* DSP_SECDERIVE_H */
