#include "dsp_evlog.h"

#include <string.h>

/*
 * Flash record, format 1 (little-endian):
 *   0 format u8 = 1 | 1 type u8 | 2 result u8 | 3 flags u8 | 4 seq u32 | 8 at i64 |
 *  16 volume_ul u32 | 20 before_ul u32 | 24 after_ul u32 | 28 capacity_ul u32 | 32 key u64 |
 *  40 id_len u8 (0..64) | 41 id bytes. Total length exactly 41 + id_len.
 */

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

bool dsp_evlog_key(size_t slot, char *buf, size_t cap)
{
    if (buf == NULL || cap < 5 || slot >= DSP_EVLOG_SLOTS) {
        return false;
    }
    buf[0] = 'e';
    buf[1] = 'v';
    buf[2] = (char)('0' + slot / 10);
    buf[3] = (char)('0' + slot % 10);
    buf[4] = '\0';
    return true;
}

/* Length of the id, or sizeof (not terminated) - strict C11, no strnlen. */
static size_t id_len(const dsp_event_t *ev)
{
    const void *z = memchr(ev->schedule_id, '\0', sizeof ev->schedule_id);
    return z == NULL ? sizeof ev->schedule_id
                     : (size_t)((const char *)z - ev->schedule_id);
}

bool dsp_event_is_valid(const dsp_event_t *ev)
{
    if (ev == NULL || ev->type < 1 || ev->type > DSP_EV_TYPE_MAX || ev->result > DSP_EVR_MAX ||
        (ev->flags & ~DSP_EVF_ALL) != 0) {
        return false;
    }
    if (ev->at != 0 && (ev->at < DSP_UTC_MIN || ev->at > DSP_UTC_MAX)) {
        return false;
    }
    size_t n = id_len(ev);
    if (n > DSP_SCHEDULE_ID_MAX_LEN) {
        return false; /* not NUL-terminated */
    }
    if (ev->type == DSP_EV_DOSE_SCHEDULED) {
        if (!dsp_schedule_id_is_valid(ev->schedule_id)) {
            return false;
        }
    } else if (n != 0) {
        return false;
    }
    bool dose = ev->type == DSP_EV_DOSE_SCHEDULED || ev->type == DSP_EV_DOSE_MANUAL;
    if (!dose && (ev->result != DSP_EVR_OK || ev->volume_ul != 0)) {
        return false;
    }
    if ((ev->flags & DSP_EVF_RESERVOIR_UNKNOWN) && (ev->before_ul != 0 || ev->after_ul != 0)) {
        return false;
    }
    return true;
}

size_t dsp_event_encode(const dsp_event_t *ev, uint8_t *buf, size_t cap)
{
    if (buf == NULL || !dsp_event_is_valid(ev) || ev->seq == 0) {
        return 0;
    }
    size_t n = id_len(ev);
    size_t len = DSP_EVLOG_HEADER_BYTES + n;
    if (cap < len) {
        return 0;
    }
    buf[0] = DSP_EVLOG_FORMAT;
    buf[1] = ev->type;
    buf[2] = ev->result;
    buf[3] = ev->flags;
    put_u32(buf + 4, ev->seq);
    put_u64(buf + 8, (uint64_t)ev->at);
    put_u32(buf + 16, ev->volume_ul);
    put_u32(buf + 20, ev->before_ul);
    put_u32(buf + 24, ev->after_ul);
    put_u32(buf + 28, ev->capacity_ul);
    put_u64(buf + 32, ev->key);
    buf[40] = (uint8_t)n;
    memcpy(buf + 41, ev->schedule_id, n);
    return len;
}

bool dsp_event_decode(const uint8_t *buf, size_t len, dsp_event_t *out)
{
    if (buf == NULL || out == NULL || len < DSP_EVLOG_HEADER_BYTES || buf[0] != DSP_EVLOG_FORMAT) {
        return false;
    }
    size_t n = buf[40];
    if (n > DSP_SCHEDULE_ID_MAX_LEN || len != DSP_EVLOG_HEADER_BYTES + n) {
        return false;
    }
    dsp_event_t e;
    memset(&e, 0, sizeof e);
    e.type = buf[1];
    e.result = buf[2];
    e.flags = buf[3];
    e.seq = get_u32(buf + 4);
    e.at = (dsp_utc_t)get_u64(buf + 8);
    e.volume_ul = get_u32(buf + 16);
    e.before_ul = get_u32(buf + 20);
    e.after_ul = get_u32(buf + 24);
    e.capacity_ul = get_u32(buf + 28);
    e.key = get_u64(buf + 32);
    if (memchr(buf + 41, '\0', n) != NULL) {
        return false; /* an embedded NUL would shorten the id */
    }
    memcpy(e.schedule_id, buf + 41, n);
    if (e.seq == 0 || !dsp_event_is_valid(&e)) {
        return false;
    }
    *out = e;
    return true;
}

static void fault(dsp_evlog_t *log)
{
    if (log->faults < UINT16_MAX) {
        log->faults++;
    }
}

typedef enum { SLOT_EMPTY, SLOT_OK, SLOT_BAD } slot_read_t;

static slot_read_t read_slot(dsp_evlog_t *log, size_t slot, dsp_event_t *out)
{
    char key[8];
    (void)dsp_evlog_key(slot, key, sizeof key);
    size_t len = 0;
    dsp_kv_status_t ks = log->kv->get(log->kv->ctx, key, log->buf, sizeof log->buf, &len);
    if (ks == DSP_KV_NOT_FOUND) {
        return SLOT_EMPTY;
    }
    if (ks != DSP_KV_OK || !dsp_event_decode(log->buf, len, out) ||
        out->seq % DSP_EVLOG_SLOTS != slot) {
        return SLOT_BAD;
    }
    return SLOT_OK;
}

bool dsp_evlog_open(dsp_evlog_t *log, const dsp_kv_t *kv)
{
    if (log == NULL || kv == NULL || kv->get == NULL || kv->set == NULL || kv->commit == NULL) {
        return false;
    }
    memset(log, 0, sizeof *log);
    log->kv = kv;
    uint32_t max = 0;
    bool bad = false;
    for (size_t i = 0; i < DSP_EVLOG_SLOTS; i++) {
        dsp_event_t e;
        slot_read_t r = read_slot(log, i, &e);
        if (r == SLOT_OK) {
            log->seq[i] = e.seq;
            log->key[i] = e.key;
            if (e.seq > max) {
                max = e.seq;
            }
        } else if (r == SLOT_BAD) {
            bad = true;
            fault(log);
        }
    }
    /* No ordering window is needed: a slot only ever holds the latest seq written to it
     * (slot = seq % SLOTS, seqs strictly increase), so an older seq in a slot is simply an
     * older event (e.g. after a gap) and sorts correctly. */
    uint32_t hw = 0;
    size_t len = 0;
    dsp_kv_status_t ks = kv->get(kv->ctx, DSP_EVLOG_HW_KEY, log->buf, sizeof log->buf, &len);
    bool hw_ok = ks == DSP_KV_OK && len == DSP_EVLOG_HW_BYTES && log->buf[0] == 1u;
    if (hw_ok) {
        hw = get_u32(log->buf + 1);
    }
    if (hw_ok && hw >= max) {
        log->head = hw; /* covers every seq ever used, readable or not */
        log->reserved = hw;
    } else if (ks == DSP_KV_NOT_FOUND && max == 0 && !bad) {
        log->head = 0; /* a log that was never written */
    } else {
        /* The bound is missing, unreadable, malformed or behind a readable event: every seq it can
         * have covered is below max + SLOTS + RESERVE. The next append reserves afresh. */
        fault(log);
        uint64_t h = (uint64_t)max + DSP_EVLOG_SLOTS + DSP_EVLOG_RESERVE;
        log->head = h > UINT32_MAX ? UINT32_MAX : (uint32_t)h;
    }
    log->open = true;
    return true;
}

dsp_evlog_status_t dsp_evlog_append(dsp_evlog_t *log, const dsp_event_t *ev, uint32_t *seq_out)
{
    if (log == NULL || !log->open || !dsp_event_is_valid(ev) || log->head == UINT32_MAX) {
        return DSP_EVLOG_ERR_ARG;
    }
    if (ev->key != 0) {
        for (size_t i = 0; i < DSP_EVLOG_SLOTS; i++) {
            if (log->seq[i] != 0 && log->key[i] == ev->key) {
                if (seq_out != NULL) {
                    *seq_out = log->seq[i];
                }
                return DSP_EVLOG_DUPLICATE;
            }
        }
    }
    dsp_event_t e = *ev;
    e.seq = log->head + 1;
    if (e.seq > log->reserved) {
        /* Durable reservation BEFORE the event: a seq is used only below a confirmed bound. */
        uint64_t b = (uint64_t)e.seq + DSP_EVLOG_RESERVE - 1u;
        uint32_t bound = b > UINT32_MAX ? UINT32_MAX : (uint32_t)b;
        uint8_t hb[DSP_EVLOG_HW_BYTES] = {1u};
        put_u32(hb + 1, bound);
        size_t rl = 0;
        if (log->kv->set(log->kv->ctx, DSP_EVLOG_HW_KEY, hb, sizeof hb) != DSP_KV_OK ||
            log->kv->commit(log->kv->ctx) != DSP_KV_OK ||
            log->kv->get(log->kv->ctx, DSP_EVLOG_HW_KEY, log->rb, sizeof log->rb, &rl) != DSP_KV_OK ||
            rl != sizeof hb || memcmp(hb, log->rb, sizeof hb) != 0) {
            /* old or new bound durable (unknown): the head is unchanged and no event is written,
             * so no seq above the old bound is ever used without a confirmed reservation */
            fault(log);
            return DSP_EVLOG_ERR_WRITE;
        }
        log->reserved = bound;
    }
    size_t slot = e.seq % DSP_EVLOG_SLOTS;
    size_t len = dsp_event_encode(&e, log->buf, sizeof log->buf);
    if (len == 0) {
        return DSP_EVLOG_ERR_ARG;
    }
    /* The seq is consumed from here on, whatever happens: a failed write leaves the slot old or
     * new (unknown), so the index forgets the slot and the seq is never handed out again. */
    log->head = e.seq;
    log->seq[slot] = 0;
    log->key[slot] = 0;
    char key[8];
    (void)dsp_evlog_key(slot, key, sizeof key);
    size_t rlen = 0;
    if (log->kv->set(log->kv->ctx, key, log->buf, len) != DSP_KV_OK ||
        log->kv->commit(log->kv->ctx) != DSP_KV_OK ||
        log->kv->get(log->kv->ctx, key, log->rb, sizeof log->rb, &rlen) != DSP_KV_OK ||
        rlen != len || memcmp(log->buf, log->rb, len) != 0) {
        fault(log);
        return DSP_EVLOG_ERR_WRITE;
    }
    log->seq[slot] = e.seq;
    log->key[slot] = e.key;
    if (seq_out != NULL) {
        *seq_out = e.seq;
    }
    return DSP_EVLOG_OK;
}

size_t dsp_evlog_read(dsp_evlog_t *log, uint32_t before_seq, dsp_event_t *out, size_t max)
{
    if (log == NULL || !log->open || out == NULL) {
        return 0;
    }
    size_t n = 0;
    uint32_t below = before_seq == 0 ? UINT32_MAX : before_seq; /* exclusive bound */
    while (n < max) {
        /* Newest remaining seq below the bound (64 index entries, no allocation). */
        size_t best = DSP_EVLOG_SLOTS;
        for (size_t i = 0; i < DSP_EVLOG_SLOTS; i++) {
            uint32_t s = log->seq[i];
            if (s != 0 && s < below && (best == DSP_EVLOG_SLOTS || s > log->seq[best])) {
                best = i;
            }
        }
        if (best == DSP_EVLOG_SLOTS) {
            break;
        }
        uint32_t want = log->seq[best];
        below = want;
        dsp_event_t e;
        if (read_slot(log, best, &e) != SLOT_OK || e.seq != want) {
            fault(log); /* skipped; the index keeps it (a transient read error may pass) */
            continue;
        }
        out[n++] = e;
    }
    return n;
}

uint32_t dsp_evlog_newest(const dsp_evlog_t *log)
{
    uint32_t m = 0;
    if (log != NULL && log->open) {
        for (size_t i = 0; i < DSP_EVLOG_SLOTS; i++) {
            if (log->seq[i] > m) {
                m = log->seq[i];
            }
        }
    }
    return m;
}

uint32_t dsp_evlog_oldest(const dsp_evlog_t *log)
{
    uint32_t m = 0;
    if (log != NULL && log->open) {
        for (size_t i = 0; i < DSP_EVLOG_SLOTS; i++) {
            if (log->seq[i] != 0 && (m == 0 || log->seq[i] < m)) {
                m = log->seq[i];
            }
        }
    }
    return m;
}
