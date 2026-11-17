#include "dsp_secderive.h"

#include <string.h>

#include "dsp_auth.h" /* dsp_secure_zero */

static const uint8_t PSK_SALT[8] = {'D', 'S', 'P', '2', ' ', 'p', 's', 'k'};
static const uint8_t PROLOGUE_LABEL[11] = {'D', 'S', 'P', '2', ' ', 'B', 'L', 'E', ' ', 'v', '1'};

bool dsp_psk_derive(uint8_t purpose, const uint8_t setup_code[DSP_SETUP_CODE_LEN],
                    const uint8_t *token, uint8_t psk[DSP_PSK_LEN])
{
    if (psk == NULL) {
        return false;
    }
    memset(psk, 0, DSP_PSK_LEN);
    if (setup_code == NULL || purpose < 1 || purpose > 4) {
        return false;
    }
    uint8_t info[1 + DSP_INVITE_TOKEN_LEN];
    size_t info_len = 1;
    info[0] = purpose;
    if (token != NULL) {
        memcpy(info + 1, token, DSP_INVITE_TOKEN_LEN);
        info_len += DSP_INVITE_TOKEN_LEN;
    }
    bool ok = dsp_hkdf_sha256(PSK_SALT, sizeof PSK_SALT, setup_code, DSP_SETUP_CODE_LEN, info,
                              info_len, psk, DSP_PSK_LEN);
    dsp_secure_zero(info, sizeof info); /* holds the invite token */
    if (!ok) {
        dsp_secure_zero(psk, DSP_PSK_LEN);
    }
    return ok;
}

void dsp_prologue_build(uint8_t major, uint8_t minor, uint8_t purpose,
                        const uint8_t id_hash[DSP_ID_HASH_LEN], uint32_t session_id,
                        uint8_t out[DSP_PROLOGUE_LEN])
{
    memcpy(out, PROLOGUE_LABEL, sizeof PROLOGUE_LABEL);
    out[11] = major;
    out[12] = minor;
    out[13] = purpose;
    memcpy(out + 14, id_hash, DSP_ID_HASH_LEN);
    for (int i = 0; i < 4; i++) {
        out[22 + i] = (uint8_t)(session_id >> (8 * i));
    }
}
