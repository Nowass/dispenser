#ifndef DSP_EVLOG_H
#define DSP_EVLOG_H

/*
 * User-visible event history (Phase C, 2026-11-20): a bounded, persistent, chronological log of
 * dose outcomes and reservoir changes, read by the phone with GET_HISTORY (docs/BLE_PROTOCOL.md
 * 10.7). Pure domain code over a dsp_kv_t; no allocation, no hidden state.
 *
 * SEPARATE FROM THE SAFETY RECORDS. The anti-replay history (dsp_history, keys "h00".."h31"), the
 * dosing record, the configuration and every other layout key are never read, written or
 * repurposed here. The log is informational only:
 *  - nothing in the dosing path reads it, so its content or its failure can never authorize a
 *    dose, close or open a gate;
 *  - every event is appended only AFTER the durable state transition it describes (claim
 *    finished, marker cleared, refill completed, configuration saved). A storage failure of the
 *    log loses that event (counted in `faults`), never the transition. A power cut between the
 *    transition and the append loses the event (documented window), it never duplicates it:
 *    the transition is already durable, so recovery does not see it again;
 *  - events carry an optional deduplication key (non-zero): an append whose key is already in
 *    the log is a no-op (defense in depth against repeated recovery or retries).
 *
 * Layout: DSP_EVLOG_SLOTS keys "ev00".."ev63" in the same NVS namespace, OUTSIDE the storage
 * layout's explicit-presence rule: a missing key is an empty slot (never provisioned, never
 * required), so adding the log needs no layout migration and older firmware ignores the keys.
 * Event `seq` (u32, from 1, strictly increasing, gaps allowed) lives in slot seq % SLOTS; one
 * append = one key write (single-key atomic in NVS: old or new event, never a mix), commit,
 * read-back compare. No separate head key: the head is the highest valid seq found at open.
 * Wraparound overwrites the oldest event. A slot that cannot be read or decoded, or whose seq
 * does not belong to it, is ignored and counted in `faults`; after any such fault at open the
 * next seq skips one full ring (head + SLOTS) so a seq possibly still held by an unreadable slot
 * is never reused (the skipped range is a gap; ordering is by seq only). (A newest record that NVS dropped as corrupt before the first wraparound is
 * indistinguishable from an empty slot; its seq can then be reused once: documented limit.)
 *
 * Capacity: an event is 41 + id bytes (<= 105 B) -> at most 6 NVS entries; 64 slots <= 384 of the
 * partition's ~1,950 free entries (firmware/README.md "Event history (Phase C)").
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_kv.h"
#include "dsp_recurrence.h"
#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_EVLOG_SLOTS 64
#define DSP_EVLOG_FORMAT 1
#define DSP_EVLOG_HEADER_BYTES 41
#define DSP_EVLOG_RECORD_MAX (DSP_EVLOG_HEADER_BYTES + DSP_SCHEDULE_ID_MAX_LEN)

/* Event types (u8 on flash and on the wire). */
enum {
    DSP_EV_DOSE_SCHEDULED = 1,  /* automatic dose of a schedule (schedule_id set) */
    DSP_EV_DOSE_MANUAL = 2,     /* manual dose requested by a phone */
    DSP_EV_RESERVOIR_INIT = 3,  /* reservoir configured while it was unconfigured (capacity 0) */
    DSP_EV_RESERVOIR_REFILL = 4,/* refill: same capacity, more remaining (or before unknown) */
    DSP_EV_RESERVOIR_ADJUST = 5,/* capacity changed, or remaining set without increasing it */
};
#define DSP_EV_TYPE_MAX 5

/* Results. Reservoir events are always OK. */
enum {
    DSP_EVR_OK = 0,           /* dose: the pump port ran the calculated runtime (no flow sensing) */
    DSP_EVR_PUMP_FAILED = 1,  /* the pump port reported a fault during the run */
    DSP_EVR_START_FAILED = 2, /* the pump port could not start */
    DSP_EVR_INTERRUPTED = 3,  /* reset or power loss after the claim: outcome unknown */
    DSP_EVR_ABORTED = 4,      /* a storage failure after the claim: the pump never ran */
};
#define DSP_EVR_MAX 4

/* Flags. */
enum {
    DSP_EVF_CLOCK_TRUSTED = 1u << 0,     /* `at` comes from a trusted clock */
    DSP_EVF_RESERVOIR_UNKNOWN = 1u << 1, /* before/after are not known (0) */
    DSP_EVF_PUMP_SIMULATED = 1u << 2,    /* firmware with the simulated pump: nothing actuated */
    DSP_EVF_PUMP_BENCH_GPIO = 1u << 3,   /* bench GPIO control signal only: no liquid verified */
};
#define DSP_EVF_ALL 0x0Fu

typedef struct {
    uint32_t seq;          /* assigned by dsp_evlog_append (input ignored) */
    uint8_t type;          /* DSP_EV_* */
    uint8_t result;        /* DSP_EVR_* */
    uint8_t flags;         /* DSP_EVF_* */
    dsp_utc_t at;          /* dose start / change time; 0 = unknown */
    uint32_t volume_ul;    /* requested (nominal) dose volume; 0 for reservoir events */
    uint32_t before_ul;    /* estimated remaining before (see DSP_EVF_RESERVOIR_UNKNOWN) */
    uint32_t after_ul;     /* estimated remaining after */
    uint32_t capacity_ul;  /* capacity after */
    uint64_t key;          /* deduplication key, 0 = none (stored, never sent over BLE) */
    char schedule_id[DSP_SCHEDULE_ID_MAX_LEN + 1]; /* DOSE_SCHEDULED only, else "" */
} dsp_event_t;

typedef enum {
    DSP_EVLOG_OK = 0,
    DSP_EVLOG_DUPLICATE,   /* key already logged: nothing written */
    DSP_EVLOG_ERR_ARG,     /* NULL, invalid event, not open */
    DSP_EVLOG_ERR_WRITE,   /* set / commit / read-back failed: the event may be lost */
} dsp_evlog_status_t;

/* Caller-owned state (about 1.0 KB). Single owner, not thread-safe. */
typedef struct {
    const dsp_kv_t *kv;
    bool open;
    uint32_t head;                    /* highest seq that may exist (0 = none) */
    uint32_t seq[DSP_EVLOG_SLOTS];    /* index: seq held by each slot, 0 = empty / unreadable */
    uint64_t key[DSP_EVLOG_SLOTS];
    uint16_t faults;                  /* unreadable slots at open + failed appends/reads since this
                                         open (RAM only, i.e. this boot/wake; saturating) */
    uint8_t buf[DSP_EVLOG_RECORD_MAX];
    uint8_t rb[DSP_EVLOG_RECORD_MAX];
} dsp_evlog_t;

/* "ev00".."ev63"; buf must hold 5 bytes. */
bool dsp_evlog_key(size_t slot, char *buf, size_t cap);

/* Field validity (type/result/flag ranges, id rules, instant range). Ignores seq. */
bool dsp_event_is_valid(const dsp_event_t *ev);

/* Strict codec of the flash record (format 1, little-endian). encode: 0 if invalid or cap is too
 * small, else the length. decode: false for any malformed, unknown-format or trailing input. */
size_t dsp_event_encode(const dsp_event_t *ev, uint8_t *buf, size_t cap);
bool dsp_event_decode(const uint8_t *buf, size_t len, dsp_event_t *out);

/* Reads every slot once and builds the index. Never writes, never fails on content (faulty slots
 * are counted and ignored); false only for invalid arguments. */
bool dsp_evlog_open(dsp_evlog_t *log, const dsp_kv_t *kv);

/* Appends ev (seq assigned, written to *seq_out if non-NULL). */
dsp_evlog_status_t dsp_evlog_append(dsp_evlog_t *log, const dsp_event_t *ev, uint32_t *seq_out);

/* Newest-first page: up to max events with seq < before_seq (before_seq 0 = from the newest).
 * Slots that fail to read back are skipped and counted. Returns the number written to out. */
size_t dsp_evlog_read(dsp_evlog_t *log, uint32_t before_seq, dsp_event_t *out, size_t max);

/* Highest / lowest seq currently held (0 = empty log). */
uint32_t dsp_evlog_newest(const dsp_evlog_t *log);
uint32_t dsp_evlog_oldest(const dsp_evlog_t *log);

#ifdef __cplusplus
}
#endif

#endif /* DSP_EVLOG_H */
