#ifndef DSP_CODEC_H
#define DSP_CODEC_H

/*
 * Serialized record formats (milestone M1.3). Byte-exact, little-endian, explicit field
 * sizes, no dependence on C struct layout. Every record starts with
 *   byte 0: record type (DSP_REC_TYPE_*), byte 1: format version (1 = this layout).
 * Decoders are strict: exact length, known type and version, every field validated,
 * padding bytes must be zero. Encoders refuse invalid input (never write a record that
 * would not decode). Full byte layouts: firmware/README.md "Persistent data format".
 *
 * No application-level checksum: NVS already protects every entry and every blob chunk
 * with CRC32 (and drops items that fail it, which the explicit-presence rule in
 * dsp_storage.h turns into a detected corruption). Structural validation here catches
 * truncated, mis-sized, foreign or newer-format data.
 */

#include <stddef.h>
#include <stdint.h>

#include "dsp_config.h"
#include "dsp_history.h"
#include "dsp_schedule_set.h"
#include "dsp_time.h"
#include "dsp_notify_state.h"
#include "dsp_dosing_state.h"
#include "dsp_time_meta.h"
#include "dsp_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSP_REC_TYPE_LAYOUT = 1,
    DSP_REC_TYPE_HISTORY = 2,
    DSP_REC_TYPE_SCHEDULE_SET = 3,
    DSP_REC_TYPE_CONFIG = 4,
    DSP_REC_TYPE_TIME = 5,   /* layout v2 (M1.4) */
    DSP_REC_TYPE_NOTIFY = 6, /* layout v3 (M1.5) */
    DSP_REC_TYPE_DOSING = 7, /* layout v4 (M1.9a) */
    DSP_REC_TYPE_AUTH = 8,   /* layout v5 (M1.10b.3) */
};

#define DSP_CODEC_VERSION 1 /* format version written by this firmware for every type */

#define DSP_LAYOUT_RECORD_BYTES 7
/* History slot: format 2 (M1.9a, 96 B) = format 1 (92 B) + claimed volume (u32). */
#define DSP_HISTORY_RECORD_V1_BYTES 92
#define DSP_HISTORY_RECORD_BYTES 96
#define DSP_HISTORY_FORMAT_V2 2
/* Schedule set: format 2 (M1.10b.1, 132 B entries) = format 1 (91 B) + name (1 + 32 B) +
 * edit floor (i64). Format 1 is still read (empty names, no floor) and upgraded by the next
 * write; format 2 is the only one written. */
#define DSP_SCHEDULE_ENTRY_V1_BYTES 91
#define DSP_SCHEDULE_ENTRY_BYTES 132
#define DSP_SCHEDULE_SET_FORMAT_V2 2
#define DSP_TOMBSTONE_ENTRY_BYTES 65
#define DSP_SCHEDULE_SET_HEADER_BYTES 8
#define DSP_SCHEDULE_SET_MAX_BYTES                                                           \
    (DSP_SCHEDULE_SET_HEADER_BYTES + DSP_SCHEDULE_CAPACITY * DSP_SCHEDULE_ENTRY_BYTES +      \
     DSP_TOMBSTONE_CAPACITY * DSP_TOMBSTONE_ENTRY_BYTES)
/* Configuration: format 2 (M1.9a, 46 B) = format 1 (18 B) + calibration and limits. */
#define DSP_CONFIG_RECORD_V1_BYTES 18
#define DSP_CONFIG_RECORD_BYTES 46
#define DSP_CONFIG_FORMAT_V2 2
/* Dosing state (layout v4, M1.9a): format 3 (M1.10b.3, 212 B) = format 2 (M1.10b.2, 206 B) +
 * manual-dose admission date and count; format 2 = format 1 (52 B) + anti-replay state. Older
 * formats are still read (missing fields 0) and upgraded by the next write. */
#define DSP_DOSING_RECORD_V1_BYTES 52
#define DSP_DOSING_RECORD_V2_BYTES 206
#define DSP_DOSING_RECORD_BYTES 212
#define DSP_DOSING_FORMAT_V2 2
#define DSP_DOSING_FORMAT_V3 3
#define DSP_DOSING_RING_ENTRY_BYTES 14
/* Credential record (layout v5, M1.10b.3): format 1, 96 B header + 4 x 80 B slots. */
#define DSP_AUTH_RECORD_BYTES 416
#define DSP_AUTH_SLOT_BYTES 80
#define DSP_TIME_RECORD_BYTES 42
#define DSP_NOTIFY_HEADER_BYTES 14
#define DSP_NOTIFY_COND_BYTES 26
#define DSP_NOTIFY_ENTRY_BYTES 32
/* Format 1 (M1.5, 682 B): still decoded (as "no diagnostics"), never written. */
#define DSP_NOTIFY_RECORD_V1_BYTES                                                           \
    (DSP_NOTIFY_HEADER_BYTES + DSP_COND_COUNT * DSP_NOTIFY_COND_BYTES +                      \
     DSP_NOTIFY_QUEUE_CAPACITY * DSP_NOTIFY_ENTRY_BYTES)
/* Format 2 (M1.7, 758 B) = format 1 + diagnostics (4 B) + rejection log header (8 B) +
 * 4 rejection entries x 16 B. Format 3 (M1.9a, 794 B) = format 2 + report keys (4 B header
 * + 4 x 8 B). Format 3 is the only format written. */
#define DSP_NOTIFY_FORMAT_V1 1
#define DSP_NOTIFY_FORMAT_V2 2
#define DSP_NOTIFY_FORMAT_V3 3
#define DSP_NOTIFY_RECORD_V2_BYTES                                                           \
    (DSP_NOTIFY_RECORD_V1_BYTES + DSP_NOTIFY_DIAG_BYTES + DSP_NOTIFY_REJLOG_HEADER_BYTES +   \
     DSP_REJECTION_LOG_CAPACITY * DSP_NOTIFY_REJECTION_BYTES)
#define DSP_NOTIFY_REPORT_KEYS_BYTES (4 + DSP_NOTIFY_REPORT_KEYS * 8)
#define DSP_NOTIFY_DIAG_BYTES 4
#define DSP_NOTIFY_REJLOG_HEADER_BYTES 8
#define DSP_NOTIFY_REJECTION_BYTES 16
#define DSP_NOTIFY_RECORD_BYTES (DSP_NOTIFY_RECORD_V2_BYTES + DSP_NOTIFY_REPORT_KEYS_BYTES)
#define DSP_RECORD_MAX_BYTES DSP_SCHEDULE_SET_MAX_BYTES

typedef enum {
    DSP_CODEC_OK = 0,
    DSP_CODEC_EMPTY,       /* history slot holding the explicit EMPTY record */
    DSP_CODEC_ERR_SIZE,    /* wrong length, or output buffer too small */
    DSP_CODEC_ERR_TYPE,    /* record of another type */
    DSP_CODEC_ERR_VERSION, /* unknown (e.g. newer) format version: unsupported */
    DSP_CODEC_ERR_FIELD,   /* malformed or invalid content */
} dsp_codec_status_t;

/* Layout marker: describes the storage layout; written last during provisioning. */
typedef struct {
    uint16_t layout_version; /* 1 */
    uint8_t history_slots;   /* DSP_HISTORY_DEFAULT_SLOTS */
    uint8_t schedule_capacity;
    uint8_t tombstone_capacity;
} dsp_layout_t;

/* Encoders return the number of bytes written, or 0 for invalid input / small buffer. */
size_t dsp_codec_encode_layout(const dsp_layout_t *layout, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_layout(const uint8_t *buf, size_t len, dsp_layout_t *out);

/* rec == NULL encodes the explicit EMPTY slot record. Decoding EMPTY returns
 * DSP_CODEC_EMPTY and leaves *out untouched. Format 2 (96 B, M1.9a) is written; format 1
 * (92 B, M1.2) is still read, with volume_ul = 0 ("unknown"). */
size_t dsp_codec_encode_history(const dsp_history_record_t *rec, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_history(const uint8_t *buf, size_t len,
                                            dsp_history_record_t *out);

/* Every schedule must pass dsp_schedule_validate(tz); IDs (active and tombstones) must be
 * unique across the whole set. Format 2 entry (132 B): format 1 entry (91 B) | 91 name
 * length | 92..123 name (zero padded) | 124..131 not_before (i64). */
size_t dsp_codec_encode_schedule_set(const dsp_schedule_set_t *set, const dsp_tz_t *tz,
                                     uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_schedule_set(const uint8_t *buf, size_t len,
                                                 const dsp_tz_t *tz, dsp_schedule_set_t *out);

/* Format 2 (46 B, M1.9a) is written: format 1 + 18..21 pump_flow_ul_per_s | 22..29
 * calibrated_at (i64) | 30..33 max_dose_ul | 34..37 max_daily_ul | 38..41 max_runtime_ms |
 * 42..45 min_interval_s. Format 1 (18 B) is still read, with these fields 0. */
size_t dsp_codec_encode_config(const dsp_config_t *cfg, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_config(const uint8_t *buf, size_t len, dsp_config_t *out);

/* time (42 B): 0 type | 1 version | 2..9 last_sync | 10..17 high_water | 18..25 hold_until |
 * 26..33 last_correction | 34..41 not_before (all i64). */
size_t dsp_codec_encode_time(const dsp_time_meta_t *meta, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_time(const uint8_t *buf, size_t len, dsp_time_meta_t *out);

/* notify, format 3 (794 B; format 2 = the first 758 B, format 1 = the first 682 B, with
 * the matching version byte):
 * header 0 type | 1 version | 2..5 epoch | 6..9 next_seq | 10..11 dropped |
 * 12 flags | 13 zero; then 6 conditions x 26 B (flags, reminders, opened_at i64,
 * next_reminder_at i64, value u32, value2 u32); then 16 entries x 32 B (0..3 seq | 4 type | 5 severity |
 * 6 condition | 7 flags | 8 reminder_no | 9 zero | 10..11 attempts | 12..19 created_at |
 * 20..21 code | 22..25 value | 26..29 value2 | 30..31 zero);
 * format 2 only, at 682: diagnostics 0 tx_diag | 1 zero | 2..3 tx_diag_status;
 * at 686: rejection log 0..3 total | 4..5 lost_unread | 6 unread | 7 count;
 * at 694: 4 rejections x 16 B (0..3 seq | 4 type | 5 flags | 6..7 code | 8..9 http_status |
 * 10 error | 11 field | 12..13 attempts | 14..15 zero).
 * format 3 only, at 758: report keys 0 count | 1..3 zero | 4.. 4 x u64 key (unused: zero).
 * The decoder accepts formats 1, 2 (missing parts zero) and 3; the encoder writes 3.
 * Forward-only: older firmware reports a newer format as unsupported. */
/* dosing, format 3 (212 B, M1.10b.3) = format 2 | 206..209 adm_date (year u16, month, day; all
 * zero = none) | 210..211 adm_count (u16). The decoder accepts formats 1, 2 and 3.
 * dosing, format 2 (206 B, M1.10b.2) = format 1 | 52..83 manual_hw[4] (u64) | 84 marker_slot |
 * 85 ring_next | 86..93 marker_req (u64) | 94..205 8 x ring entry (0 slot | 1 state | 2..9 seq u64
 * | 10..13 detail); unused ring entries all zero.
 * dosing format 1 (52 B, layout v4, M1.9a): 0 type | 1 version | 2 readiness | 3 flags |
 * 4..7 refill_capacity | 8..11 refill_remaining | 12..15 refill_seq | 16..19 manual_volume |
 * 20..27 manual_started_at (i64) | 28..31 manual_seq | 32..35 ledger_date (year u16, month,
 * day) | 36..39 ledger_sum | 40..47 last_start (i64) | 48..51 next_seq. */
size_t dsp_codec_encode_dosing(const dsp_dosing_state_t *s, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_dosing(const uint8_t *buf, size_t len, dsp_dosing_state_t *out);

/* auth, format 1 (416 B, layout v5, M1.10b.3): 0 type (8) | 1 format | 2 claim_state |
 * 3 flags (bit 0 KEY_FAULT) | 4..7 owner_epoch | 8..39 identity private key | 40..71 identity
 * public key | 72..79 claimed_at (i64) | 80..87 recovered_at (i64) | 88..91 recoveries |
 * 92..95 failed_sessions | 96.. 4 x slot (80 B: 0 used | 1 name_len | 2..3 zero | 4..35 phone
 * public key | 36..59 name, zero padded | 60..63 zero | 64..71 enrolled_at | 72..79 last_seen).
 * Strict (dsp_auth_is_valid). The encoded buffer holds the private key: callers wipe it. */
size_t dsp_codec_encode_auth(const dsp_auth_t *a, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_auth(const uint8_t *buf, size_t len, dsp_auth_t *out);

size_t dsp_codec_encode_notify(const dsp_notify_state_t *s, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_notify(const uint8_t *buf, size_t len, dsp_notify_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CODEC_H */
