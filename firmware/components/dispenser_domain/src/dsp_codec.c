#include "dsp_codec.h"

#include <string.h>

/* ---- Little-endian primitives (no struct overlays, no unaligned access) ---------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}

static void put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v; /* two's complement, well defined */
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (8 * i)) & 0xFFu);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t get_i64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)p[i] << (8 * i);
    }
    /* Defined conversion of the two's complement bit pattern (no implementation-defined
     * unsigned-to-signed cast for values above INT64_MAX). */
    if (u <= (uint64_t)INT64_MAX) {
        return (int64_t)u;
    }
    return -(int64_t)(~u) - 1;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Common header check: length >= 2, type, version. */
static dsp_codec_status_t check_header(const uint8_t *buf, size_t len, uint8_t type)
{
    if (buf == NULL || len < 2) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[0] != type) {
        return DSP_CODEC_ERR_TYPE;
    }
    if (buf[1] != DSP_CODEC_VERSION) {
        return DSP_CODEC_ERR_VERSION;
    }
    return DSP_CODEC_OK;
}

/* ---- Schedule IDs: 1 length byte + 64 zero-padded bytes ----------------------------- */

static void put_id(uint8_t *p, const char *id)
{
    size_t n = strlen(id); /* validated: 1..64 */
    p[0] = (uint8_t)n;
    memset(p + 1, 0, DSP_SCHEDULE_ID_MAX_LEN);
    memcpy(p + 1, id, n);
}

static bool get_id(const uint8_t *p, char out[DSP_SCHEDULE_ID_MAX_LEN + 1])
{
    size_t n = p[0];
    if (n < 1 || n > DSP_SCHEDULE_ID_MAX_LEN || !all_zero(p + 1 + n, DSP_SCHEDULE_ID_MAX_LEN - n)) {
        return false;
    }
    memset(out, 0, DSP_SCHEDULE_ID_MAX_LEN + 1);
    memcpy(out, p + 1, n);
    return dsp_schedule_id_is_valid(out); /* also rejects embedded NUL bytes */
}

static void put_date(uint8_t *p, dsp_date_t d)
{
    put_u16(p, (uint16_t)d.year);
    p[2] = d.month;
    p[3] = d.day;
}

static dsp_date_t get_date(const uint8_t *p)
{
    dsp_date_t d = {(int16_t)(get_u16(p) & 0x7FFF), p[2], p[3]};
    if (get_u16(p) > 0x7FFF) {
        d.year = 0; /* invalid; years are 2000..2099 */
    }
    return d;
}

/* ---- Layout marker ------------------------------------------------------------------ */

size_t dsp_codec_encode_layout(const dsp_layout_t *layout, uint8_t *buf, size_t cap)
{
    if (layout == NULL || buf == NULL || cap < DSP_LAYOUT_RECORD_BYTES) {
        return 0;
    }
    buf[0] = DSP_REC_TYPE_LAYOUT;
    buf[1] = DSP_CODEC_VERSION;
    put_u16(buf + 2, layout->layout_version);
    buf[4] = layout->history_slots;
    buf[5] = layout->schedule_capacity;
    buf[6] = layout->tombstone_capacity;
    return DSP_LAYOUT_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_layout(const uint8_t *buf, size_t len, dsp_layout_t *out)
{
    dsp_codec_status_t st = check_header(buf, len, DSP_REC_TYPE_LAYOUT);
    if (st != DSP_CODEC_OK) {
        return st;
    }
    if (len != DSP_LAYOUT_RECORD_BYTES || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    out->layout_version = get_u16(buf + 2);
    out->history_slots = buf[4];
    out->schedule_capacity = buf[5];
    out->tombstone_capacity = buf[6];
    return DSP_CODEC_OK;
}

/* ---- History slot --------------------------------------------------------------------
 *  0 type | 1 version | 2 state (0 = EMPTY) | 3..67 id | 68..71 local date |
 *  72..75 revision | 76..83 scheduled_at | 84..91 claimed_at | format 2: 92..95 volume   */

size_t dsp_codec_encode_history(const dsp_history_record_t *rec, uint8_t *buf, size_t cap)
{
    if (buf == NULL || cap < DSP_HISTORY_RECORD_BYTES ||
        (rec != NULL && !dsp_history_record_is_valid(rec))) {
        return 0;
    }
    memset(buf, 0, DSP_HISTORY_RECORD_BYTES);
    buf[0] = DSP_REC_TYPE_HISTORY;
    buf[1] = DSP_HISTORY_FORMAT_V2;
    if (rec == NULL) {
        return DSP_HISTORY_RECORD_BYTES; /* EMPTY: state 0, all fields zero */
    }
    buf[2] = rec->state;
    put_id(buf + 3, rec->schedule_id);
    put_date(buf + 68, rec->local_date);
    put_u32(buf + 72, rec->schedule_revision);
    put_i64(buf + 76, rec->scheduled_at);
    put_i64(buf + 84, rec->claimed_at);
    put_u32(buf + 92, rec->volume_ul);
    return DSP_HISTORY_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_history(const uint8_t *buf, size_t len,
                                            dsp_history_record_t *out)
{
    if (buf == NULL || len < 2) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[0] != DSP_REC_TYPE_HISTORY) {
        return DSP_CODEC_ERR_TYPE;
    }
    if (buf[1] != DSP_CODEC_VERSION && buf[1] != DSP_HISTORY_FORMAT_V2) {
        return DSP_CODEC_ERR_VERSION;
    }
    bool v2 = buf[1] == DSP_HISTORY_FORMAT_V2;
    size_t expect = v2 ? DSP_HISTORY_RECORD_BYTES : DSP_HISTORY_RECORD_V1_BYTES;
    if (len != expect || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[2] == 0) {
        return all_zero(buf + 3, expect - 3) ? DSP_CODEC_EMPTY : DSP_CODEC_ERR_FIELD;
    }
    dsp_history_record_t rec;
    memset(&rec, 0, sizeof rec);
    if (!get_id(buf + 3, rec.schedule_id)) {
        return DSP_CODEC_ERR_FIELD;
    }
    rec.state = buf[2];
    rec.local_date = get_date(buf + 68);
    rec.schedule_revision = get_u32(buf + 72);
    rec.scheduled_at = get_i64(buf + 76);
    rec.claimed_at = get_i64(buf + 84);
    rec.volume_ul = v2 ? get_u32(buf + 92) : 0; /* format 1: unknown */
    if (!dsp_history_record_is_valid(&rec)) {
        return DSP_CODEC_ERR_FIELD;
    }
    *out = rec;
    return DSP_CODEC_OK;
}

/* ---- Schedule set ------------------------------------------------------------------
 *  header: 0 type | 1 version | 2..5 set revision | 6 count | 7 tombstone count
 *  entry (91 B): 0..64 id | 65..68 revision | 69 enabled | 70..73 amount_ul |
 *    74..77 start date | 78 hour | 79 minute | 80 kind | 81..84 interval |
 *    85 weekday mask | 86 has end | 87..90 end date (zero when absent)
 *  format 2 entry (132 B, M1.10b.1): format 1 entry | 91 name length (0..32) |
 *    92..123 name bytes (zero padded) | 124..131 not_before (i64)
 *  tombstone (65 B): id                                                                */

static bool ids_unique(const dsp_schedule_set_t *set)
{
    size_t n = (size_t)set->count + set->tombstone_count;
    for (size_t i = 0; i < n; i++) {
        const char *a = i < set->count ? set->schedules[i].id : set->tombstones[i - set->count];
        for (size_t j = 0; j < i; j++) {
            const char *b =
                j < set->count ? set->schedules[j].id : set->tombstones[j - set->count];
            if (strcmp(a, b) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool set_valid(const dsp_schedule_set_t *set, const dsp_tz_t *tz)
{
    if (set->count > DSP_SCHEDULE_CAPACITY || set->tombstone_count > DSP_TOMBSTONE_CAPACITY) {
        return false;
    }
    for (size_t i = 0; i < set->count; i++) {
        if (dsp_schedule_validate(&set->schedules[i], tz) != DSP_SCHED_OK) {
            return false;
        }
    }
    for (size_t i = 0; i < set->tombstone_count; i++) {
        if (!dsp_schedule_id_is_valid(set->tombstones[i])) {
            return false;
        }
    }
    return ids_unique(set);
}

static void put_schedule(uint8_t *p, const dsp_schedule_t *s)
{
    memset(p, 0, DSP_SCHEDULE_ENTRY_BYTES);
    put_id(p, s->id);
    put_u32(p + 65, s->revision);
    p[69] = s->enabled ? 1 : 0;
    put_u32(p + 70, s->amount_ul);
    put_date(p + 74, s->start_date);
    p[78] = s->hour;
    p[79] = s->minute;
    p[80] = s->kind;
    put_u32(p + 81, s->interval);
    p[85] = s->weekday_mask;
    p[86] = s->has_end_date ? 1 : 0;
    if (s->has_end_date) {
        put_date(p + 87, s->end_date);
    }
    size_t name_len = strlen(s->name); /* validated: <= DSP_SCHEDULE_NAME_MAX_BYTES */
    p[91] = (uint8_t)name_len;
    memcpy(p + 92, s->name, name_len);
    put_i64(p + 124, s->not_before);
}

static bool get_schedule(const uint8_t *p, bool v2, dsp_schedule_t *s)
{
    memset(s, 0, sizeof *s);
    if (!get_id(p, s->id) || p[69] > 1 || p[86] > 1) {
        return false;
    }
    s->revision = get_u32(p + 65);
    s->enabled = p[69] == 1;
    s->amount_ul = get_u32(p + 70);
    s->start_date = get_date(p + 74);
    s->hour = p[78];
    s->minute = p[79];
    s->kind = p[80];
    s->interval = get_u32(p + 81);
    s->weekday_mask = p[85];
    s->has_end_date = p[86] == 1;
    if (s->has_end_date) {
        s->end_date = get_date(p + 87);
    } else if (!all_zero(p + 87, 4)) {
        return false;
    }
    if (v2) {
        uint8_t name_len = p[91];
        if (name_len > DSP_SCHEDULE_NAME_MAX_BYTES ||
            !all_zero(p + 92 + name_len, DSP_SCHEDULE_NAME_MAX_BYTES - name_len) ||
            memchr(p + 92, 0, name_len) != NULL) {
            return false; /* padding must be zero; an embedded NUL would truncate the name */
        }
        memcpy(s->name, p + 92, name_len);
        s->name[name_len] = '\0';
        s->not_before = get_i64(p + 124);
    }
    return true; /* name text rules and the floor range: dsp_schedule_validate */
}

static size_t set_size(uint8_t count, uint8_t tombstones, bool v2)
{
    return DSP_SCHEDULE_SET_HEADER_BYTES +
           (size_t)count * (v2 ? DSP_SCHEDULE_ENTRY_BYTES : DSP_SCHEDULE_ENTRY_V1_BYTES) +
           (size_t)tombstones * DSP_TOMBSTONE_ENTRY_BYTES;
}

size_t dsp_codec_encode_schedule_set(const dsp_schedule_set_t *set, const dsp_tz_t *tz,
                                     uint8_t *buf, size_t cap)
{
    if (set == NULL || buf == NULL || !set_valid(set, tz)) {
        return 0;
    }
    size_t size = set_size(set->count, set->tombstone_count, true);
    if (cap < size) {
        return 0;
    }
    buf[0] = DSP_REC_TYPE_SCHEDULE_SET;
    buf[1] = DSP_SCHEDULE_SET_FORMAT_V2;
    put_u32(buf + 2, set->revision);
    buf[6] = set->count;
    buf[7] = set->tombstone_count;
    uint8_t *p = buf + DSP_SCHEDULE_SET_HEADER_BYTES;
    for (size_t i = 0; i < set->count; i++, p += DSP_SCHEDULE_ENTRY_BYTES) {
        put_schedule(p, &set->schedules[i]);
    }
    for (size_t i = 0; i < set->tombstone_count; i++, p += DSP_TOMBSTONE_ENTRY_BYTES) {
        put_id(p, set->tombstones[i]);
    }
    return size;
}

dsp_codec_status_t dsp_codec_decode_schedule_set(const uint8_t *buf, size_t len,
                                                 const dsp_tz_t *tz, dsp_schedule_set_t *out)
{
    if (buf == NULL || len < 2) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[0] != DSP_REC_TYPE_SCHEDULE_SET) {
        return DSP_CODEC_ERR_TYPE;
    }
    if (buf[1] != DSP_CODEC_VERSION && buf[1] != DSP_SCHEDULE_SET_FORMAT_V2) {
        return DSP_CODEC_ERR_VERSION; /* newer format: unsupported (fail closed) */
    }
    bool v2 = buf[1] == DSP_SCHEDULE_SET_FORMAT_V2;
    size_t entry = v2 ? DSP_SCHEDULE_ENTRY_BYTES : DSP_SCHEDULE_ENTRY_V1_BYTES;
    if (len < DSP_SCHEDULE_SET_HEADER_BYTES || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    uint8_t count = buf[6];
    uint8_t tombstones = buf[7];
    if (count > DSP_SCHEDULE_CAPACITY || tombstones > DSP_TOMBSTONE_CAPACITY) {
        return DSP_CODEC_ERR_FIELD;
    }
    if (len != set_size(count, tombstones, v2)) {
        return DSP_CODEC_ERR_SIZE;
    }
    /* Decode into *out only after full validation would need a second set; decode in
     * place and wipe on failure so a half-decoded set is never left behind. */
    memset(out, 0, sizeof *out);
    out->revision = get_u32(buf + 2);
    out->count = count;
    out->tombstone_count = tombstones;
    const uint8_t *p = buf + DSP_SCHEDULE_SET_HEADER_BYTES;
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++, p += entry) {
        ok = get_schedule(p, v2, &out->schedules[i]);
    }
    for (size_t i = 0; i < tombstones && ok; i++, p += DSP_TOMBSTONE_ENTRY_BYTES) {
        ok = get_id(p, out->tombstones[i]);
    }
    if (!ok || !set_valid(out, tz)) {
        memset(out, 0, sizeof *out);
        return DSP_CODEC_ERR_FIELD;
    }
    return DSP_CODEC_OK;
}

/* ---- Device configuration ------------------------------------------------------------
 *  0 type | 1 version | 2..5 revision | 6..9 capacity_ul | 10..13 remaining_ul |
 *  14 low_stock_percent | 15..16 time_sync_interval_h | 17 notify_mask                */

size_t dsp_codec_encode_config(const dsp_config_t *cfg, uint8_t *buf, size_t cap)
{
    if (cfg == NULL || buf == NULL || cap < DSP_CONFIG_RECORD_BYTES ||
        dsp_config_validate(cfg) != 0) {
        return 0;
    }
    buf[0] = DSP_REC_TYPE_CONFIG;
    buf[1] = DSP_CONFIG_FORMAT_V2;
    put_u32(buf + 2, cfg->revision);
    put_u32(buf + 6, cfg->reservoir_capacity_ul);
    put_u32(buf + 10, cfg->reservoir_remaining_ul);
    buf[14] = cfg->low_stock_percent;
    put_u16(buf + 15, cfg->time_sync_interval_h);
    buf[17] = cfg->notify_mask;
    put_u32(buf + 18, cfg->pump_flow_ul_per_s);
    put_i64(buf + 22, cfg->calibrated_at);
    put_u32(buf + 30, cfg->max_dose_ul);
    put_u32(buf + 34, cfg->max_daily_ul);
    put_u32(buf + 38, cfg->max_runtime_ms);
    put_u32(buf + 42, cfg->min_interval_s);
    return DSP_CONFIG_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_config(const uint8_t *buf, size_t len, dsp_config_t *out)
{
    if (buf == NULL || len < 2) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[0] != DSP_REC_TYPE_CONFIG) {
        return DSP_CODEC_ERR_TYPE;
    }
    if (buf[1] != DSP_CODEC_VERSION && buf[1] != DSP_CONFIG_FORMAT_V2) {
        return DSP_CODEC_ERR_VERSION;
    }
    bool v2 = buf[1] == DSP_CONFIG_FORMAT_V2;
    if (len != (v2 ? DSP_CONFIG_RECORD_BYTES : DSP_CONFIG_RECORD_V1_BYTES) || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    dsp_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.revision = get_u32(buf + 2);
    cfg.reservoir_capacity_ul = get_u32(buf + 6);
    cfg.reservoir_remaining_ul = get_u32(buf + 10);
    cfg.low_stock_percent = buf[14];
    cfg.time_sync_interval_h = get_u16(buf + 15);
    cfg.notify_mask = buf[17];
    if (v2) {
        cfg.pump_flow_ul_per_s = get_u32(buf + 18);
        cfg.calibrated_at = get_i64(buf + 22);
        cfg.max_dose_ul = get_u32(buf + 30);
        cfg.max_daily_ul = get_u32(buf + 34);
        cfg.max_runtime_ms = get_u32(buf + 38);
        cfg.min_interval_s = get_u32(buf + 42);
    }
    if (dsp_config_validate(&cfg) != 0) {
        return DSP_CODEC_ERR_FIELD;
    }
    *out = cfg;
    return DSP_CODEC_OK;
}

/* ---- Time metadata ------------------------------------------------------------------ */

size_t dsp_codec_encode_time(const dsp_time_meta_t *meta, uint8_t *buf, size_t cap)
{
    if (meta == NULL || buf == NULL || cap < DSP_TIME_RECORD_BYTES || !dsp_time_meta_is_valid(meta)) {
        return 0;
    }
    buf[0] = DSP_REC_TYPE_TIME;
    buf[1] = DSP_CODEC_VERSION;
    put_i64(buf + 2, meta->last_sync_utc);
    put_i64(buf + 10, meta->high_water_utc);
    put_i64(buf + 18, meta->hold_until_utc);
    put_i64(buf + 26, meta->last_correction_s);
    put_i64(buf + 34, meta->not_before_utc);
    return DSP_TIME_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_time(const uint8_t *buf, size_t len, dsp_time_meta_t *out)
{
    dsp_codec_status_t st = check_header(buf, len, DSP_REC_TYPE_TIME);
    if (st != DSP_CODEC_OK) {
        return st;
    }
    if (len != DSP_TIME_RECORD_BYTES || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    dsp_time_meta_t m;
    m.last_sync_utc = get_i64(buf + 2);
    m.high_water_utc = get_i64(buf + 10);
    m.hold_until_utc = get_i64(buf + 18);
    m.last_correction_s = get_i64(buf + 26);
    m.not_before_utc = get_i64(buf + 34);
    if (!dsp_time_meta_is_valid(&m)) {
        return DSP_CODEC_ERR_FIELD;
    }
    *out = m;
    return DSP_CODEC_OK;
}

/* ---- Notification state ------------------------------------------------------------- */

size_t dsp_codec_encode_notify(const dsp_notify_state_t *s, uint8_t *buf, size_t cap)
{
    if (s == NULL || buf == NULL || cap < DSP_NOTIFY_RECORD_BYTES || !dsp_notify_state_is_valid(s)) {
        return 0;
    }
    memset(buf, 0, DSP_NOTIFY_RECORD_BYTES);
    buf[0] = DSP_REC_TYPE_NOTIFY;
    buf[1] = DSP_NOTIFY_FORMAT_V3;
    put_u32(buf + 2, s->epoch);
    put_u32(buf + 6, s->next_seq);
    put_u16(buf + 10, s->dropped);
    buf[12] = s->flags;
    uint8_t *p = buf + DSP_NOTIFY_HEADER_BYTES;
    for (size_t c = 0; c < DSP_COND_COUNT; c++, p += DSP_NOTIFY_COND_BYTES) {
        p[0] = s->cond[c].flags;
        p[1] = s->cond[c].reminders;
        put_i64(p + 2, s->cond[c].opened_at);
        put_i64(p + 10, s->cond[c].next_reminder_at);
        put_u32(p + 18, s->cond[c].value);
        put_u32(p + 22, s->cond[c].value2);
    }
    for (size_t i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++, p += DSP_NOTIFY_ENTRY_BYTES) {
        const dsp_note_t *e = &s->queue[i];
        put_u32(p, e->seq);
        p[4] = e->type;
        p[5] = e->severity;
        p[6] = e->condition;
        p[7] = e->flags;
        p[8] = e->reminder_no;
        put_u16(p + 10, e->attempts);
        put_i64(p + 12, e->created_at);
        put_u16(p + 20, e->code);
        put_u32(p + 22, e->value);
        put_u32(p + 26, e->value2);
    }
    /* Format 2: diagnostics and rejection log. */
    p[0] = s->tx_diag;
    put_u16(p + 2, s->tx_diag_status);
    p += DSP_NOTIFY_DIAG_BYTES;
    const dsp_rejection_log_t *log = &s->rejections;
    put_u32(p, log->total);
    put_u16(p + 4, log->lost_unread);
    p[6] = log->unread;
    p[7] = log->count;
    p += DSP_NOTIFY_REJLOG_HEADER_BYTES;
    for (size_t i = 0; i < DSP_REJECTION_LOG_CAPACITY; i++, p += DSP_NOTIFY_REJECTION_BYTES) {
        const dsp_rejection_t *r = &log->entry[i];
        put_u32(p, r->seq);
        p[4] = r->type;
        p[5] = r->flags;
        put_u16(p + 6, r->code);
        put_u16(p + 8, r->http_status);
        p[10] = r->error;
        p[11] = r->field;
        put_u16(p + 12, r->attempts);
    }
    /* Format 3: dose-bound report keys. */
    p[0] = s->report_key_count;
    p += 4;
    for (size_t i = 0; i < DSP_NOTIFY_REPORT_KEYS; i++, p += 8) {
        put_u32(p, (uint32_t)(s->report_keys[i] & 0xFFFFFFFFu));
        put_u32(p + 4, (uint32_t)(s->report_keys[i] >> 32));
    }
    return DSP_NOTIFY_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_notify(const uint8_t *buf, size_t len, dsp_notify_state_t *out)
{
    if (buf == NULL || len < 2) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[0] != DSP_REC_TYPE_NOTIFY) {
        return DSP_CODEC_ERR_TYPE;
    }
    if (buf[1] != DSP_NOTIFY_FORMAT_V1 && buf[1] != DSP_NOTIFY_FORMAT_V2 &&
        buf[1] != DSP_NOTIFY_FORMAT_V3) {
        return DSP_CODEC_ERR_VERSION; /* newer format: unsupported, never guessed */
    }
    bool v3 = buf[1] == DSP_NOTIFY_FORMAT_V3;
    bool v2 = buf[1] == DSP_NOTIFY_FORMAT_V2 || v3;
    size_t expect = v3 ? DSP_NOTIFY_RECORD_BYTES
                       : (v2 ? DSP_NOTIFY_RECORD_V2_BYTES : DSP_NOTIFY_RECORD_V1_BYTES);
    if (len != expect || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    if (buf[13] != 0) {
        return DSP_CODEC_ERR_FIELD;
    }
    dsp_notify_state_t s;
    memset(&s, 0, sizeof s);
    s.epoch = get_u32(buf + 2);
    s.next_seq = get_u32(buf + 6);
    s.dropped = get_u16(buf + 10);
    s.flags = buf[12];
    const uint8_t *p = buf + DSP_NOTIFY_HEADER_BYTES;
    for (size_t c = 0; c < DSP_COND_COUNT; c++, p += DSP_NOTIFY_COND_BYTES) {
        s.cond[c].flags = p[0];
        s.cond[c].reminders = p[1];
        s.cond[c].opened_at = get_i64(p + 2);
        s.cond[c].next_reminder_at = get_i64(p + 10);
        s.cond[c].value = get_u32(p + 18);
        s.cond[c].value2 = get_u32(p + 22);
    }
    for (size_t i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++, p += DSP_NOTIFY_ENTRY_BYTES) {
        if (p[9] != 0 || p[30] != 0 || p[31] != 0) {
            return DSP_CODEC_ERR_FIELD;
        }
        dsp_note_t *e = &s.queue[i];
        e->seq = get_u32(p);
        e->type = p[4];
        e->severity = p[5];
        e->condition = p[6];
        e->flags = p[7];
        e->reminder_no = p[8];
        e->attempts = get_u16(p + 10);
        e->created_at = get_i64(p + 12);
        e->code = get_u16(p + 20);
        e->value = get_u32(p + 22);
        e->value2 = get_u32(p + 26);
    }
    if (v2) {
        if (p[1] != 0) {
            return DSP_CODEC_ERR_FIELD;
        }
        s.tx_diag = p[0];
        s.tx_diag_status = get_u16(p + 2);
        p += DSP_NOTIFY_DIAG_BYTES;
        dsp_rejection_log_t *log = &s.rejections;
        log->total = get_u32(p);
        log->lost_unread = get_u16(p + 4);
        log->unread = p[6];
        log->count = p[7];
        p += DSP_NOTIFY_REJLOG_HEADER_BYTES;
        for (size_t i = 0; i < DSP_REJECTION_LOG_CAPACITY; i++, p += DSP_NOTIFY_REJECTION_BYTES) {
            if (p[14] != 0 || p[15] != 0) {
                return DSP_CODEC_ERR_FIELD;
            }
            dsp_rejection_t *r = &log->entry[i];
            r->seq = get_u32(p);
            r->type = p[4];
            r->flags = p[5];
            r->code = get_u16(p + 6);
            r->http_status = get_u16(p + 8);
            r->error = p[10];
            r->field = p[11];
            r->attempts = get_u16(p + 12);
        }
    }
    if (v3) {
        if (p[1] != 0 || p[2] != 0 || p[3] != 0) {
            return DSP_CODEC_ERR_FIELD;
        }
        s.report_key_count = p[0];
        p += 4;
        for (size_t i = 0; i < DSP_NOTIFY_REPORT_KEYS; i++, p += 8) {
            s.report_keys[i] = (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
        }
    }
    if (!dsp_notify_state_is_valid(&s)) {
        return DSP_CODEC_ERR_FIELD;
    }
    *out = s;
    return DSP_CODEC_OK;
}

/* ---- Dosing state (layout v4, M1.9a) ------------------------------------------------- */

size_t dsp_codec_encode_dosing(const dsp_dosing_state_t *d, uint8_t *buf, size_t cap)
{
    if (d == NULL || buf == NULL || cap < DSP_DOSING_RECORD_BYTES || !dsp_dosing_state_is_valid(d)) {
        return 0;
    }
    memset(buf, 0, DSP_DOSING_RECORD_BYTES);
    buf[0] = DSP_REC_TYPE_DOSING;
    buf[1] = DSP_CODEC_VERSION;
    buf[2] = d->readiness;
    buf[3] = d->flags;
    put_u32(buf + 4, d->refill_capacity_ul);
    put_u32(buf + 8, d->refill_remaining_ul);
    put_u32(buf + 12, d->refill_seq);
    put_u32(buf + 16, d->manual_volume_ul);
    put_i64(buf + 20, d->manual_started_at);
    put_u32(buf + 28, d->manual_seq);
    put_date(buf + 32, d->ledger_date);
    put_u32(buf + 36, d->ledger_sum_ul);
    put_i64(buf + 40, d->last_start_utc);
    put_u32(buf + 48, d->next_seq);
    return DSP_DOSING_RECORD_BYTES;
}

dsp_codec_status_t dsp_codec_decode_dosing(const uint8_t *buf, size_t len, dsp_dosing_state_t *out)
{
    dsp_codec_status_t st = check_header(buf, len, DSP_REC_TYPE_DOSING);
    if (st != DSP_CODEC_OK) {
        return st;
    }
    if (len != DSP_DOSING_RECORD_BYTES || out == NULL) {
        return DSP_CODEC_ERR_SIZE;
    }
    dsp_dosing_state_t d;
    memset(&d, 0, sizeof d);
    d.readiness = buf[2];
    d.flags = buf[3];
    d.refill_capacity_ul = get_u32(buf + 4);
    d.refill_remaining_ul = get_u32(buf + 8);
    d.refill_seq = get_u32(buf + 12);
    d.manual_volume_ul = get_u32(buf + 16);
    d.manual_started_at = get_i64(buf + 20);
    d.manual_seq = get_u32(buf + 28);
    d.ledger_date = get_date(buf + 32);
    d.ledger_sum_ul = get_u32(buf + 36);
    d.last_start_utc = get_i64(buf + 40);
    d.next_seq = get_u32(buf + 48);
    if (!dsp_dosing_state_is_valid(&d)) {
        return DSP_CODEC_ERR_FIELD;
    }
    *out = d;
    return DSP_CODEC_OK;
}
