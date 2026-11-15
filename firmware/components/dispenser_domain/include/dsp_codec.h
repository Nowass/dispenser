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
#include "dsp_time_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSP_REC_TYPE_LAYOUT = 1,
    DSP_REC_TYPE_HISTORY = 2,
    DSP_REC_TYPE_SCHEDULE_SET = 3,
    DSP_REC_TYPE_CONFIG = 4,
    DSP_REC_TYPE_TIME = 5, /* layout v2 (M1.4) */
};

#define DSP_CODEC_VERSION 1 /* format version written by this firmware for every type */

#define DSP_LAYOUT_RECORD_BYTES 7
#define DSP_HISTORY_RECORD_BYTES 92
#define DSP_SCHEDULE_ENTRY_BYTES 91
#define DSP_TOMBSTONE_ENTRY_BYTES 65
#define DSP_SCHEDULE_SET_HEADER_BYTES 8
#define DSP_SCHEDULE_SET_MAX_BYTES                                                           \
    (DSP_SCHEDULE_SET_HEADER_BYTES + DSP_SCHEDULE_CAPACITY * DSP_SCHEDULE_ENTRY_BYTES +      \
     DSP_TOMBSTONE_CAPACITY * DSP_TOMBSTONE_ENTRY_BYTES)
#define DSP_CONFIG_RECORD_BYTES 18
#define DSP_TIME_RECORD_BYTES 42
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
 * DSP_CODEC_EMPTY and leaves *out untouched. */
size_t dsp_codec_encode_history(const dsp_history_record_t *rec, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_history(const uint8_t *buf, size_t len,
                                            dsp_history_record_t *out);

/* Every schedule must pass dsp_schedule_validate(tz); IDs (active and tombstones) must be
 * unique across the whole set. */
size_t dsp_codec_encode_schedule_set(const dsp_schedule_set_t *set, const dsp_tz_t *tz,
                                     uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_schedule_set(const uint8_t *buf, size_t len,
                                                 const dsp_tz_t *tz, dsp_schedule_set_t *out);

size_t dsp_codec_encode_config(const dsp_config_t *cfg, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_config(const uint8_t *buf, size_t len, dsp_config_t *out);

/* time (42 B): 0 type | 1 version | 2..9 last_sync | 10..17 high_water | 18..25 hold_until |
 * 26..33 last_correction | 34..41 not_before (all i64). */
size_t dsp_codec_encode_time(const dsp_time_meta_t *meta, uint8_t *buf, size_t cap);
dsp_codec_status_t dsp_codec_decode_time(const uint8_t *buf, size_t len, dsp_time_meta_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CODEC_H */
