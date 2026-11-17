/* HKDF-SHA256 by libsodium (crypto_kdf_hkdf_sha256_extract/expand): the provider of the host
 * tests and of the Android app (decision C1). Host builds only (the ESP-IDF build uses the PSA
 * provider); the PRK is cleared on every path. */
#include "dsp_secderive.h"

#include <string.h>

#include <sodium.h>

bool dsp_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || out_len > crypto_kdf_hkdf_sha256_BYTES_MAX) {
        return false;
    }
    memset(out, 0, out_len);
    uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
    bool ok = crypto_kdf_hkdf_sha256_extract(prk, salt, salt_len, ikm, ikm_len) == 0 &&
              crypto_kdf_hkdf_sha256_expand(out, out_len, (const char *)info, info_len, prk) == 0;
    sodium_memzero(prk, sizeof prk);
    if (!ok) {
        sodium_memzero(out, out_len);
    }
    return ok;
}
