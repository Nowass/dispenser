/*
 * noise-c's randomness hook (NOISE_USE_CUSTOM_RAND, the library default in every build we use):
 * all Noise randomness goes through libsodium's randombytes_buf(), i.e. through the ONE
 * implementation registered with randombytes_set_implementation() before sodium_init():
 *   - ESP32-C3: the hardware adapter (dispenser_platform dsp_esp_rng.c) over esp_fill_random,
 *     which refuses (abort) when no hardware entropy source is active (decision C2);
 *   - host tests: the deterministic test implementation (test/host/support/test_rng.c).
 * No primitive and no fallback source here.
 */
#include <stddef.h>

#include <sodium.h>

void noise_rand_bytes(void *bytes, size_t size); /* prototype: noise-c src/protocol/internal.h */

void noise_rand_bytes(void *bytes, size_t size)
{
    randombytes_buf(bytes, size);
}
