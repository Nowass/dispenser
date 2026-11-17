#include "test_rng.h"

#include <string.h>

#include <sodium.h>

/* SHA-256 counter stream over (seed, counter): deterministic, well distributed, test only. */
static uint64_t g_seed, g_ctr, g_calls;
static uint8_t g_block[32];
static size_t g_avail;

static void refill(void)
{
    uint8_t in[16];
    for (int i = 0; i < 8; i++) {
        in[i] = (uint8_t)(g_seed >> (8 * i));
        in[8 + i] = (uint8_t)(g_ctr >> (8 * i));
    }
    g_ctr++;
    crypto_hash_sha256(g_block, in, sizeof in);
    g_avail = sizeof g_block;
}

static void buf(void *const out, const size_t size)
{
    uint8_t *o = out;
    g_calls++;
    for (size_t i = 0; i < size; i++) {
        if (g_avail == 0) {
            refill();
        }
        o[i] = g_block[sizeof g_block - g_avail--];
    }
}

static uint32_t random32(void)
{
    uint32_t v;
    buf(&v, sizeof v);
    return v;
}

static const char *impl_name(void)
{
    return "dsp_test_deterministic";
}

static void stir(void)
{
}

static int close_impl(void)
{
    return 0;
}

static randombytes_implementation IMPL = {
    .implementation_name = impl_name,
    .random = random32,
    .stir = stir,
    .uniform = NULL,
    .buf = buf,
    .close = close_impl,
};

void test_rng_reseed(uint64_t seed)
{
    g_seed = seed;
    g_ctr = 0;
    g_avail = 0;
}

bool test_rng_install(uint64_t seed)
{
    test_rng_reseed(seed);
    if (randombytes_set_implementation(&IMPL) != 0) {
        return false;
    }
    return sodium_init() >= 0 && strcmp(randombytes_implementation_name(), impl_name()) == 0;
}

bool test_rng_bytes(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    buf(out, len);
    return true;
}

uint64_t test_rng_calls(void)
{
    return g_calls;
}
