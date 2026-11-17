#include "dsp_rescache.h"

#include <string.h>

#include <sodium.h>

static void digest(uint8_t type, uint16_t txn, const uint8_t *body, size_t len,
                   uint8_t out[DSP_RESCACHE_DIGEST])
{
    crypto_hash_sha256_state st;
    const uint8_t head[3] = {type, (uint8_t)txn, (uint8_t)(txn >> 8)};
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, head, sizeof head);
    if (len > 0) {
        crypto_hash_sha256_update(&st, body, len);
    }
    crypto_hash_sha256_final(&st, out);
    sodium_memzero(&st, sizeof st);
}

static bool same_bind(const dsp_rescache_bind_t *a, const dsp_rescache_bind_t *b)
{
    return a->session_id == b->session_id && a->conn_gen == b->conn_gen && a->epoch == b->epoch &&
           a->slot == b->slot;
}

void dsp_rescache_clear(dsp_rescache_t *c)
{
    if (c != NULL) {
        sodium_memzero(c, sizeof *c);
    }
}

void dsp_rescache_purge_epoch(dsp_rescache_t *c, uint32_t current_epoch)
{
    if (c == NULL) {
        return;
    }
    for (int i = 0; i < DSP_RESCACHE_ENTRIES; i++) {
        if (c->e[i].used && c->e[i].bind.epoch != current_epoch) {
            sodium_memzero(&c->e[i], sizeof c->e[i]);
        }
    }
}

dsp_rescache_result_t dsp_rescache_lookup(const dsp_rescache_t *c, const dsp_rescache_bind_t *b,
                                          uint8_t type, uint16_t txn, const uint8_t *body,
                                          size_t body_len, const dsp_rescache_entry_t **hit)
{
    if (hit != NULL) {
        *hit = NULL;
    }
    if (c == NULL || b == NULL || (body_len > 0 && body == NULL)) {
        return DSP_RESCACHE_MISS;
    }
    for (int i = 0; i < DSP_RESCACHE_ENTRIES; i++) {
        const dsp_rescache_entry_t *e = &c->e[i];
        if (!e->used || e->txn != txn || !same_bind(&e->bind, b)) {
            continue;
        }
        uint8_t d[DSP_RESCACHE_DIGEST];
        digest(type, txn, body, body_len, d);
        bool same = e->type == type && sodium_memcmp(d, e->digest, sizeof d) == 0;
        sodium_memzero(d, sizeof d);
        if (!same) {
            return DSP_RESCACHE_CONFLICT;
        }
        if (hit != NULL) {
            *hit = e;
        }
        return DSP_RESCACHE_HIT;
    }
    return DSP_RESCACHE_MISS;
}

void dsp_rescache_store(dsp_rescache_t *c, const dsp_rescache_bind_t *b, uint8_t type, uint16_t txn,
                        const uint8_t *body, size_t body_len, uint8_t rsp_flags,
                        const uint8_t *rsp, size_t rsp_len)
{
    if (c == NULL || b == NULL || rsp_len > DSP_BLE_PLAIN_BODY_MAX || (rsp_len > 0 && rsp == NULL) ||
        (body_len > 0 && body == NULL)) {
        return;
    }
    /* an older entry with the same txn of this connection is replaced (txn reuse after eviction
     * window is a client matter; never two entries for one txn) */
    int slot = -1;
    for (int i = 0; i < DSP_RESCACHE_ENTRIES; i++) {
        if (c->e[i].used && c->e[i].txn == txn && same_bind(&c->e[i].bind, b)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = c->next;
        c->next = (uint8_t)((c->next + 1u) % DSP_RESCACHE_ENTRIES);
    }
    dsp_rescache_entry_t *e = &c->e[slot];
    sodium_memzero(e, sizeof *e);
    e->bind = *b;
    e->used = 1;
    e->type = type;
    e->txn = txn;
    digest(type, txn, body, body_len, e->digest);
    e->rsp_flags = rsp_flags;
    e->rsp_len = (uint16_t)rsp_len;
    if (rsp_len > 0) {
        memcpy(e->rsp, rsp, rsp_len);
    }
}
