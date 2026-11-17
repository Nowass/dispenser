/* PSA provider + production PSK code against test-vectors/ble_session_v1.txt (see CMakeLists.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_secderive.h"

static int hexfield(const char *line, const char *key, uint8_t *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat);
    if (p == NULL) {
        return -1;
    }
    p += strlen(pat);
    if (*p == '-') {
        return 0;
    }
    if (strncmp(p, "x:", 2) != 0) {
        return -1;
    }
    p += 2;
    size_t n = 0;
    while (p[0] && p[1] && p[0] != ' ' && p[0] != '\n') {
        unsigned v;
        if (n >= cap || sscanf(p, "%2x", &v) != 1) {
            return -1;
        }
        out[n++] = (uint8_t)v;
        p += 2;
    }
    return (int)n;
}

int main(void)
{
    FILE *f = fopen(DSP_SESSION_VECTORS, "r");
    if (f == NULL) {
        printf("FAIL  vectors not found: %s\n", DSP_SESSION_VECTORS);
        return 1;
    }
    char line[1024];
    int pos = 0, neg = 0, bad = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        bool is_pos = strncmp(line, "psk ", 4) == 0, is_neg = strncmp(line, "psk_not ", 8) == 0;
        if (!is_pos && !is_neg) {
            continue;
        }
        uint8_t setup[16], token[16], want[32], wrong[32], got[32];
        const char *pp = strstr(line, " purpose=");
        long purpose = pp != NULL ? strtol(pp + 9, NULL, 10) : -1;
        int tl = hexfield(line, "token", token, sizeof token);
        if (hexfield(line, "setup", setup, sizeof setup) != 16 || (tl != 0 && tl != 16) ||
            hexfield(line, "psk", want, sizeof want) != 32 ||
            !dsp_psk_derive((uint8_t)purpose, setup, tl == 16 ? token : NULL, got) ||
            memcmp(got, want, 32) != 0) {
            bad++;
            continue;
        }
        if (is_neg) {
            if (hexfield(line, "wrong", wrong, sizeof wrong) != 32 || memcmp(got, wrong, 32) == 0) {
                bad++;
                continue;
            }
            neg++;
        } else {
            pos++;
        }
    }
    fclose(f);
    uint8_t psk[32];
    memset(psk, 0xEE, sizeof psk);
    uint8_t setup[16] = {0};
    bool fail_clears = !dsp_psk_derive(0, setup, NULL, psk) && psk[0] == 0 && psk[31] == 0;
    printf("%s  PSA HKDF (mbedTLS/TF-PSA-Crypto, ESP-IDF v6.1): %d psk + %d psk_not vectors, %d mismatches\n",
           (bad == 0 && pos >= 24 && neg >= 12) ? "PASS" : "FAIL", pos, neg, bad);
    printf("%s  failure path clears the output\n", fail_clears ? "PASS" : "FAIL");
    return (bad == 0 && pos >= 24 && neg >= 12 && fail_clears) ? 0 : 1;
}
