#ifndef FAKE_KV_H
#define FAKE_KV_H

/*
 * In-memory dsp_kv_t for host tests (M1.3).
 *
 * Two durability modes:
 *  - durable_on_set = true (default, like ESP-IDF v6.1 NVS): set() is durable at once.
 *  - durable_on_set = false (stricter contract): set() is staged; commit() makes it
 *    durable; a power cycle drops staged values.
 * Per-key replacement is atomic in both modes (old or new value, never a mix).
 * The struct outlives the code under test, so re-opening storage on the same fake models
 * a restart with persistent flash.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_codec.h"
#include "dsp_kv.h"

#define FAKE_KV_MAX_KEYS 112 /* layout keys (39) + event log (64) + spare */
#define FAKE_KV_MAX_VALUE (DSP_RECORD_MAX_BYTES + 64)

typedef struct {
    bool used;
    char key[DSP_KV_KEY_MAX_LEN + 1];
    size_t len;
    uint8_t value[FAKE_KV_MAX_VALUE];
} fake_kv_entry_t;

typedef struct {
    bool durable_on_set;
    fake_kv_entry_t durable[FAKE_KV_MAX_KEYS];
    fake_kv_entry_t staged[FAKE_KV_MAX_KEYS];

    /* Fault injection (cleared by fake_kv_clear_faults). */
    bool fail_gets;
    char fail_get_key[DSP_KV_KEY_MAX_LEN + 1]; /* fail gets of this key only */
    bool fail_sets;
    bool no_space;
    bool fail_commits;
    int power_cut_after_sets; /* >0: after N more successful sets the "power" is cut:
                                 every later operation fails until fake_kv_power_cycle */
    bool powered_off;
    bool corrupt_next_readback; /* the first get after the next successful commit flips a
                                   byte of the returned value */
    bool corrupt_armed;

    int gets;
    int sets;
    int commits;

    dsp_kv_t iface;
} fake_kv_t;

void fake_kv_init(fake_kv_t *f, bool durable_on_set);
void fake_kv_power_cycle(fake_kv_t *f); /* drops staged values, power back on */
void fake_kv_clear_faults(fake_kv_t *f);

/* Direct manipulation of durable content (corruption scenarios). */
void fake_kv_remove(fake_kv_t *f, const char *key); /* as NVS does with a corrupt item */
void fake_kv_put_raw(fake_kv_t *f, const char *key, const uint8_t *data, size_t len);
const fake_kv_entry_t *fake_kv_find(const fake_kv_t *f, const char *key);
int fake_kv_count(const fake_kv_t *f);

#endif /* FAKE_KV_H */
