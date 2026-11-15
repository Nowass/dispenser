#include "dsp_storage.h"

#include <stdio.h>
#include <string.h>

#define KEY_LAYOUT "layout"
#define KEY_SCHED "sched"
#define KEY_CONFIG "config"

static const dsp_layout_t LAYOUT_V1 = {
    .layout_version = DSP_STORAGE_LAYOUT_VERSION,
    .history_slots = DSP_STORAGE_HISTORY_SLOTS,
    .schedule_capacity = DSP_SCHEDULE_CAPACITY,
    .tombstone_capacity = DSP_TOMBSTONE_CAPACITY,
};

static dsp_storage_status_t from_kv_read(dsp_kv_status_t s)
{
    switch (s) {
    case DSP_KV_OK:
        return DSP_STORAGE_OK;
    case DSP_KV_NOT_FOUND:
    case DSP_KV_ERR_TOO_LARGE:
        return DSP_STORAGE_ERR_CORRUPT; /* explicit presence / foreign size */
    default:
        return DSP_STORAGE_ERR_READ;
    }
}

dsp_storage_status_t dsp_storage_from_codec(dsp_codec_status_t st)
{
    return st == DSP_CODEC_ERR_VERSION ? DSP_STORAGE_ERR_UNSUPPORTED : DSP_STORAGE_ERR_CORRUPT;
}

bool dsp_storage_history_key(size_t slot, char *buf, size_t cap)
{
    if (buf == NULL || cap < 4 || slot >= DSP_STORAGE_HISTORY_SLOTS) {
        return false;
    }
    buf[0] = 'h';
    buf[1] = (char)('0' + slot / 10);
    buf[2] = (char)('0' + slot % 10);
    buf[3] = '\0';
    return true;
}

static dsp_storage_status_t raw_get(dsp_storage_t *st, const char *key, uint8_t *dst,
                                    size_t *len)
{
    *len = 0;
    return from_kv_read(st->kv->get(st->kv->ctx, key, dst, DSP_RECORD_MAX_BYTES, len));
}

dsp_storage_status_t dsp_storage_get(dsp_storage_t *st, const char *key, size_t *len)
{
    if (st == NULL || !st->open || key == NULL || len == NULL) {
        return DSP_STORAGE_ERR_NOT_OPEN;
    }
    return raw_get(st, key, st->buf, len);
}

static dsp_storage_status_t raw_put(dsp_storage_t *st, const char *key, size_t len)
{
    dsp_kv_status_t ws = st->kv->set(st->kv->ctx, key, st->buf, len);
    if (ws == DSP_KV_ERR_NO_SPACE) {
        return DSP_STORAGE_ERR_NO_SPACE;
    }
    if (ws != DSP_KV_OK) {
        return DSP_STORAGE_ERR_WRITE;
    }
    if (st->kv->commit(st->kv->ctx) != DSP_KV_OK) {
        return DSP_STORAGE_ERR_COMMIT;
    }
    size_t back_len = 0;
    if (st->kv->get(st->kv->ctx, key, st->readback, DSP_RECORD_MAX_BYTES, &back_len) != DSP_KV_OK ||
        back_len != len || memcmp(st->readback, st->buf, len) != 0) {
        return DSP_STORAGE_ERR_CONFIRM;
    }
    return DSP_STORAGE_OK;
}

dsp_storage_status_t dsp_storage_put(dsp_storage_t *st, const char *key, size_t len)
{
    if (st == NULL || !st->open) {
        return DSP_STORAGE_ERR_NOT_OPEN;
    }
    if (key == NULL || len == 0 || len > DSP_RECORD_MAX_BYTES) {
        return DSP_STORAGE_ERR_ARG;
    }
    return raw_put(st, key, len);
}

/* ---- Provisioning values ----------------------------------------------------------- */

typedef enum { KEY_HISTORY, KEY_SET, KEY_CONFIG_REC } key_kind_t;

static size_t encode_pristine(dsp_storage_t *st, key_kind_t kind)
{
    switch (kind) {
    case KEY_HISTORY:
        return dsp_codec_encode_history(NULL, st->buf, sizeof st->buf);
    case KEY_SET:
        memset(&st->set_scratch, 0, sizeof st->set_scratch); /* revision 0, no entries */
        return dsp_codec_encode_schedule_set(&st->set_scratch, st->tz, st->buf, sizeof st->buf);
    default: {
        dsp_config_t cfg;
        dsp_config_defaults(&cfg);
        return dsp_codec_encode_config(&cfg, st->buf, sizeof st->buf);
    }
    }
}

/* Classifies an existing value: OK = valid record (pristine reports whether it equals
 * the provisioning value), otherwise CORRUPT/UNSUPPORTED. */
static dsp_storage_status_t check_value(dsp_storage_t *st, key_kind_t kind, const uint8_t *v,
                                        size_t len, bool *pristine)
{
    dsp_codec_status_t cs;
    *pristine = false;
    switch (kind) {
    case KEY_HISTORY: {
        dsp_history_record_t rec;
        cs = dsp_codec_decode_history(v, len, &rec);
        if (cs == DSP_CODEC_EMPTY) {
            *pristine = true;
            return DSP_STORAGE_OK;
        }
        break;
    }
    case KEY_SET: {
        dsp_schedule_set_t *set = &st->set_scratch; /* caller-owned, not on the stack */
        cs = dsp_codec_decode_schedule_set(v, len, st->tz, set);
        if (cs == DSP_CODEC_OK) {
            *pristine = set->revision == 0 && set->count == 0 && set->tombstone_count == 0;
        }
        break;
    }
    default: {
        dsp_config_t cfg;
        dsp_config_t def;
        dsp_config_defaults(&def);
        cs = dsp_codec_decode_config(v, len, &cfg);
        if (cs == DSP_CODEC_OK) {
            *pristine = dsp_config_equal(&cfg, &def);
        }
        break;
    }
    }
    return cs == DSP_CODEC_OK ? DSP_STORAGE_OK : dsp_storage_from_codec(cs);
}

/* Iterates over all data keys (everything except the marker). */
#define DATA_KEYS (DSP_STORAGE_HISTORY_SLOTS + 2)

static void data_key(size_t i, char key[8], key_kind_t *kind)
{
    if (i < DSP_STORAGE_HISTORY_SLOTS) {
        (void)dsp_storage_history_key(i, key, 8);
        *kind = KEY_HISTORY;
    } else if (i == DSP_STORAGE_HISTORY_SLOTS) {
        strcpy(key, KEY_SCHED);
        *kind = KEY_SET;
    } else {
        strcpy(key, KEY_CONFIG);
        *kind = KEY_CONFIG_REC;
    }
}

static dsp_storage_status_t verify_all(dsp_storage_t *st)
{
    for (size_t i = 0; i < DATA_KEYS; i++) {
        char key[8];
        key_kind_t kind;
        data_key(i, key, &kind);
        size_t len;
        dsp_storage_status_t s = raw_get(st, key, st->readback, &len);
        if (s != DSP_STORAGE_OK) {
            return s;
        }
        bool pristine;
        s = check_value(st, kind, st->readback, len, &pristine);
        if (s != DSP_STORAGE_OK) {
            return s;
        }
    }
    /* Whole-history validation (corrupted records, duplicate schedule IDs). */
    dsp_history_store_t hs;
    st->open = true;
    dsp_storage_history_store(st, &hs);
    dsp_history_scan_t scan;
    dsp_exec_status_t es = dsp_history_scan(&hs, NULL, INT32_MIN, &scan);
    st->open = false;
    if (es == DSP_EXEC_ERR_STORAGE_READ) {
        return DSP_STORAGE_ERR_READ;
    }
    return es == DSP_EXEC_OK ? DSP_STORAGE_OK : DSP_STORAGE_ERR_CORRUPT;
}

static dsp_storage_status_t provision(dsp_storage_t *st, bool allow)
{
    bool missing[DATA_KEYS];
    size_t n_missing = 0;
    for (size_t i = 0; i < DATA_KEYS; i++) {
        char key[8];
        key_kind_t kind;
        data_key(i, key, &kind);
        size_t len = 0;
        dsp_kv_status_t ks = st->kv->get(st->kv->ctx, key, st->readback, DSP_RECORD_MAX_BYTES, &len);
        missing[i] = ks == DSP_KV_NOT_FOUND;
        if (missing[i]) {
            n_missing++;
            continue;
        }
        if (ks != DSP_KV_OK) {
            return from_kv_read(ks);
        }
        bool pristine;
        dsp_storage_status_t s = check_value(st, kind, st->readback, len, &pristine);
        if (s != DSP_STORAGE_OK) {
            return s;
        }
        if (!pristine) {
            return DSP_STORAGE_ERR_CORRUPT; /* real data without marker: marker lost */
        }
    }
    if (!allow) {
        return DSP_STORAGE_ERR_NOT_PROVISIONED;
    }
    for (size_t i = 0; i < DATA_KEYS && n_missing > 0; i++) {
        if (!missing[i]) {
            continue;
        }
        char key[8];
        key_kind_t kind;
        data_key(i, key, &kind);
        size_t len = encode_pristine(st, kind);
        if (len == 0) {
            return DSP_STORAGE_ERR_ARG;
        }
        dsp_storage_status_t s = raw_put(st, key, len);
        if (s != DSP_STORAGE_OK) {
            return s;
        }
    }
    size_t len = dsp_codec_encode_layout(&LAYOUT_V1, st->buf, sizeof st->buf);
    dsp_storage_status_t s = raw_put(st, KEY_LAYOUT, len); /* marker LAST */
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    st->provisioned_now = true;
    return verify_all(st);
}

dsp_storage_status_t dsp_storage_open(dsp_storage_t *st, const dsp_kv_t *kv, const dsp_tz_t *tz,
                                      bool allow_provision)
{
    if (st == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    st->open = false;
    st->provisioned_now = false;
    if (kv == NULL || kv->get == NULL || kv->set == NULL || kv->commit == NULL ||
        !dsp_tz_is_valid(tz)) {
        return DSP_STORAGE_ERR_ARG;
    }
    st->kv = kv;
    st->tz = tz;

    size_t len = 0;
    dsp_kv_status_t ks = kv->get(kv->ctx, KEY_LAYOUT, st->readback, DSP_RECORD_MAX_BYTES, &len);
    dsp_storage_status_t s;
    if (ks == DSP_KV_NOT_FOUND) {
        s = provision(st, allow_provision);
    } else if (ks != DSP_KV_OK) {
        s = from_kv_read(ks);
    } else {
        dsp_layout_t layout;
        dsp_codec_status_t cs = dsp_codec_decode_layout(st->readback, len, &layout);
        if (cs != DSP_CODEC_OK) {
            s = dsp_storage_from_codec(cs);
        } else if (layout.layout_version != LAYOUT_V1.layout_version ||
                   layout.history_slots != LAYOUT_V1.history_slots ||
                   layout.schedule_capacity != LAYOUT_V1.schedule_capacity ||
                   layout.tombstone_capacity != LAYOUT_V1.tombstone_capacity) {
            s = DSP_STORAGE_ERR_UNSUPPORTED;
        } else {
            s = verify_all(st);
        }
    }
    st->open = s == DSP_STORAGE_OK;
    return s;
}

/* ---- History store (M1.2 contract) ------------------------------------------------- */

static dsp_store_status_t hs_read(void *ctx, size_t slot, dsp_history_record_t *out)
{
    dsp_storage_t *st = ctx;
    char key[8];
    if (st == NULL || !st->open || !dsp_storage_history_key(slot, key, sizeof key) || out == NULL) {
        return DSP_STORE_ERR_READ;
    }
    uint8_t buf[DSP_HISTORY_RECORD_BYTES];
    size_t len = 0;
    if (st->kv->get(st->kv->ctx, key, buf, sizeof buf, &len) != DSP_KV_OK) {
        return DSP_STORE_ERR_READ; /* includes NOT_FOUND: missing = corrupted, not empty */
    }
    dsp_codec_status_t cs = dsp_codec_decode_history(buf, len, out);
    if (cs == DSP_CODEC_EMPTY) {
        return DSP_STORE_EMPTY;
    }
    return cs == DSP_CODEC_OK ? DSP_STORE_OK : DSP_STORE_ERR_READ;
}

static dsp_store_status_t hs_write(void *ctx, size_t slot, const dsp_history_record_t *rec)
{
    dsp_storage_t *st = ctx;
    char key[8];
    uint8_t buf[DSP_HISTORY_RECORD_BYTES];
    if (st == NULL || !st->open || !dsp_storage_history_key(slot, key, sizeof key) || rec == NULL ||
        dsp_codec_encode_history(rec, buf, sizeof buf) != DSP_HISTORY_RECORD_BYTES) {
        return DSP_STORE_ERR_WRITE;
    }
    return st->kv->set(st->kv->ctx, key, buf, sizeof buf) == DSP_KV_OK ? DSP_STORE_OK
                                                                     : DSP_STORE_ERR_WRITE;
}

static dsp_store_status_t hs_commit(void *ctx)
{
    dsp_storage_t *st = ctx;
    if (st == NULL || !st->open) {
        return DSP_STORE_ERR_COMMIT;
    }
    return st->kv->commit(st->kv->ctx) == DSP_KV_OK ? DSP_STORE_OK : DSP_STORE_ERR_COMMIT;
}

void dsp_storage_history_store(dsp_storage_t *st, dsp_history_store_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ctx = st;
    out->slot_count = DSP_STORAGE_HISTORY_SLOTS;
    out->read = hs_read;
    out->write = hs_write;
    out->commit = hs_commit;
}

/* ---- Configuration ----------------------------------------------------------------- */

dsp_storage_status_t dsp_storage_load_config(dsp_storage_t *st, dsp_config_t *out)
{
    if (out == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    size_t len;
    dsp_storage_status_t s = dsp_storage_get(st, KEY_CONFIG, &len);
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    dsp_codec_status_t cs = dsp_codec_decode_config(st->buf, len, out);
    return cs == DSP_CODEC_OK ? DSP_STORAGE_OK : dsp_storage_from_codec(cs);
}

dsp_storage_status_t dsp_storage_save_config(dsp_storage_t *st, const dsp_config_t *cfg)
{
    if (cfg == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    if (st == NULL || !st->open) {
        return DSP_STORAGE_ERR_NOT_OPEN;
    }
    if (dsp_config_validate(cfg) != 0) {
        return DSP_STORAGE_ERR_INVALID;
    }
    dsp_config_t current;
    dsp_storage_status_t s = dsp_storage_load_config(st, &current);
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    if (current.revision == UINT32_MAX || cfg->revision != current.revision + 1) {
        return DSP_STORAGE_ERR_REVISION;
    }
    size_t len = dsp_codec_encode_config(cfg, st->buf, sizeof st->buf);
    if (len == 0) {
        return DSP_STORAGE_ERR_INVALID;
    }
    return dsp_storage_put(st, KEY_CONFIG, len);
}
