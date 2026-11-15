#ifndef FAKE_STORE_H
#define FAKE_STORE_H

/*
 * In-memory implementation of dsp_history_store_t for host tests.
 *
 * Models the storage contract of dsp_history.h: write() stages one slot, commit() makes
 * staged slots durable, read() returns the durable value. fake_store_power_cycle() drops
 * everything not yet committed, which is what survives a reset or power loss. The
 * struct outlives coordinator instances, so re-initializing a coordinator on the same
 * fake models a restart with persistent flash.
 */

#include <stdbool.h>
#include <stddef.h>

#include "dsp_history.h"

#define FAKE_STORE_MAX_SLOTS 64

typedef struct {
    bool used;
    dsp_history_record_t rec;
} fake_slot_t;

typedef struct {
    size_t slot_count;
    fake_slot_t durable[FAKE_STORE_MAX_SLOTS];
    fake_slot_t staged[FAKE_STORE_MAX_SLOTS];
    bool dirty[FAKE_STORE_MAX_SLOTS];

    /* Fault injection (each flag stays active until cleared by the test). */
    bool fail_reads;            /* every read returns DSP_STORE_ERR_READ */
    int fail_read_after;        /* N>0: the N-th read from now fails, and every read after it */
    bool fail_writes;           /* write returns DSP_STORE_ERR_WRITE, nothing staged */
    bool fail_commits;          /* commit fails, staged data dropped (old value kept) */
    bool commit_fails_but_persists; /* commit reports failure, data is durable anyway */
    bool power_loss_at_commit;  /* power cut during commit: staged lost, reports failure */
    bool corrupt_next_readback; /* the first read after the next commit returns altered data */
    bool corrupt_armed;         /* internal: set by commit when corrupt_next_readback */

    /* Counters (I/O actually performed). */
    int reads;
    int writes;
    int commits;

    dsp_history_store_t iface;
} fake_store_t;

void fake_store_init(fake_store_t *f, size_t slot_count);

/* Reset/power loss: uncommitted writes are lost; durable slots are kept. */
void fake_store_power_cycle(fake_store_t *f);

/* Clears all fault flags and counters (durable content is kept). */
void fake_store_clear_faults(fake_store_t *f);

#endif /* FAKE_STORE_H */
