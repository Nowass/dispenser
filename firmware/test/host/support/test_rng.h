#ifndef TEST_RNG_H
#define TEST_RNG_H

/*
 * DETERMINISTIC randomness for host tests ONLY (M1.10c.2a). Registered with libsodium through the
 * same documented interface the firmware uses for the hardware RNG (randombytes_set_implementation
 * before sodium_init), so noise-c's ephemeral keys, the generated identity keys and the
 * Application Service's session IDs and tokens are reproducible in tests. Never built into
 * firmware: dsp_blesec.c refuses DSP_TEST_DETERMINISTIC_RNG on ESP_PLATFORM.
 */
#define DSP_TEST_DETERMINISTIC_RNG 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#error "test_rng is a host-test facility"
#endif

/* Registers the implementation and calls sodium_init(); returns false on failure. */
bool test_rng_install(uint64_t seed);
void test_rng_reseed(uint64_t seed);
bool test_rng_bytes(void *ctx, uint8_t *out, size_t len);
/* Number of randombytes calls served (proves libsodium routes through this implementation). */
uint64_t test_rng_calls(void);

/* Scripted draws (M1.10e handshake transcripts): a request of exactly `len` bytes is served from
 * the oldest queued entry of that length (FIFO per length); other requests come from the stream.
 * Used to make the device draw the vector's session IDs, keys, ephemerals and invite token. */
bool test_rng_script(const uint8_t *bytes, size_t len);
void test_rng_script_clear(void);
/* Queued entries not consumed yet. */
size_t test_rng_script_pending(void);

#endif
