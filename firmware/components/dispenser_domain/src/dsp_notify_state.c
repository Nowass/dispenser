#include "dsp_notify_state.h"

#include <stddef.h>

static bool instant_ok(dsp_utc_t t)
{
    return t == 0 || (t >= DSP_UTC_MIN && t <= DSP_UTC_MAX);
}

static int severity_of(uint8_t type)
{
    switch (type) {
    case DSP_NOTE_RESERVOIR_LOW:
    case DSP_NOTE_BATTERY_LOW:
        return DSP_SEV_WARNING;
    case DSP_NOTE_RESERVOIR_BLOCKED:
    case DSP_NOTE_DOSING_ERROR:
    case DSP_NOTE_DEVICE_ERROR:
        return DSP_SEV_CRITICAL;
    default:
        return -1;
    }
}

/* Expected condition of a note (DSP_COND_NONE for one-shot events), -1 if invalid. */
static int condition_of(uint8_t type, uint16_t code)
{
    switch (type) {
    case DSP_NOTE_RESERVOIR_LOW:
        return DSP_COND_RESERVOIR_LOW;
    case DSP_NOTE_RESERVOIR_BLOCKED:
        return DSP_COND_RESERVOIR_BLOCKED;
    case DSP_NOTE_BATTERY_LOW:
        return DSP_COND_BATTERY_LOW;
    case DSP_NOTE_DOSING_ERROR:
        return code >= 1 && code <= DSP_DOSEERR_MAX ? DSP_COND_NONE : -1;
    case DSP_NOTE_DEVICE_ERROR:
        switch (code) {
        case DSP_DEVERR_STORAGE:
            return DSP_COND_STORAGE;
        case DSP_DEVERR_TIME_SYNC:
            return DSP_COND_TIME_SYNC;
        case DSP_DEVERR_CLOCK:
            return DSP_COND_CLOCK;
        case DSP_DEVERR_QUEUE_OVERFLOW:
            return DSP_COND_NONE;
        default:
            return -1;
        }
    default:
        return -1;
    }
}

static bool note_empty(const dsp_note_t *e)
{
    return e->seq == 0 && e->type == 0 && e->severity == 0 && e->condition == 0 &&
           e->flags == 0 && e->reminder_no == 0 && e->attempts == 0 && e->created_at == 0 &&
           e->code == 0 && e->value == 0 && e->value2 == 0;
}

static bool note_valid(const dsp_note_t *e, uint32_t next_seq)
{
    if (e->seq == 0) {
        return note_empty(e);
    }
    if (next_seq == 0 || e->seq >= next_seq || severity_of(e->type) != e->severity ||
        condition_of(e->type, e->code) != e->condition || (e->flags & ~DSP_NOTE_F_ALL) != 0) {
        return false;
    }
    bool time_valid = (e->flags & DSP_NOTE_F_TIME_VALID) != 0;
    if (time_valid != (e->created_at != 0) || !instant_ok(e->created_at)) {
        return false;
    }
    if ((e->flags & DSP_NOTE_F_MANUAL) && e->type != DSP_NOTE_DOSING_ERROR) {
        return false;
    }
    bool reminder = (e->flags & DSP_NOTE_F_REMINDER) != 0;
    if (reminder != (e->reminder_no > 0) || (reminder && e->condition == DSP_COND_NONE)) {
        return false; /* reminder flag iff reminder number, only for conditions */
    }
    return true;
}

static bool rejection_empty(const dsp_rejection_t *r)
{
    return r->seq == 0 && r->type == 0 && r->flags == 0 && r->code == 0 && r->http_status == 0 &&
           r->error == 0 && r->field == 0 && r->attempts == 0;
}

static bool rejection_valid(const dsp_rejection_t *r)
{
    return r->seq != 0 && severity_of(r->type) > 0 && condition_of(r->type, r->code) != -1 &&
           (r->flags & ~DSP_NOTE_F_ALL) == 0 && r->http_status >= 400 && r->http_status <= 499 &&
           r->error <= DSP_TXERR_MAX && r->field <= DSP_TXFIELD_MAX && r->attempts >= 1;
}

/* Format-2 additions: diagnostic flags and the rejection log invariants (see header). */
static bool diagnostics_valid(const dsp_notify_state_t *s)
{
    if ((s->tx_diag & ~DSP_TXDIAG_ALL) != 0) {
        return false;
    }
    if (s->tx_diag == 0 && s->tx_diag_status != 0) {
        return false; /* a status is only kept together with a flag */
    }
    if (s->tx_diag_status != 0 && (s->tx_diag_status < 100 || s->tx_diag_status > 599)) {
        return false;
    }
    const dsp_rejection_log_t *log = &s->rejections;
    if (log->count > DSP_REJECTION_LOG_CAPACITY || log->unread > log->count ||
        log->total < log->count) {
        return false;
    }
    /* Unread entries are only lost when the log was full of unread entries. */
    if (log->lost_unread != 0 &&
        (log->count != DSP_REJECTION_LOG_CAPACITY || log->unread != DSP_REJECTION_LOG_CAPACITY)) {
        return false;
    }
    for (size_t i = 0; i < DSP_REJECTION_LOG_CAPACITY; i++) {
        if (i < log->count ? !rejection_valid(&log->entry[i]) : !rejection_empty(&log->entry[i])) {
            return false;
        }
    }
    /* Format 3: report keys, non-zero and unique while used, zero afterwards. */
    if (s->report_key_count > DSP_NOTIFY_REPORT_KEYS) {
        return false;
    }
    for (size_t i = 0; i < DSP_NOTIFY_REPORT_KEYS; i++) {
        if ((i < s->report_key_count) != (s->report_keys[i] != 0)) {
            return false;
        }
        for (size_t j = 0; j < i && i < s->report_key_count; j++) {
            if (s->report_keys[j] == s->report_keys[i]) {
                return false;
            }
        }
    }
    return true;
}

bool dsp_notify_state_is_valid(const dsp_notify_state_t *s)
{
    if (s == NULL || (s->flags & ~DSP_NOTIFY_F_ESTIMATE_UNCERTAIN) != 0 || !diagnostics_valid(s)) {
        return false;
    }
    for (size_t c = 0; c < DSP_COND_COUNT; c++) {
        const dsp_condition_state_t *cs = &s->cond[c];
        uint8_t known = DSP_COND_F_ACTIVE | DSP_COND_F_RAISE_PENDING;
        if ((cs->flags & ~known) != 0 || !instant_ok(cs->opened_at) || !instant_ok(cs->next_reminder_at)) {
            return false;
        }
        if (!(cs->flags & DSP_COND_F_ACTIVE) &&
            (cs->flags != 0 || cs->reminders != 0 || cs->opened_at != 0 || cs->next_reminder_at != 0 ||
             cs->value != 0 || cs->value2 != 0)) {
            return false; /* inactive conditions are fully cleared */
        }
    }
    for (size_t i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (!note_valid(&s->queue[i], s->next_seq) || (s->queue[i].seq != 0 && s->epoch == 0)) {
            return false; /* a queued entry needs an identity (epoch) */
        }
        for (size_t j = 0; j < i && s->queue[i].seq != 0; j++) {
            if (s->queue[j].seq == s->queue[i].seq) {
                return false;
            }
        }
    }
    return true;
}
