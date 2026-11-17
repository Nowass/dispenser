#ifndef DSP_STORAGE_H
#define DSP_STORAGE_H

/*
 * Persistent storage layout v1 over a dsp_kv_t (milestone M1.3). Pure domain code.
 *
 * Keys (one NVS namespace, see firmware/README.md "Storage layout"):
 *   "layout"        layout marker, written LAST during provisioning
 *   "h00".."h31"    execution history slots (M1.2 record or explicit EMPTY record)
 *   "sched"         the whole schedule set (one atomic record)
 *   "config"        device configuration
 *   "time"          time metadata (layout v2, M1.4)
 *   "notify"        notification state and queue (layout v3, M1.5)
 *   "dosing"        dosing state: readiness, BLOCKED latch, refill intent, manual marker,
 *                   daily ledger (layout v4, M1.9a)
 *   "auth"          credential record: identity key pair, phone slots, ownership epoch
 *                   (layout v5, M1.10b.3)
 *
 * Explicit presence (approved 2026-11-15): every key exists once provisioned and keys are
 * never deleted. Because ESP-IDF NVS drops items that fail its CRC checks, a missing key
 * after provisioning means corruption, never "empty". Corruption, unsupported formats and
 * inconsistencies are reported; this code never erases or rewrites data to "repair" it.
 *
 * Every write is: encode, set one key, commit, read back, compare bytes. Only single-key
 * atomicity is relied upon.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_codec.h"
#include "dsp_config.h"
#include "dsp_history.h"
#include "dsp_kv.h"
#include "dsp_time.h"
#include "dsp_notify_state.h"
#include "dsp_time_meta.h"
#include "dsp_dosing_state.h"
#include "dsp_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_STORAGE_LAYOUT_VERSION 5 /* v1..v4 are migrated: new keys added, marker last */
#define DSP_STORAGE_HISTORY_SLOTS DSP_HISTORY_DEFAULT_SLOTS /* 32, fixed in layout v1 */

typedef enum {
    DSP_STORAGE_OK = 0,
    DSP_STORAGE_ERR_ARG,
    DSP_STORAGE_ERR_NOT_OPEN,        /* storage not opened (or open failed) */
    DSP_STORAGE_ERR_NOT_PROVISIONED, /* fresh storage and provisioning not allowed */
    DSP_STORAGE_ERR_READ,
    DSP_STORAGE_ERR_WRITE,
    DSP_STORAGE_ERR_NO_SPACE,        /* NVS partition full */
    DSP_STORAGE_ERR_COMMIT,
    DSP_STORAGE_ERR_CONFIRM,         /* read-back after commit failed or differs */
    DSP_STORAGE_ERR_CORRUPT,         /* missing key, malformed record, inconsistency */
    DSP_STORAGE_ERR_UNSUPPORTED,     /* unknown/newer record format or layout */
    DSP_STORAGE_ERR_INVALID,         /* input fails validation */
    DSP_STORAGE_ERR_REVISION,        /* revision conflict (optimistic concurrency) */
    DSP_STORAGE_ERR_NOT_FOUND,       /* schedule ID not present */
    DSP_STORAGE_ERR_DUPLICATE_ID,    /* ID of an existing schedule */
    DSP_STORAGE_ERR_ID_REUSED,       /* ID of a deleted / previously executed schedule */
    DSP_STORAGE_ERR_CAPACITY,        /* schedule capacity reached */
    DSP_STORAGE_ERR_RELOAD_REQUIRED, /* a write failed; durable state unknown until reload */
    DSP_STORAGE_ERR_MIGRATION_REQUIRED, /* layout v1 found and writes not allowed */
} dsp_storage_status_t;

/* Caller-owned storage context (about 8 KB: two record buffers and one decoded schedule
 * set used for validation). Single owner; no hidden static state. */
typedef struct {
    const dsp_kv_t *kv;
    const dsp_tz_t *tz;
    bool open;
    bool provisioned_now; /* this open() provisioned a fresh storage */
    bool migrated_now;    /* this open() migrated layout v1 to v2 */
    uint8_t buf[DSP_RECORD_MAX_BYTES];
    uint8_t readback[DSP_RECORD_MAX_BYTES];
    dsp_schedule_set_t set_scratch;
    dsp_notify_state_t notify_scratch;
    /* M1.10b.3: classification of "auth" at open (DSP_STORAGE_OK, ERR_CORRUPT or
     * ERR_UNSUPPORTED). With a v5 marker a missing, malformed or newer "auth" does NOT fail the
     * open (the other records stay usable so recovery can force SERVICE_REQUIRED), but the
     * caller MUST treat it as CRED_DEGRADED: no dosing until recovery or maintenance. */
    dsp_storage_status_t auth_status;
    dsp_auth_t auth_scratch; /* transition copy (key material; wiped after use) */
} dsp_storage_t;

/*
 * Opens the layout.
 *  - Marker v4 (M1.9a..M1.10b.2): migrated to v5 if allow_provision: the "auth" record is
 *    provisioned UNCLAIMED (a v4 device was never claimable) unless an interrupted migration
 *    already wrote that value; any other "auth" content next to a v4 marker is CORRUPT. Then
 *    the marker is rewritten last (resumable).
 *  - Marker v1 (M1.3), v2 (M1.4) or v3 (M1.5): migrated to v5 if allow_provision (keys added
 *    since are provisioned, marker rewritten last; resumable), otherwise
 *    DSP_STORAGE_ERR_MIGRATION_REQUIRED. The new "dosing" record is provisioned
 *    SERVICE_REQUIRED (never enabling dosing), with the BLOCKED latch set when the existing
 *    notification state has an active RESERVOIR_BLOCKED condition (conservative).
 *  - Marker present and supported: every key is read and validated (history also through
 *    dsp_history_scan). Any missing or malformed key -> DSP_STORAGE_ERR_CORRUPT; a newer
 *    format -> DSP_STORAGE_ERR_UNSUPPORTED. Nothing is modified. Exception (v5): "auth" is
 *    classified into st->auth_status instead of failing the open (see dsp_storage_t).
 *  - Marker absent: if every other key is absent or still holds its provisioning value
 *    (first boot, or provisioning interrupted by power loss), the missing keys are written,
 *    then the marker (if allow_provision; otherwise DSP_STORAGE_ERR_NOT_PROVISIONED).
 *    If any key holds real data, the marker was lost: DSP_STORAGE_ERR_CORRUPT.
 * On any error st->open stays false and automatic dosing must not run.
 */
dsp_storage_status_t dsp_storage_open(dsp_storage_t *st, const dsp_kv_t *kv, const dsp_tz_t *tz,
                                      bool allow_provision);

/* NVS key of a history slot: "h00".."h31". buf must hold 4 bytes. */
bool dsp_storage_history_key(size_t slot, char *buf, size_t cap);

/* Binds a dsp_history_store_t (M1.2 contract) to this storage. A missing slot key reads
 * as DSP_STORE_ERR_READ, never as empty; malformed or unsupported records likewise. */
void dsp_storage_history_store(dsp_storage_t *st, dsp_history_store_t *out);

dsp_storage_status_t dsp_storage_load_config(dsp_storage_t *st, dsp_config_t *out);

/* Validates cfg, requires cfg->revision == stored revision + 1, writes and confirms. */
dsp_storage_status_t dsp_storage_save_config(dsp_storage_t *st, const dsp_config_t *cfg);

/* Time metadata (M1.4). save validates and writes only the given record (caller writes
 * only when the metadata changed, to limit flash wear). */
dsp_storage_status_t dsp_storage_load_time(dsp_storage_t *st, dsp_time_meta_t *out);
dsp_storage_status_t dsp_storage_save_time(dsp_storage_t *st, const dsp_time_meta_t *meta);

/* Notification state (M1.5): one atomic record; save validates and confirms. */
dsp_storage_status_t dsp_storage_load_notify(dsp_storage_t *st, dsp_notify_state_t *out);
dsp_storage_status_t dsp_storage_save_notify(dsp_storage_t *st, const dsp_notify_state_t *s);

/* Dosing state (M1.9a, layout v4): one atomic record; save validates and confirms. */
dsp_storage_status_t dsp_storage_load_dosing(dsp_storage_t *st, dsp_dosing_state_t *out);
dsp_storage_status_t dsp_storage_save_dosing(dsp_storage_t *st, const dsp_dosing_state_t *s);

/* Credential record (M1.10b.3, layout v5). load: strict decode into *out (the caller wipes
 * it); st->buf is wiped. save: validates, writes, reads back, compares; both buffers wiped.
 * A successful save also sets st->auth_status = OK. */
dsp_storage_status_t dsp_storage_load_auth(dsp_storage_t *st, dsp_auth_t *out);
dsp_storage_status_t dsp_storage_save_auth(dsp_storage_t *st, const dsp_auth_t *a);

/* ---- Internal helpers shared with the schedule repository ------------------------- */

/* Reads key into st->buf. Missing key -> CORRUPT (explicit presence). */
dsp_storage_status_t dsp_storage_get(dsp_storage_t *st, const char *key, size_t *len);

/* Writes st->buf[0..len) to key, commits, reads back and compares. */
dsp_storage_status_t dsp_storage_put(dsp_storage_t *st, const char *key, size_t len);

/* Maps a codec decode error: VERSION -> UNSUPPORTED, anything else -> CORRUPT. */
dsp_storage_status_t dsp_storage_from_codec(dsp_codec_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* DSP_STORAGE_H */
