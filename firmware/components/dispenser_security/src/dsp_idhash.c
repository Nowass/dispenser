/* device_id_hash (docs/BLE_PROTOCOL.md section 11): SHA-256 by libsodium. Separate from
 * dsp_secderive.c so that the PSK code can be checked alone against the PSA provider
 * (test/psk_linux). */
#include "dsp_secderive.h"

#include <string.h>

#include <sodium.h>

#include "dsp_auth.h" /* dsp_secure_zero */

void dsp_id_hash(const uint8_t *id_pub, const uint8_t device_id[6], uint8_t out[DSP_ID_HASH_LEN])
{
    static const uint8_t L_ID[7] = {'D', 'S', 'P', '2', ' ', 'i', 'd'};
    static const uint8_t L_DEV[8] = {'D', 'S', 'P', '2', ' ', 'd', 'e', 'v'};
    crypto_hash_sha256_state st;
    uint8_t h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_init(&st);
    if (id_pub != NULL) {
        crypto_hash_sha256_update(&st, L_ID, sizeof L_ID);
        crypto_hash_sha256_update(&st, id_pub, 32);
    } else {
        crypto_hash_sha256_update(&st, L_DEV, sizeof L_DEV);
        crypto_hash_sha256_update(&st, device_id, 6);
    }
    crypto_hash_sha256_final(&st, h);
    memcpy(out, h, DSP_ID_HASH_LEN);
    dsp_secure_zero(&st, sizeof st);
}
