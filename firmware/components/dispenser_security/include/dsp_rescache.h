#ifndef DSP_RESCACHE_H
#define DSP_RESCACHE_H

/*
 * Transport response cache (decision C4, docs/SECURITY_INTEGRATION_DESIGN.md 8.1): the last eight
 * responses of ONE authenticated connection. A duplicate request (same txn, type and plaintext)
 * is answered with the stored response, re-encrypted with the next nonce, without executing the
 * operation again; the same txn with different content is rejected (MALFORMED, not executed).
 *
 * Every entry is bound to the connection (session ID + transport generation), the ownership epoch
 * and the phone slot of the issued authorization at execution time. The cache is cleared (and
 * zeroized: an entry may hold an invite token) at disconnect, session end, BLE suspension and
 * reboot (RAM only); on an epoch change every older entry is purged. It is NOT exactly-once across
 * reconnections: there the durable mechanisms decide (revisions, invite single use, and the
 * durable manual-dose anti-replay, which stays authoritative).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_bleproto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_RESCACHE_ENTRIES 8
#define DSP_RESCACHE_DIGEST 32

typedef struct {
    uint32_t session_id;
    uint32_t conn_gen;
    uint32_t epoch;
    uint8_t slot;
} dsp_rescache_bind_t;

typedef struct {
    dsp_rescache_bind_t bind;
    uint8_t used;
    uint8_t type;      /* request type (= response type) */
    uint16_t txn;
    uint8_t digest[DSP_RESCACHE_DIGEST]; /* SHA-256(type || txn || plaintext request body) */
    uint8_t rsp_flags; /* RESPONSE or RESPONSE|ERROR (ENCRYPTED added when sent) */
    uint16_t rsp_len;  /* plaintext response body length */
    uint8_t rsp[DSP_BLE_PLAIN_BODY_MAX];
} dsp_rescache_entry_t;

typedef struct {
    dsp_rescache_entry_t e[DSP_RESCACHE_ENTRIES];
    uint8_t next; /* oldest-first eviction cursor */
} dsp_rescache_t;

typedef enum {
    DSP_RESCACHE_MISS = 0,
    DSP_RESCACHE_HIT,      /* *hit: the original response */
    DSP_RESCACHE_CONFLICT, /* same txn, different type or content: reject, do not execute */
} dsp_rescache_result_t;

void dsp_rescache_clear(dsp_rescache_t *c); /* zeroizes every entry */
/* Purges (zeroizes) every entry whose epoch differs from current_epoch. */
void dsp_rescache_purge_epoch(dsp_rescache_t *c, uint32_t current_epoch);
dsp_rescache_result_t dsp_rescache_lookup(const dsp_rescache_t *c, const dsp_rescache_bind_t *b,
                                          uint8_t type, uint16_t txn, const uint8_t *body,
                                          size_t body_len, const dsp_rescache_entry_t **hit);
void dsp_rescache_store(dsp_rescache_t *c, const dsp_rescache_bind_t *b, uint8_t type, uint16_t txn,
                        const uint8_t *body, size_t body_len, uint8_t rsp_flags,
                        const uint8_t *rsp, size_t rsp_len);

#ifdef __cplusplus
}
#endif

#endif /* DSP_RESCACHE_H */
