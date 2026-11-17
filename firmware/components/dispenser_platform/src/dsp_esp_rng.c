/* ESP32-C3 RNG adapter for libsodium (see dsp_esp_rng.h). ESP-IDF only. */
#include "dsp_esp_rng.h"

#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "esp_random.h"
#include "sdkconfig.h"
#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#endif

#if defined(DSP_TEST_DETERMINISTIC_RNG)
#error "deterministic randomness is a host-test facility; it must never be built into firmware"
#endif

static bool s_registered;
static bool s_sodium_ready;
static bool s_order_ok; /* registration happened before sodium_init */
static uint32_t s_calls;

static bool source_active(void)
{
#if CONFIG_BT_ENABLED
    return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
#else
    return false; /* no RF entropy source in this build: never ready (fail closed) */
#endif
}

static void rng_buf(void *const buf, const size_t size)
{
    if (!s_registered || !source_active()) {
        abort(); /* unreachable by construction; never return bytes from an inactive source */
    }
    esp_fill_random(buf, size);
    s_calls++;
}

static uint32_t rng_random(void)
{
    uint32_t v;
    rng_buf(&v, sizeof v);
    return v;
}

static const char *rng_name(void)
{
    return "dsp_esp32c3_hw";
}

static void rng_stir(void)
{
}

static int rng_close(void)
{
    return 0;
}

static randombytes_implementation s_impl = {
    .implementation_name = rng_name,
    .random = rng_random,
    .stir = rng_stir,
    .uniform = NULL, /* libsodium's own rejection sampling over random() */
    .buf = rng_buf,
    .close = rng_close,
};

bool dsp_esp_rng_register(void)
{
    if (s_registered) {
        return true;
    }
    if (s_sodium_ready) {
        return false; /* sodium_init must never precede the registration */
    }
    s_registered = randombytes_set_implementation(&s_impl) == 0;
    return s_registered;
}

bool dsp_esp_rng_ready(void *ctx)
{
    (void)ctx;
    if (!s_registered || !source_active()) {
        return false;
    }
    if (!s_sodium_ready) {
        s_order_ok = true; /* registered (above) before this first sodium_init */
        s_sodium_ready = sodium_init() >= 0 && strcmp(randombytes_implementation_name(), rng_name()) == 0;
    }
    return s_sodium_ready;
}

bool dsp_esp_rng_bytes(void *ctx, uint8_t *out, size_t len)
{
    if (out == NULL || !dsp_esp_rng_ready(ctx)) {
        return false;
    }
    esp_fill_random(out, len);
    return true;
}

const char *dsp_esp_rng_name(void)
{
    return s_sodium_ready ? randombytes_implementation_name() : "";
}

bool dsp_esp_rng_registered_before_init(void)
{
    return s_order_ok;
}

uint32_t dsp_esp_rng_calls(void)
{
    return s_calls;
}
