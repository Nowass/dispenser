/*
 * Regression test for patch 0001 (noise_pattern_expand bounded by the table size).
 *
 * Checks that the patched lookup keeps the intended semantics:
 *  - every one of the 15 base patterns expands to exactly its token table (flags, tokens up to
 *    and including END, zero fill), compared with an independent sequential reference and with
 *    the token sequences of the Noise specification for XX and IK;
 *  - PSK and fallback modifiers still apply (XXpsk3 against the specification);
 *  - unknown pattern ids and malformed protocol names are rejected without crashing;
 *  - every base pattern is usable through the public name API.
 * Built for the host and the Android NDK (FORTIFY enabled) by run_gate.sh.
 */
#include <noise/protocol.h>
#include "protocol/internal.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c, ...)                                                                     \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(c)) {                                                                       \
            failures++;                                                                   \
            printf("FAIL line %d: ", __LINE__);                                           \
            printf(__VA_ARGS__);                                                          \
            printf("\n");                                                                 \
        }                                                                                 \
    } while (0)

#if NOISE_MAX_TOKENS < 64
#error "test_patterns needs the table build (NOISE_USE_PROTOCOL_NAME_TABLE=1)"
#endif

static const struct {
    int id;
    const char *name;
} BASES[] = {
    {NOISE_PATTERN_N, "N"},   {NOISE_PATTERN_K, "K"},   {NOISE_PATTERN_X, "X"},
    {NOISE_PATTERN_NN, "NN"}, {NOISE_PATTERN_NK, "NK"}, {NOISE_PATTERN_NX, "NX"},
    {NOISE_PATTERN_XN, "XN"}, {NOISE_PATTERN_XK, "XK"}, {NOISE_PATTERN_XX, "XX"},
    {NOISE_PATTERN_KN, "KN"}, {NOISE_PATTERN_KK, "KK"}, {NOISE_PATTERN_KX, "KX"},
    {NOISE_PATTERN_IN, "IN"}, {NOISE_PATTERN_IK, "IK"}, {NOISE_PATTERN_IX, "IX"},
};

#define E NOISE_TOKEN_E
#define S NOISE_TOKEN_S
#define EE NOISE_TOKEN_EE
#define ES NOISE_TOKEN_ES
#define SE NOISE_TOKEN_SE
#define SS NOISE_TOKEN_SS
#define FL NOISE_TOKEN_FLIP_DIR
#define END NOISE_TOKEN_END
#define PSK NOISE_TOKEN_PSK

/* Noise specification, section 7.5: XX and IK token sequences. */
static const uint8_t SPEC_XX[] = {E, FL, E, EE, S, ES, FL, S, SE, END};
static const uint8_t SPEC_IK[] = {E, ES, S, SS, FL, E, EE, SE, END};
/* Section 9.2: psk3 appends "psk" to the third message. */
static const uint8_t SPEC_XXPSK3[] = {E, FL, E, EE, S, ES, FL, S, SE, PSK, END};

/* Independent reference of the original (intended) semantics: copy the table byte by byte up
   to and including END, then zero-fill. Tables are terminated, so this stays in bounds. */
static size_t reference(const uint8_t *table, uint8_t out[NOISE_MAX_TOKENS])
{
    size_t i = 0;
    memset(out, 0, NOISE_MAX_TOKENS);
    out[0] = table[0];
    out[1] = table[1];
    for (i = 2; i < NOISE_MAX_TOKENS; i++) {
        out[i] = table[i];
        if (table[i] == END) {
            return i + 1;
        }
    }
    return 0;
}

static void check_tokens(const char *what, const uint8_t *got, const uint8_t *spec, size_t n)
{
    CHECK(memcmp(got + 2, spec, n) == 0, "%s token sequence differs from the specification", what);
    for (size_t i = 2 + n; i < NOISE_MAX_TOKENS; i++) {
        CHECK(got[i] == 0, "%s not zero-filled at %zu", what, i);
    }
}

int main(void)
{
    uint8_t got[NOISE_MAX_TOKENS];
    uint8_t ref[NOISE_MAX_TOKENS];

    /* 1. Every base pattern: identical to the sequential reference. */
    for (size_t b = 0; b < sizeof BASES / sizeof BASES[0]; b++) {
        const uint8_t *table = noise_pattern_lookup(BASES[b].id);
        CHECK(table != NULL, "lookup %s", BASES[b].name);
        if (table == NULL) {
            continue;
        }
        size_t len = reference(table, ref);
        CHECK(len >= 4 && len <= 16, "%s reference length %zu", BASES[b].name, len);
        memset(got, 0xA5, sizeof got);
        int err = noise_pattern_expand(got, BASES[b].id, NULL, 0);
        CHECK(err == NOISE_ERROR_NONE, "expand %s: 0x%x", BASES[b].name, err);
        CHECK(memcmp(got, ref, NOISE_MAX_TOKENS) == 0, "expand %s differs from the reference",
              BASES[b].name);
    }

    /* 2. Specification sequences. */
    CHECK(noise_pattern_expand(got, NOISE_PATTERN_XX, NULL, 0) == NOISE_ERROR_NONE, "XX");
    check_tokens("XX", got, SPEC_XX, sizeof SPEC_XX);
    CHECK(noise_pattern_expand(got, NOISE_PATTERN_IK, NULL, 0) == NOISE_ERROR_NONE, "IK");
    check_tokens("IK", got, SPEC_IK, sizeof SPEC_IK);
    int psk3 = NOISE_MODIFIER_PSK3;
    CHECK(noise_pattern_expand(got, NOISE_PATTERN_XX, &psk3, 1) == NOISE_ERROR_NONE, "XXpsk3");
    check_tokens("XXpsk3", got, SPEC_XXPSK3, sizeof SPEC_XXPSK3);
    int fallback = NOISE_MODIFIER_FALLBACK;
    CHECK(noise_pattern_expand(got, NOISE_PATTERN_XX, &fallback, 1) == NOISE_ERROR_NONE,
          "XXfallback");

    /* 3. Unknown ids and malformed modifier use: rejected, output not trusted. */
    const int unknown[] = {0, -1, 1, NOISE_ID('P', 0), NOISE_ID('P', 16), NOISE_ID('P', 255),
                           NOISE_MODIFIER_PSK0, 0x7FFFFFFF};
    for (size_t u = 0; u < sizeof unknown / sizeof unknown[0]; u++) {
        CHECK(noise_pattern_lookup(unknown[u]) == NULL, "lookup of unknown id 0x%x", unknown[u]);
        CHECK(noise_pattern_expand(got, unknown[u], NULL, 0) == NOISE_ERROR_UNKNOWN_NAME,
              "expand of unknown id 0x%x", unknown[u]);
    }
    int bad_mod = NOISE_ID('M', 99);
    CHECK(noise_pattern_expand(got, NOISE_PATTERN_XX, &bad_mod, 1) != NOISE_ERROR_NONE,
          "unknown modifier accepted");

    /* 4. Public name API: every base pattern usable; malformed names rejected. */
    for (size_t b = 0; b < sizeof BASES / sizeof BASES[0]; b++) {
        char name[64];
        snprintf(name, sizeof name, "Noise_%s_25519_ChaChaPoly_SHA256", BASES[b].name);
        NoiseHandshakeState *hs = NULL;
        int err = noise_handshakestate_new_by_name(&hs, name, NOISE_ROLE_INITIATOR);
        CHECK(err == NOISE_ERROR_NONE, "%s: 0x%x", name, err);
        noise_handshakestate_free(hs);
    }
    const char *good[] = {"Noise_XXpsk3_25519_ChaChaPoly_SHA256", "Noise_IK_25519_ChaChaPoly_SHA256",
                          "Noise_NNpsk0_25519_ChaChaPoly_SHA256"};
    for (size_t g = 0; g < sizeof good / sizeof good[0]; g++) {
        NoiseHandshakeState *hs = NULL;
        CHECK(noise_handshakestate_new_by_name(&hs, good[g], NOISE_ROLE_RESPONDER) == NOISE_ERROR_NONE,
              "%s", good[g]);
        noise_handshakestate_free(hs);
    }
    const char *bad[] = {"Noise_QQ_25519_ChaChaPoly_SHA256", "Noise_XXpsk9_25519_ChaChaPoly_SHA256",
                         "Noise_XX_25519_ChaChaPoly", "Noise__25519_ChaChaPoly_SHA256", "",
                         "Noise_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX_25519_ChaChaPoly_SHA256"};
    for (size_t x = 0; x < sizeof bad / sizeof bad[0]; x++) {
        NoiseHandshakeState *hs = NULL;
        CHECK(noise_handshakestate_new_by_name(&hs, bad[x], NOISE_ROLE_INITIATOR) != NOISE_ERROR_NONE,
              "malformed name accepted: '%s'", bad[x]);
        noise_handshakestate_free(hs);
    }

    printf("test_patterns: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
