#include "dsp_power.h"

#include <string.h>

bool dsp_power_cause_keeps_retained(dsp_wake_cause_t cause)
{
    switch (cause) {
    case DSP_CAUSE_TIMER:
    case DSP_CAUSE_BUTTON:
    case DSP_CAUSE_SOFTWARE:
    case DSP_CAUSE_WATCHDOG:
    case DSP_CAUSE_PANIC:
        return true; /* the CRC still guards against a block torn by the reset */
    default:
        return false; /* power-on, brownout, unknown */
    }
}

/* ---- Retained block ------------------------------------------------------------------- */

void dsp_power_retained_defaults(dsp_power_retained_t *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof *r);
    }
}

static bool instant_ok(dsp_utc_t t)
{
    return t == 0 || (t >= DSP_UTC_MIN && t <= DSP_UTC_MAX);
}

bool dsp_power_retained_is_valid(const dsp_power_retained_t *r)
{
    return r != NULL && instant_ok(r->time.next_attempt) && instant_ok(r->conn.last_notify_attempt) &&
           instant_ok(r->last_wake_at) && r->last_reason <= DSP_WAKE_MAX_SLEEP &&
           r->failsafe_count <= DSP_POWER_FAILSAFE_LIMIT;
}

uint32_t dsp_power_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(u >> (8 * i));
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

static int64_t get_i64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)p[i] << (8 * i);
    }
    return (int64_t)u;
}

/* Layout: 0 magic[4] | 4 version | 5 length | 6 zero[2] | 8 failures u16 | 10 zero[2] |
 * 12 next_attempt i64 | 20 last_notify_attempt i64 | 28 wake_count u32 | 32 failsafe u8 |
 * 33 last_reason u8 | 34 zero[2] | 36 last_wake_at i64 | 44 crc32 */
static const uint8_t MAGIC[4] = {'D', 'S', 'P', 'R'};

bool dsp_power_retained_encode(const dsp_power_retained_t *r, uint8_t out[DSP_POWER_RETAINED_BYTES])
{
    if (out == NULL || !dsp_power_retained_is_valid(r)) {
        return false;
    }
    memset(out, 0, DSP_POWER_RETAINED_BYTES);
    memcpy(out, MAGIC, 4);
    out[4] = DSP_POWER_RETAINED_VERSION;
    out[5] = DSP_POWER_RETAINED_BYTES;
    put_u16(out + 8, r->time.consecutive_failures);
    put_i64(out + 12, r->time.next_attempt);
    put_i64(out + 20, r->conn.last_notify_attempt);
    put_u32(out + 28, r->wake_count);
    out[32] = r->failsafe_count;
    out[33] = r->last_reason;
    put_i64(out + 36, r->last_wake_at);
    put_u32(out + 44, dsp_power_crc32(out, 44));
    return true;
}

bool dsp_power_retained_decode(const uint8_t *buf, size_t len, dsp_power_retained_t *out)
{
    if (buf == NULL || out == NULL || len != DSP_POWER_RETAINED_BYTES ||
        memcmp(buf, MAGIC, 4) != 0 || buf[4] != DSP_POWER_RETAINED_VERSION ||
        buf[5] != DSP_POWER_RETAINED_BYTES || buf[6] != 0 || buf[7] != 0 || buf[10] != 0 ||
        buf[11] != 0 || buf[34] != 0 || buf[35] != 0 ||
        get_u32(buf + 44) != dsp_power_crc32(buf, 44)) {
        return false;
    }
    dsp_power_retained_t r;
    memset(&r, 0, sizeof r);
    r.time.consecutive_failures = get_u16(buf + 8);
    r.time.next_attempt = get_i64(buf + 12);
    r.conn.last_notify_attempt = get_i64(buf + 20);
    r.wake_count = get_u32(buf + 28);
    r.failsafe_count = buf[32];
    r.last_reason = buf[33];
    r.last_wake_at = get_i64(buf + 36);
    if (!dsp_power_retained_is_valid(&r)) {
        return false;
    }
    *out = r;
    return true;
}

/* ---- Plan ------------------------------------------------------------------------------ */

int64_t dsp_power_max_sleep_s(int64_t configured)
{
    if (configured <= 0) {
        return DSP_POWER_MAX_SLEEP_DEFAULT_S;
    }
    if (configured < DSP_POWER_MAX_SLEEP_MIN_S) {
        return DSP_POWER_MAX_SLEEP_MIN_S;
    }
    return configured > DSP_POWER_MAX_SLEEP_MAX_S ? DSP_POWER_MAX_SLEEP_MAX_S : configured;
}

dsp_wake_plan_t dsp_power_plan(const dsp_power_inputs_t *in, dsp_utc_t now)
{
    dsp_wake_inputs_t w;
    memset(&w, 0, sizeof w);
    int64_t cap = dsp_power_max_sleep_s(in != NULL ? in->max_sleep_s : 0);
    if (in != NULL) {
        w.has_dose = in->dose_known;
        w.dose_at = in->dose_at;
        w.has_sync = in->sync_known;
        w.sync_at = in->sync_known && in->sync_at <= now ? now + DSP_POWER_SYNC_DEFER_S : in->sync_at;
    }
    return dsp_wake_plan(&w, now, cap);
}
