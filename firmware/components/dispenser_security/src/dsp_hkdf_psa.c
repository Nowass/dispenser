/* HKDF-SHA256 by mbedTLS / TF-PSA-Crypto (PSA key derivation), the device provider (decision
 * C1). The secret is passed as bytes (no persistent key object exists to destroy); the
 * derivation operation is aborted on every path, which wipes its internal PRK state. */
#include "dsp_secderive.h"

#include <string.h>

#include "psa/crypto.h"

#include "dsp_auth.h"

bool dsp_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    memset(out, 0, out_len);
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) ==
                  PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len) ==
                  PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len) ==
                  PSA_SUCCESS &&
              psa_key_derivation_output_bytes(&op, out, out_len) == PSA_SUCCESS;
    (void)psa_key_derivation_abort(&op);
    if (!ok) {
        dsp_secure_zero(out, out_len);
    }
    return ok;
}
