#include "fake_kv.h"

#include <string.h>

static fake_kv_entry_t *find_in(fake_kv_entry_t *tab, const char *key)
{
    for (size_t i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        if (tab[i].used && strcmp(tab[i].key, key) == 0) {
            return &tab[i];
        }
    }
    return NULL;
}

static fake_kv_entry_t *slot_in(fake_kv_entry_t *tab, const char *key)
{
    fake_kv_entry_t *e = find_in(tab, key);
    if (e != NULL) {
        return e;
    }
    for (size_t i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        if (!tab[i].used) {
            return &tab[i];
        }
    }
    return NULL;
}

static void store_in(fake_kv_entry_t *tab, const char *key, const uint8_t *data, size_t len)
{
    fake_kv_entry_t *e = slot_in(tab, key);
    if (e == NULL) {
        return;
    }
    e->used = true;
    strncpy(e->key, key, DSP_KV_KEY_MAX_LEN);
    e->key[DSP_KV_KEY_MAX_LEN] = '\0';
    e->len = len;
    memcpy(e->value, data, len);
}

static bool key_ok(const char *key)
{
    if (key == NULL) {
        return false;
    }
    size_t n = strlen(key);
    return n >= 1 && n <= DSP_KV_KEY_MAX_LEN;
}

static dsp_kv_status_t kv_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    fake_kv_t *f = ctx;
    if (!key_ok(key) || buf == NULL || len == NULL) {
        return DSP_KV_ERR_ARG;
    }
    *len = 0;
    if (f->powered_off || f->fail_gets ||
        (f->fail_get_key[0] != '\0' && strcmp(f->fail_get_key, key) == 0)) {
        return DSP_KV_ERR_READ;
    }
    f->gets++;
    /* Reads see staged values too (same device, before a power cycle). */
    const fake_kv_entry_t *e = find_in(f->staged, key);
    if (e == NULL) {
        e = find_in(f->durable, key);
    }
    if (e == NULL) {
        return DSP_KV_NOT_FOUND;
    }
    if (e->len > cap) {
        return DSP_KV_ERR_TOO_LARGE;
    }
    memcpy(buf, e->value, e->len);
    *len = e->len;
    if (f->corrupt_armed && e->len > 0) {
        f->corrupt_armed = false;
        buf[e->len - 1] ^= 0x01;
    }
    return DSP_KV_OK;
}

static dsp_kv_status_t kv_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    fake_kv_t *f = ctx;
    if (!key_ok(key) || data == NULL || len == 0 || len > FAKE_KV_MAX_VALUE) {
        return DSP_KV_ERR_ARG;
    }
    if (f->powered_off || f->fail_sets) {
        return DSP_KV_ERR_WRITE;
    }
    if (f->no_space) {
        return DSP_KV_ERR_NO_SPACE;
    }
    f->sets++;
    store_in(f->durable_on_set ? f->durable : f->staged, key, data, len);
    if (f->power_cut_after_sets > 0 && --f->power_cut_after_sets == 0) {
        f->powered_off = true; /* this set completed; nothing after it does */
    }
    return DSP_KV_OK;
}

static dsp_kv_status_t kv_commit(void *ctx)
{
    fake_kv_t *f = ctx;
    if (f->powered_off) {
        return DSP_KV_ERR_COMMIT;
    }
    f->commits++;
    if (f->fail_commits) {
        if (!f->durable_on_set) {
            memset(f->staged, 0, sizeof f->staged); /* staged data lost */
        }
        return DSP_KV_ERR_COMMIT;
    }
    for (size_t i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        if (f->staged[i].used) {
            store_in(f->durable, f->staged[i].key, f->staged[i].value, f->staged[i].len);
        }
    }
    memset(f->staged, 0, sizeof f->staged);
    if (f->corrupt_next_readback) {
        f->corrupt_next_readback = false;
        f->corrupt_armed = true;
    }
    return DSP_KV_OK;
}

void fake_kv_init(fake_kv_t *f, bool durable_on_set)
{
    memset(f, 0, sizeof *f);
    f->durable_on_set = durable_on_set;
    f->iface.ctx = f;
    f->iface.get = kv_get;
    f->iface.set = kv_set;
    f->iface.commit = kv_commit;
}

void fake_kv_power_cycle(fake_kv_t *f)
{
    memset(f->staged, 0, sizeof f->staged);
    f->powered_off = false;
    f->power_cut_after_sets = 0;
    f->corrupt_armed = false;
}

void fake_kv_clear_faults(fake_kv_t *f)
{
    f->fail_gets = false;
    f->fail_get_key[0] = '\0';
    f->fail_sets = false;
    f->no_space = false;
    f->fail_commits = false;
    f->power_cut_after_sets = 0;
    f->powered_off = false;
    f->corrupt_next_readback = false;
    f->corrupt_armed = false;
    f->gets = 0;
    f->sets = 0;
    f->commits = 0;
}

void fake_kv_remove(fake_kv_t *f, const char *key)
{
    fake_kv_entry_t *e = find_in(f->durable, key);
    if (e != NULL) {
        memset(e, 0, sizeof *e);
    }
}

void fake_kv_put_raw(fake_kv_t *f, const char *key, const uint8_t *data, size_t len)
{
    store_in(f->durable, key, data, len);
}

const fake_kv_entry_t *fake_kv_find(const fake_kv_t *f, const char *key)
{
    return find_in((fake_kv_entry_t *)f->durable, key);
}

int fake_kv_count(const fake_kv_t *f)
{
    int n = 0;
    for (size_t i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        n += f->durable[i].used;
    }
    return n;
}
