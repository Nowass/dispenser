#ifndef DSP_ESP_RNG_H
#define DSP_ESP_RNG_H

/*
 * ESP32-C3 RNG adapter for libsodium (M1.10c.2a, decision C2; docs/SECURITY_INTEGRATION_DESIGN.md
 * section 6). Uses only the documented libsodium interface (randombytes_set_implementation) and
 * esp_fill_random(). No primitive of our own, no deterministic mode (test randomness exists only in
 * the host tests).
 *
 * Fail-closed rules:
 *  - The implementation is registered (dsp_esp_rng_register) before libsodium is used at all.
 *  - esp_fill_random() is a true RNG only while the RF subsystem runs; the hardware entropy source
 *    counts as active only while the Bluetooth controller is enabled. Without CONFIG_BT_ENABLED in
 *    the build there is no such source and nothing is ever considered ready.
 *  - sodium_init() draws randomness (allocation canary), so it runs only at the first moment the
 *    source is active (dsp_esp_rng_ready), never at boot with RF off.
 *  - The libsodium callbacks abort() if they are ever reached while the source is not active
 *    (unreachable by construction: every consumer asks dsp_esp_rng_ready first). A restart
 *    generates no key and persists nothing; returning bytes from an inactive source would.
 *
 * Whether the target RNG path actually behaves this way is a HARDWARE acceptance item (M1.10c.2b,
 * design 6.1 R1-R4); compilation and host tests do not demonstrate it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the implementation (no randomness drawn). false: libsodium refused it. Call once at
 * runtime start, before any other libsodium or noise-c use. */
bool dsp_esp_rng_register(void);

/* true only if registered, the hardware entropy source is active now, and sodium_init() has
 * succeeded (performed here on the first ready call). */
bool dsp_esp_rng_ready(void *ctx);

/* Random bytes for the Application Service (session IDs, tokens, nonces); false unless ready. */
bool dsp_esp_rng_bytes(void *ctx, uint8_t *out, size_t len);

/* Acceptance-test evidence (6.1 R1/R2): the registered name, whether registration preceded
 * sodium_init, and how many libsodium requests the adapter served. */
const char *dsp_esp_rng_name(void);
bool dsp_esp_rng_registered_before_init(void);
uint32_t dsp_esp_rng_calls(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_RNG_H */
