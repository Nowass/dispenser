#include "dsp_power.h"

#include "dsp_auth.h" /* dsp_secure_zero */

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

static bool raw_ok(dsp_utc_t t)
{
    return t >= 0 && t <= DSP_UTC_MAX;
}

static bool bytes_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

bool dsp_power_overlays_consistent(const dsp_power_retained_t *r)
{
    if (r == NULL) {
        return false;
    }
    const dsp_invite_retained_t *i = &r->invite;
    if (i->valid == 0) {
        if (i->creator_slot != 0 || i->owner_epoch != 0 || !bytes_zero(i->token, sizeof i->token) ||
            i->created_raw != 0 || i->deadline_raw != 0) {
            return false;
        }
    } else if (i->valid != 1 || i->creator_slot >= 4 || i->owner_epoch == 0 ||
               i->owner_epoch == DSP_OWNER_EPOCH_DEGRADED || bytes_zero(i->token, sizeof i->token) ||
               !raw_ok(i->created_raw) || !raw_ok(i->deadline_raw) ||
               i->deadline_raw - i->created_raw != DSP_OWNER_INVITE_S) {
        return false;
    }
    const dsp_recovery_retained_t *a = &r->arm;
    if (a->armed == 0) {
        if (a->aborts != 0 || a->owner_epoch != 0 || a->armed_raw != 0 || a->deadline_raw != 0) {
            return false;
        }
    } else if (a->armed != 1 || a->aborts > 2 || a->owner_epoch == 0 || !raw_ok(a->armed_raw) ||
               !raw_ok(a->deadline_raw) || a->deadline_raw - a->armed_raw != DSP_OWNER_ARM_S) {
        return false;
    }
    return true;
}

void dsp_power_overlays_clear(dsp_power_retained_t *r)
{
    if (r != NULL) {
        memset(&r->invite, 0, sizeof r->invite);
        memset(&r->arm, 0, sizeof r->arm);
    }
}

bool dsp_power_retained_is_valid(const dsp_power_retained_t *r)
{
    return r != NULL && instant_ok(r->time.next_attempt) && instant_ok(r->conn.last_notify_attempt) &&
           instant_ok(r->last_wake_at) && r->last_reason <= DSP_WAKE_MAX_SLEEP &&
           r->failsafe_count <= DSP_POWER_FAILSAFE_LIMIT && raw_ok(r->saved_raw) &&
           dsp_power_overlays_consistent(r);
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

/* Layout (version 2): 0 magic[4] | 4 version | 5 length | 6 zero[2] | 8 failures u16 |
 * 10 zero[2] | 12 next_attempt i64 | 20 last_notify_attempt i64 | 28 wake_count u32 |
 * 32 failsafe u8 | 33 last_reason u8 | 34 zero[2] | 36 last_wake_at i64 | 44 saved_raw i64 |
 * 52 invite valid | 53 creator | 54 zero[2] | 56 invite epoch u32 | 60 token[16] |
 * 76 created_raw i64 | 84 deadline_raw i64 | 92 armed | 93 aborts | 94 zero[2] |
 * 96 arm epoch u32 | 100 armed_raw i64 | 108 deadline_raw i64 | 116 crc32 */
#define CRC_AT 116
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
    put_i64(out + 44, r->saved_raw);
    out[52] = r->invite.valid;
    out[53] = r->invite.creator_slot;
    put_u32(out + 56, r->invite.owner_epoch);
    memcpy(out + 60, r->invite.token, sizeof r->invite.token);
    put_i64(out + 76, r->invite.created_raw);
    put_i64(out + 84, r->invite.deadline_raw);
    out[92] = r->arm.armed;
    out[93] = r->arm.aborts;
    put_u32(out + 96, r->arm.owner_epoch);
    put_i64(out + 100, r->arm.armed_raw);
    put_i64(out + 108, r->arm.deadline_raw);
    put_u32(out + CRC_AT, dsp_power_crc32(out, CRC_AT));
    return true;
}

bool dsp_power_retained_decode(const uint8_t *buf, size_t len, dsp_power_retained_t *out)
{
    if (buf == NULL || out == NULL || len != DSP_POWER_RETAINED_BYTES ||
        memcmp(buf, MAGIC, 4) != 0 || buf[4] != DSP_POWER_RETAINED_VERSION ||
        buf[5] != DSP_POWER_RETAINED_BYTES || buf[6] != 0 || buf[7] != 0 || buf[10] != 0 ||
        buf[11] != 0 || buf[34] != 0 || buf[35] != 0 || buf[54] != 0 || buf[55] != 0 ||
        buf[94] != 0 || buf[95] != 0 || get_u32(buf + CRC_AT) != dsp_power_crc32(buf, CRC_AT)) {
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
    r.saved_raw = get_i64(buf + 44);
    r.invite.valid = buf[52];
    r.invite.creator_slot = buf[53];
    r.invite.owner_epoch = get_u32(buf + 56);
    memcpy(r.invite.token, buf + 60, sizeof r.invite.token);
    r.invite.created_raw = get_i64(buf + 76);
    r.invite.deadline_raw = get_i64(buf + 84);
    r.arm.armed = buf[92];
    r.arm.aborts = buf[93];
    r.arm.owner_epoch = get_u32(buf + 96);
    r.arm.armed_raw = get_i64(buf + 100);
    r.arm.deadline_raw = get_i64(buf + 108);
    if (!dsp_power_overlays_consistent(&r)) {
        /* CRC is not authentication: inconsistent overlays are never trusted, but they do not
         * discard the fail-safe counter or the sync backoff. */
        dsp_power_overlays_clear(&r);
        r.overlays_dropped = true;
    }
    if (!dsp_power_retained_is_valid(&r)) {
        dsp_secure_zero(r.invite.token, sizeof r.invite.token);
        return false;
    }
    *out = r;
    dsp_secure_zero(r.invite.token, sizeof r.invite.token);
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
