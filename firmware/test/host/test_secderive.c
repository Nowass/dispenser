/*
 * M1.10c.2a: session-security derivations against the independent reference vectors
 * (test-vectors/ble_session_v1.txt, Python standard library; cross-checked with OpenSSL).
 * Provider here: libsodium's crypto_kdf_hkdf_sha256 (the Android provider). The device provider
 * (mbedTLS / TF-PSA-Crypto PSA) runs the same vectors in test/psk_linux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "dsp_secderive.h"
#include "dsp_test.h"

#ifndef DSP_VECTORS_DIR
#error "DSP_VECTORS_DIR must be defined"
#endif

static int n_psk, n_not, n_pro, n_id;

/* value of key=... in line; returns length of hex-decoded bytes or -1 ("-" -> 0) */
static int field(const char *line, const char *key, uint8_t *out, size_t cap)
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

static long num(const char *line, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat);
    return p == NULL ? -1 : strtol(p + strlen(pat), NULL, 0);
}

static void check_psk_line(const char *line, bool negative)
{
    uint8_t setup[16], token[16], want[32], wrong[32], got[32];
    long purpose = num(line, "purpose");
    int sl = field(line, "setup", setup, sizeof setup);
    int tl = field(line, "token", token, sizeof token);
    int wl = field(line, "psk", want, sizeof want);
    DSP_CHECK(purpose >= 1 && purpose <= 4 && sl == 16 && (tl == 0 || tl == 16) && wl == 32);
    DSP_CHECK(dsp_psk_derive((uint8_t)purpose, setup, tl == 16 ? token : NULL, got));
    DSP_CHECK(memcmp(got, want, 32) == 0);
    if (negative) {
        DSP_CHECK_EQ_INT(32, field(line, "wrong", wrong, sizeof wrong));
        DSP_CHECK(memcmp(got, wrong, 32) != 0); /* the named mistake is not what we compute */
        n_not++;
    } else {
        n_psk++;
    }
    sodium_memzero(got, sizeof got);
}

static void test_vectors(void)
{
    FILE *f = fopen(DSP_VECTORS_DIR "/ble_session_v1.txt", "r");
    DSP_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "psk ", 4) == 0) {
            check_psk_line(line, false);
        } else if (strncmp(line, "psk_not ", 8) == 0) {
            check_psk_line(line, true);
        } else if (strncmp(line, "prologue ", 9) == 0) {
            uint8_t hash[8], want[64], got[DSP_PROLOGUE_LEN];
            DSP_CHECK_EQ_INT(8, field(line, "hash", hash, sizeof hash));
            DSP_CHECK_EQ_INT(DSP_PROLOGUE_LEN, field(line, "prologue", want, sizeof want));
            dsp_prologue_build((uint8_t)num(line, "major"), (uint8_t)num(line, "minor"),
                               (uint8_t)num(line, "purpose"), hash,
                               (uint32_t)strtoul(strstr(line, " session_id=") + 12, NULL, 10), got);
            DSP_CHECK(memcmp(got, want, sizeof got) == 0);
            n_pro++;
        } else if (strncmp(line, "idhash ", 7) == 0) {
            uint8_t in[32], want[8], got[8];
            bool claimed = strstr(line, " kind=claimed") != NULL;
            int il = field(line, "input", in, sizeof in);
            DSP_CHECK(il == (claimed ? 32 : 6));
            DSP_CHECK_EQ_INT(8, field(line, "hash", want, sizeof want));
            dsp_id_hash(claimed ? in : NULL, in, got);
            DSP_CHECK(memcmp(got, want, 8) == 0);
            n_id++;
        }
    }
    fclose(f);
    /* every record kind is exercised (no silent skip) */
    DSP_CHECK(n_psk >= 24);
    DSP_CHECK(n_not >= 12);
    DSP_CHECK(n_pro >= 24);
    DSP_CHECK(n_id >= 6);
    printf("vectors: psk %d, psk_not %d, prologue %d, idhash %d\n", n_psk, n_not, n_pro, n_id);
}

static void test_failure_paths_clear_output(void)
{
    uint8_t setup[16], psk[32];
    memset(setup, 0x42, sizeof setup);
    memset(psk, 0xEE, sizeof psk);
    DSP_CHECK(!dsp_psk_derive(0, setup, NULL, psk)); /* invalid purpose */
    for (size_t i = 0; i < sizeof psk; i++) {
        DSP_CHECK_EQ_INT(0, psk[i]);
    }
    memset(psk, 0xEE, sizeof psk);
    DSP_CHECK(!dsp_psk_derive(5, setup, NULL, psk));
    DSP_CHECK(psk[0] == 0 && psk[31] == 0);
    memset(psk, 0xEE, sizeof psk);
    DSP_CHECK(!dsp_psk_derive(2, NULL, NULL, psk));
    DSP_CHECK(psk[0] == 0 && psk[31] == 0);
    uint8_t one[1];
    DSP_CHECK(!dsp_hkdf_sha256(NULL, 0, setup, 16, NULL, 0, one, 0)); /* empty output refused */
    DSP_CHECK(!dsp_hkdf_sha256(NULL, 0, setup, 16, NULL, 0, NULL, 32));
}

int main(void)
{
    if (sodium_init() < 0) {
        return 1;
    }
    DSP_RUN(test_vectors);
    DSP_RUN(test_failure_paths_clear_output);
    return dsp_test_finish();
}
