#include "dsp_auth.h"

#include <string.h>

#include "dsp_text.h"

void dsp_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (p != NULL && n-- > 0) {
        *v++ = 0;
    }
}

void dsp_auth_wipe(dsp_auth_t *a)
{
    if (a != NULL) {
        dsp_secure_zero(a, sizeof *a);
    }
}

void dsp_auth_provisioned(dsp_auth_t *a)
{
    if (a != NULL) {
        memset(a, 0, sizeof *a);
    }
}

static bool zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool instant_ok(dsp_utc_t t)
{
    return t == 0 || (t >= DSP_UTC_MIN && t <= DSP_UTC_MAX);
}

bool dsp_auth_name_valid(const char *name, size_t len)
{
    if (len > DSP_AUTH_NAME_MAX || (len > 0 && name == NULL)) {
        return false;
    }
    return dsp_utf8_valid((const uint8_t *)name, len, true);
}

static bool slot_zero(const dsp_auth_slot_t *s)
{
    return !s->used && s->name_len == 0 && zero(s->pub, sizeof s->pub) &&
           zero(s->name, sizeof s->name) && s->enrolled_at == 0 && s->last_seen == 0;
}

static bool slot_valid(const dsp_auth_slot_t *s)
{
    if (!s->used) {
        return slot_zero(s);
    }
    return s->name_len <= DSP_AUTH_NAME_MAX && dsp_auth_name_valid(s->name, s->name_len) &&
           zero(s->name + s->name_len, DSP_AUTH_NAME_MAX - s->name_len) &&
           !zero(s->pub, sizeof s->pub) && instant_ok(s->enrolled_at) && instant_ok(s->last_seen);
}

bool dsp_auth_is_valid(const dsp_auth_t *a)
{
    if (a == NULL || (a->flags & ~DSP_AUTH_F_ALL) != 0 || !instant_ok(a->claimed_at) ||
        !instant_ok(a->recovered_at)) {
        return false;
    }
    for (size_t i = 0; i < DSP_AUTH_SLOTS; i++) {
        if (!slot_valid(&a->slot[i])) {
            return false;
        }
    }
    if (a->claim_state == DSP_AUTH_UNCLAIMED) {
        if (a->flags != 0 || !zero(a->id_priv, sizeof a->id_priv) ||
            !zero(a->id_pub, sizeof a->id_pub) || a->claimed_at != 0) {
            return false;
        }
        for (size_t i = 0; i < DSP_AUTH_SLOTS; i++) {
            if (a->slot[i].used) {
                return false;
            }
        }
        return true;
    }
    if (a->claim_state != DSP_AUTH_CLAIMED || zero(a->id_priv, sizeof a->id_priv) ||
        zero(a->id_pub, sizeof a->id_pub) || a->owner_epoch == 0) {
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < DSP_AUTH_SLOTS; i++) {
        const dsp_auth_slot_t *s = &a->slot[i];
        if (!s->used) {
            continue;
        }
        used++;
        if (memcmp(s->pub, a->id_pub, DSP_AUTH_KEY_BYTES) == 0) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (a->slot[j].used && memcmp(a->slot[j].pub, s->pub, DSP_AUTH_KEY_BYTES) == 0) {
                return false;
            }
        }
    }
    return used > 0;
}

bool dsp_auth_is_provisioning(const dsp_auth_t *a)
{
    dsp_auth_t p;
    dsp_auth_provisioned(&p);
    return a != NULL && dsp_auth_equal(a, &p);
}

static bool slot_equal(const dsp_auth_slot_t *x, const dsp_auth_slot_t *y)
{
    return x->used == y->used && x->name_len == y->name_len &&
           memcmp(x->pub, y->pub, sizeof x->pub) == 0 &&
           memcmp(x->name, y->name, sizeof x->name) == 0 && x->enrolled_at == y->enrolled_at &&
           x->last_seen == y->last_seen;
}

bool dsp_auth_equal(const dsp_auth_t *x, const dsp_auth_t *y)
{
    if (x->claim_state != y->claim_state || x->flags != y->flags ||
        x->owner_epoch != y->owner_epoch || memcmp(x->id_priv, y->id_priv, sizeof x->id_priv) != 0 ||
        memcmp(x->id_pub, y->id_pub, sizeof x->id_pub) != 0 || x->claimed_at != y->claimed_at ||
        x->recovered_at != y->recovered_at || x->recoveries != y->recoveries ||
        x->failed_sessions != y->failed_sessions) {
        return false;
    }
    for (size_t i = 0; i < DSP_AUTH_SLOTS; i++) {
        if (!slot_equal(&x->slot[i], &y->slot[i])) {
            return false;
        }
    }
    return true;
}

bool dsp_auth_usable(const dsp_auth_t *a)
{
    return dsp_auth_is_valid(a) && a->claim_state == DSP_AUTH_CLAIMED &&
           (a->flags & DSP_AUTH_F_KEY_FAULT) == 0;
}

int dsp_auth_find(const dsp_auth_t *a, const uint8_t pub[DSP_AUTH_KEY_BYTES])
{
    if (a == NULL || pub == NULL) {
        return -1;
    }
    for (int i = 0; i < DSP_AUTH_SLOTS; i++) {
        if (a->slot[i].used && memcmp(a->slot[i].pub, pub, DSP_AUTH_KEY_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

int dsp_auth_free_slot(const dsp_auth_t *a)
{
    if (a == NULL) {
        return -1;
    }
    for (int i = 0; i < DSP_AUTH_SLOTS; i++) {
        if (!a->slot[i].used) {
            return i;
        }
    }
    return -1;
}

uint8_t dsp_auth_used_count(const dsp_auth_t *a)
{
    uint8_t n = 0;
    for (size_t i = 0; a != NULL && i < DSP_AUTH_SLOTS; i++) {
        n = (uint8_t)(n + (a->slot[i].used ? 1u : 0u));
    }
    return n;
}

/* A phone key usable for a new slot: non-zero, not the identity key, not enrolled. */
static dsp_auth_result_t check_phone(const dsp_auth_t *a, const uint8_t *phone_pub,
                                     const uint8_t *id_pub, const char *name, size_t name_len)
{
    if (phone_pub == NULL || zero(phone_pub, DSP_AUTH_KEY_BYTES) ||
        !dsp_auth_name_valid(name, name_len)) {
        return DSP_AUTH_ERR_INVALID;
    }
    if (id_pub != NULL && memcmp(phone_pub, id_pub, DSP_AUTH_KEY_BYTES) == 0) {
        return DSP_AUTH_ERR_DUPLICATE_KEY;
    }
    if (a != NULL && dsp_auth_find(a, phone_pub) >= 0) {
        return DSP_AUTH_ERR_DUPLICATE_KEY;
    }
    return DSP_AUTH_OK;
}

static void fill_slot(dsp_auth_slot_t *s, const uint8_t *pub, const char *name, size_t name_len,
                      dsp_utc_t now)
{
    memset(s, 0, sizeof *s);
    s->used = true;
    memcpy(s->pub, pub, DSP_AUTH_KEY_BYTES);
    s->name_len = (uint8_t)name_len;
    if (name_len > 0) {
        memcpy(s->name, name, name_len);
    }
    s->enrolled_at = instant_ok(now) ? now : 0;
}

static bool next_epoch(uint32_t epoch, uint32_t *out)
{
    if (epoch == UINT32_MAX - 1 || epoch == UINT32_MAX) {
        return false; /* UINT32_MAX is reserved as the CRED_DEGRADED arm sentinel */
    }
    *out = epoch + 1;
    return true;
}

static bool key_ok(const uint8_t *k)
{
    return k != NULL && !zero(k, DSP_AUTH_KEY_BYTES);
}

dsp_auth_result_t dsp_auth_claim(dsp_auth_t *a, const uint8_t id_priv[DSP_AUTH_KEY_BYTES],
                                 const uint8_t id_pub[DSP_AUTH_KEY_BYTES],
                                 const uint8_t phone_pub[DSP_AUTH_KEY_BYTES], const char *name,
                                 size_t name_len, dsp_utc_t now_or_0)
{
    if (!dsp_auth_is_valid(a) || !key_ok(id_priv) || !key_ok(id_pub)) {
        return DSP_AUTH_ERR_INVALID;
    }
    if (a->claim_state != DSP_AUTH_UNCLAIMED) {
        return DSP_AUTH_ERR_STATE;
    }
    dsp_auth_result_t r = check_phone(NULL, phone_pub, id_pub, name, name_len);
    if (r != DSP_AUTH_OK) {
        return r;
    }
    uint32_t epoch;
    if (!next_epoch(a->owner_epoch, &epoch)) {
        return DSP_AUTH_ERR_EPOCH;
    }
    dsp_auth_t n = *a;
    n.claim_state = DSP_AUTH_CLAIMED;
    n.owner_epoch = epoch;
    memcpy(n.id_priv, id_priv, DSP_AUTH_KEY_BYTES);
    memcpy(n.id_pub, id_pub, DSP_AUTH_KEY_BYTES);
    n.claimed_at = instant_ok(now_or_0) ? now_or_0 : 0;
    fill_slot(&n.slot[0], phone_pub, name, name_len, now_or_0);
    *a = n;
    dsp_auth_wipe(&n);
    return DSP_AUTH_OK;
}

dsp_auth_result_t dsp_auth_add(dsp_auth_t *a, const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                               const char *name, size_t name_len, dsp_utc_t now_or_0,
                               uint8_t *slot_out)
{
    if (!dsp_auth_is_valid(a)) {
        return DSP_AUTH_ERR_INVALID;
    }
    if (!dsp_auth_usable(a)) {
        return DSP_AUTH_ERR_STATE;
    }
    dsp_auth_result_t r = check_phone(a, phone_pub, a->id_pub, name, name_len);
    if (r != DSP_AUTH_OK) {
        return r;
    }
    int slot = dsp_auth_free_slot(a);
    if (slot < 0) {
        return DSP_AUTH_ERR_SLOTS_FULL;
    }
    uint32_t epoch;
    if (!next_epoch(a->owner_epoch, &epoch)) {
        return DSP_AUTH_ERR_EPOCH;
    }
    a->owner_epoch = epoch;
    fill_slot(&a->slot[slot], phone_pub, name, name_len, now_or_0);
    if (slot_out != NULL) {
        *slot_out = (uint8_t)slot;
    }
    return DSP_AUTH_OK;
}

dsp_auth_result_t dsp_auth_remove(dsp_auth_t *a, uint8_t slot)
{
    if (!dsp_auth_is_valid(a) || slot >= DSP_AUTH_SLOTS) {
        return DSP_AUTH_ERR_INVALID;
    }
    if (!dsp_auth_usable(a)) {
        return DSP_AUTH_ERR_STATE;
    }
    if (!a->slot[slot].used) {
        return DSP_AUTH_ERR_NOT_FOUND;
    }
    if (dsp_auth_used_count(a) <= 1) {
        return DSP_AUTH_ERR_LAST_SLOT;
    }
    uint32_t epoch;
    if (!next_epoch(a->owner_epoch, &epoch)) {
        return DSP_AUTH_ERR_EPOCH;
    }
    a->owner_epoch = epoch;
    memset(&a->slot[slot], 0, sizeof a->slot[slot]);
    return DSP_AUTH_OK;
}

dsp_auth_result_t dsp_auth_mark_key_fault(dsp_auth_t *a)
{
    if (!dsp_auth_is_valid(a)) {
        return DSP_AUTH_ERR_INVALID;
    }
    if (a->claim_state != DSP_AUTH_CLAIMED) {
        return DSP_AUTH_ERR_STATE;
    }
    if (a->flags & DSP_AUTH_F_KEY_FAULT) {
        return DSP_AUTH_OK; /* already recorded: no write needed (caller compares) */
    }
    uint32_t epoch;
    if (!next_epoch(a->owner_epoch, &epoch)) {
        return DSP_AUTH_ERR_EPOCH;
    }
    a->owner_epoch = epoch;
    a->flags |= DSP_AUTH_F_KEY_FAULT;
    return DSP_AUTH_OK;
}

dsp_auth_result_t dsp_auth_recover(const dsp_auth_t *prev, uint32_t min_epoch,
                                   const uint8_t id_priv[DSP_AUTH_KEY_BYTES],
                                   const uint8_t id_pub[DSP_AUTH_KEY_BYTES],
                                   const uint8_t phone_pub[DSP_AUTH_KEY_BYTES], const char *name,
                                   size_t name_len, dsp_utc_t now_or_0, dsp_auth_t *out)
{
    if (out == NULL || !key_ok(id_priv) || !key_ok(id_pub) ||
        (prev != NULL && !dsp_auth_is_valid(prev))) {
        return DSP_AUTH_ERR_INVALID;
    }
    dsp_auth_result_t r = check_phone(NULL, phone_pub, id_pub, name, name_len);
    if (r != DSP_AUTH_OK) {
        return r;
    }
    uint32_t base = prev != NULL ? prev->owner_epoch : 0;
    uint32_t epoch;
    if (!next_epoch(base, &epoch)) {
        return DSP_AUTH_ERR_EPOCH;
    }
    if (min_epoch > epoch) {
        if (min_epoch >= UINT32_MAX - 1) {
            return DSP_AUTH_ERR_EPOCH;
        }
        epoch = min_epoch;
    }
    dsp_auth_t n;
    dsp_auth_provisioned(&n);
    if (prev != NULL) {
        n.recoveries = prev->recoveries;
        n.failed_sessions = prev->failed_sessions;
    }
    n.claim_state = DSP_AUTH_CLAIMED;
    n.owner_epoch = epoch;
    memcpy(n.id_priv, id_priv, DSP_AUTH_KEY_BYTES);
    memcpy(n.id_pub, id_pub, DSP_AUTH_KEY_BYTES);
    n.claimed_at = instant_ok(now_or_0) ? now_or_0 : 0;
    n.recovered_at = n.claimed_at;
    if (n.recoveries < UINT32_MAX) {
        n.recoveries++;
    }
    fill_slot(&n.slot[0], phone_pub, name, name_len, now_or_0);
    *out = n;
    dsp_auth_wipe(&n);
    return DSP_AUTH_OK;
}

bool dsp_auth_touch(dsp_auth_t *a, uint8_t slot, dsp_utc_t now_or_0)
{
    if (a == NULL || slot >= DSP_AUTH_SLOTS || !a->slot[slot].used || now_or_0 < DSP_UTC_MIN ||
        now_or_0 > DSP_UTC_MAX) {
        return false;
    }
    dsp_utc_t last = a->slot[slot].last_seen;
    if (last != 0 && now_or_0 - last < DSP_AUTH_LAST_SEEN_MIN_S) {
        return false; /* includes a clock that moved back */
    }
    a->slot[slot].last_seen = now_or_0;
    return true;
}

bool dsp_auth_count_failed_session(dsp_auth_t *a)
{
    if (a == NULL || a->failed_sessions == UINT32_MAX) {
        return false;
    }
    a->failed_sessions++;
    return true;
}
