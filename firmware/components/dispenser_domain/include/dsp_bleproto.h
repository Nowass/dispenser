#ifndef DSP_BLEPROTO_H
#define DSP_BLEPROTO_H

/*
 * BLE protocol v1 codec (milestone M1.10a), normative specification docs/BLE_PROTOCOL.md,
 * shared vectors test-vectors/ble_protocol_v1.txt. Pure C11, no allocation, no I/O.
 *
 *  - Frames: 8-byte header + body; plain bodies are decoded/encoded from the layout tables
 *    (every rule of the specification: lengths, bools, enumerations, masks, reserved bytes,
 *    dates, strings incl. UTF-8 well-formedness, list counts, forward-compatible trailing bytes).
 *  - ENCRYPTED frames: only the header is validated here; the body (ciphertext || tag) is
 *    opaque. The session layer (M1.10c) decrypts it and calls dsp_ble_decode_body().
 *  - Fragmentation and a bounded reassembler (one partial frame, 2,000 ms timeout).
 *  - Version negotiation, the public Info record and the advertising Service Data payload.
 * Semantics (schedule rules, gates, authorization) are not decided here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_BLE_MAJOR 1
#define DSP_BLE_MINOR 0
#define DSP_BLE_HEADER_LEN 8
#define DSP_BLE_FRAME_MAX 520
#define DSP_BLE_BODY_MAX (DSP_BLE_FRAME_MAX - DSP_BLE_HEADER_LEN) /* 512 */
#define DSP_BLE_PLAIN_BODY_MAX 496
#define DSP_BLE_TAG_LEN 16
#define DSP_BLE_FRAG_MAX 64
#define DSP_BLE_ATT_MTU_MIN 23
#define DSP_BLE_REASM_TIMEOUT_MS 2000u
#define DSP_BLE_STR_MAX 64
#define DSP_BLE_LIST_MAX 4
#define DSP_BLE_INFO_LEN 32
#define DSP_BLE_ADVERT_LEN 4
#define DSP_BLE_FACTORY_RESET_CONFIRM 0x54455352u /* "RSET" */

enum {
    DSP_BLE_F_RESPONSE = 1u << 0,
    DSP_BLE_F_ERROR = 1u << 1,
    DSP_BLE_F_EVENT = 1u << 2,
    DSP_BLE_F_ENCRYPTED = 1u << 3,
};

typedef enum {
    DSP_BLE_T_HELLO = 0x01,
    DSP_BLE_T_HS = 0x02,
    DSP_BLE_T_ENROLL_RESULT = 0x08,
    DSP_BLE_T_GET_STATUS = 0x10,
    DSP_BLE_T_GET_DIAGNOSTICS = 0x11,
    DSP_BLE_T_ACK_REJECTIONS = 0x12,
    DSP_BLE_T_GET_BATTERY = 0x13, /* firmware >= 0.2.0 (docs/BLE_PROTOCOL.md 7.7) */
    DSP_BLE_T_GET_CONFIG = 0x20,
    DSP_BLE_T_SET_CONFIG = 0x21,
    DSP_BLE_T_LIST_SCHEDULES = 0x30,
    DSP_BLE_T_GET_SCHEDULE = 0x31,
    DSP_BLE_T_CREATE_SCHEDULE = 0x32,
    DSP_BLE_T_UPDATE_SCHEDULE = 0x33,
    DSP_BLE_T_DELETE_SCHEDULE = 0x34,
    DSP_BLE_T_MANUAL_DOSE = 0x40,
    DSP_BLE_T_GET_RESULT = 0x41,
    DSP_BLE_T_REFILL = 0x42,
    DSP_BLE_T_SET_READINESS = 0x43,
    DSP_BLE_T_SET_TIME = 0x50,
    DSP_BLE_T_WIFI_SET = 0x60,
    DSP_BLE_T_WIFI_GET = 0x61,
    DSP_BLE_T_WIFI_CLEAR = 0x62,
    DSP_BLE_T_PHONES_LIST = 0x70,
    DSP_BLE_T_PHONE_REMOVE = 0x71,
    DSP_BLE_T_INVITE_CREATE = 0x72,
    DSP_BLE_T_INVITE_CANCEL = 0x73,
    DSP_BLE_T_RECOVERY_CANCEL = 0x74,
    DSP_BLE_T_FACTORY_RESET = 0x7D,
    DSP_BLE_T_END_SESSION = 0x7E,
    DSP_BLE_T_GET_HISTORY = 0x90, /* Phase C: first type of the journal range 0x90-0x9F */
    DSP_BLE_T_GET_UPDATE_STATUS = 0xB0, /* firmware >= 0.3.0 (docs/BLE_PROTOCOL.md 7.8, 10.8) */
    DSP_BLE_T_START_UPDATE = 0xB1,      /* firmware >= 0.3.0 */
    DSP_BLE_T_EV_SESSION_CLOSING = 0x80,
    DSP_BLE_T_EV_SUSPENDING_FOR_DOSE = 0x81,
    DSP_BLE_T_EV_BUSY = 0x82,
    DSP_BLE_T_EV_CONFIRM_ON_DEVICE = 0x83,
} dsp_ble_type_t;

/* Wire error codes (error body). */
typedef enum {
    DSP_BLE_E_MALFORMED = 1,
    DSP_BLE_E_UNSUPPORTED_VERSION,
    DSP_BLE_E_UNKNOWN_TYPE,
    DSP_BLE_E_UNAUTHENTICATED,
    DSP_BLE_E_FORBIDDEN,
    DSP_BLE_E_BUSY,
    DSP_BLE_E_REFUSED,
    DSP_BLE_E_LIMIT,
    DSP_BLE_E_NOT_COVERED,
    DSP_BLE_E_INVALID,
    DSP_BLE_E_CONFLICT,
    DSP_BLE_E_NOT_FOUND,
    DSP_BLE_E_STORAGE,
    DSP_BLE_E_DEGRADED,
    DSP_BLE_E_PUMP_FAILED,
    DSP_BLE_E_SESSION_EXPIRED,
    DSP_BLE_E_DUPLICATE,
    DSP_BLE_E_HANDSHAKE_FAILED,
    DSP_BLE_E_SLOTS_FULL,
    DSP_BLE_E_UNAVAILABLE, /* 20: a recognized operation this firmware does not provide (M1.10c.1) */
} dsp_ble_error_code_t;

/* HELLO / Info flags (protocol 5.1, 7.1) and advertising flags (7.1). */
#define DSP_BLE_HELLO_F_RECOVERY_ARMED 0x01u
#define DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED 0x02u /* M1.10c.1 (decision O9) */
#define DSP_BLE_HELLO_F_ALL 0x03u
#define DSP_BLE_ADV_F_CLAIMED 0x01u
#define DSP_BLE_ADV_F_RECOVERY_ARMED 0x02u
#define DSP_BLE_ADV_F_CREDENTIALS_DAMAGED 0x04u
#define DSP_BLE_ADV_F_ALL 0x07u

/* Codec results (the "reasons" of the shared vectors). */
typedef enum {
    DSP_BLE_OK = 0,
    DSP_BLE_ERR_ARG,
    DSP_BLE_ERR_TRUNCATED,
    DSP_BLE_ERR_LENGTH_MISMATCH,
    DSP_BLE_ERR_BAD_MAJOR,
    DSP_BLE_ERR_BAD_FLAGS,
    DSP_BLE_ERR_UNKNOWN_TYPE,
    DSP_BLE_ERR_NOT_A_REQUEST,
    DSP_BLE_ERR_TRAILING,
    DSP_BLE_ERR_BAD_BOOL,
    DSP_BLE_ERR_BAD_ENUM,
    DSP_BLE_ERR_BAD_RESERVED,
    DSP_BLE_ERR_BAD_DATE,
    DSP_BLE_ERR_BAD_STRING,
    DSP_BLE_ERR_TOO_LARGE,
    DSP_BLE_ERR_BAD_ENCRYPTED,
    DSP_BLE_ERR_BAD_MAGIC,
    DSP_BLE_ERR_FRAG_NO_FIRST,
    DSP_BLE_ERR_FRAG_INDEX,
    DSP_BLE_ERR_FRAG_EMPTY,
    DSP_BLE_ERR_FRAG_TIMEOUT,
    DSP_BLE_ERR_FRAG_OVERFLOW,
    DSP_BLE_ERR_BUFFER, /* output buffer too small (encoder) */
} dsp_ble_result_t;

/* Name of a result as used in the shared vectors ("truncated", ...). */
const char *dsp_ble_result_name(dsp_ble_result_t r);

/* ---- Value types ------------------------------------------------------------------------- */

typedef struct {
    uint8_t len;
    uint8_t b[DSP_BLE_STR_MAX];
} dsp_ble_str_t;

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
} dsp_ble_date_t;

typedef struct {
    uint16_t len;
    uint8_t b[DSP_BLE_PLAIN_BODY_MAX];
} dsp_ble_blob_t;

typedef struct {
    uint8_t readiness;
    uint8_t dosing_flags;
    uint32_t gates;
    uint32_t pump_problems;
    uint32_t capacity_ul;
    uint32_t remaining_ul;
    uint8_t low_percent;
    bool estimate_uncertain;
    uint8_t clock_trust;
    uint8_t clock_reason;
    int64_t device_utc;
    bool next_due_known;
    int64_t next_due_at;
    uint8_t pending_notes;
    uint8_t highest_severity;
    bool storage_ok;
    uint16_t session_seconds_left;
    uint8_t battery_percent;
    uint64_t manual_floor; /* the authenticated slot's next acceptable MANUAL_DOSE seq */
} dsp_ble_status_rec_t;

typedef struct {
    bool notify_storage_ok;
    uint8_t tx_diag;
    uint16_t tx_diag_status;
    uint8_t rejections_unread;
    uint16_t rejections_total;
    uint8_t clock_trust;
    uint8_t clock_reason;
    int64_t last_sync_utc;
    uint8_t last_wake_reason;
} dsp_ble_diag_rec_t;

/* Battery telemetry (protocol 7.7): state 0 VALID .. 5 UNSTABLE (dsp_batt_state_t). */
typedef struct {
    uint8_t state;
    uint16_t battery_mv; /* 0 unless VALID */
    uint32_t age_s;      /* seconds since the measurement (0 unless VALID) */
} dsp_ble_battery_rec_t;

typedef struct {
    uint32_t revision;
    uint32_t reservoir_capacity_ul;
    uint32_t reservoir_remaining_ul;
    uint8_t low_stock_percent;
    uint16_t time_sync_interval_h;
    uint8_t notify_mask;
    uint32_t pump_flow_ul_per_s;
    int64_t calibrated_at;
    uint32_t max_dose_ul;
    uint32_t max_daily_ul;
    uint32_t max_runtime_ms;
    uint32_t min_interval_s;
    uint32_t hw_max_runtime_ms;
} dsp_ble_config_rec_t;

typedef struct {
    dsp_ble_str_t id;
    uint32_t revision;
    bool enabled;
    uint32_t amount_ul;
    dsp_ble_date_t start_date;
    uint8_t hour;
    uint8_t minute;
    uint8_t kind;
    uint32_t interval;
    uint8_t weekday_mask;
    bool has_end;
    dsp_ble_date_t end_date;
    dsp_ble_str_t name;
} dsp_ble_schedule_rec_t;

/* ---- Bodies (one member per type and direction) -------------------------------------------- */

typedef struct { uint8_t proto_min, proto_max; } dsp_ble_hello_req_t;
typedef struct {
    uint8_t proto, claim_state, flags;
    uint32_t caps, session_id;
    uint8_t device_id_hash[8];
} dsp_ble_hello_rsp_t;
typedef struct { uint8_t purpose, msg_index; dsp_ble_blob_t noise; } dsp_ble_hs_req_t;
typedef struct { uint8_t msg_index; dsp_ble_blob_t noise; } dsp_ble_hs_rsp_t;
typedef struct { uint8_t slot; } dsp_ble_slot_t;
typedef struct { dsp_ble_status_rec_t status; } dsp_ble_status_body_t;
typedef struct { dsp_ble_diag_rec_t diag; } dsp_ble_diag_body_t;
typedef struct { dsp_ble_battery_rec_t battery; } dsp_ble_battery_body_t;
typedef struct { dsp_ble_config_rec_t config; } dsp_ble_config_body_t;
typedef struct {
    uint32_t expected_revision;
    uint16_t field_mask;
    dsp_ble_config_rec_t config;
} dsp_ble_set_config_req_t;
typedef struct { uint32_t revision; } dsp_ble_revision_t;
typedef struct { uint8_t page; } dsp_ble_page_t;
typedef struct { dsp_ble_str_t id; uint32_t revision; } dsp_ble_sched_item_t;
typedef struct { uint8_t n; dsp_ble_sched_item_t items[DSP_BLE_LIST_MAX]; } dsp_ble_sched_list_t;
typedef struct {
    uint32_t set_revision;
    uint8_t total, page;
    dsp_ble_sched_list_t items;
} dsp_ble_list_schedules_rsp_t;
typedef struct { dsp_ble_str_t id; } dsp_ble_id_t;
typedef struct { dsp_ble_schedule_rec_t schedule; } dsp_ble_schedule_body_t;
typedef struct { uint32_t set_revision; } dsp_ble_set_revision_t;
typedef struct { uint32_t expected_revision; dsp_ble_schedule_rec_t schedule; } dsp_ble_update_schedule_req_t;
typedef struct { uint32_t revision, set_revision; } dsp_ble_update_schedule_rsp_t;
typedef struct { dsp_ble_str_t id; uint32_t expected_revision; } dsp_ble_delete_schedule_req_t;
typedef struct { uint64_t seq; uint32_t volume_ul; } dsp_ble_manual_dose_req_t;
typedef struct {
    uint8_t outcome;
    uint32_t volume_ul, runtime_ms;
    dsp_ble_status_rec_t status;
} dsp_ble_manual_dose_rsp_t;
typedef struct { uint64_t seq; } dsp_ble_seq_t;
typedef struct { uint8_t state; uint32_t detail; } dsp_ble_get_result_rsp_t;
typedef struct { uint32_t capacity_ul, remaining_ul; } dsp_ble_refill_req_t;
typedef struct { uint8_t readiness; } dsp_ble_readiness_t;
typedef struct { int64_t phone_utc_ms; uint32_t uncertainty_ms; } dsp_ble_set_time_req_t;
typedef struct { uint8_t decision; int64_t offset_s; } dsp_ble_set_time_rsp_t;
typedef struct { dsp_ble_str_t ssid, passphrase; } dsp_ble_wifi_set_req_t;
typedef struct { dsp_ble_str_t ssid; bool passphrase_set; uint8_t last_result; } dsp_ble_wifi_get_rsp_t;
typedef struct {
    uint8_t slot;
    dsp_ble_str_t name;
    int64_t enrolled_at, last_seen;
    bool is_self;
} dsp_ble_phone_item_t;
typedef struct { uint8_t n; dsp_ble_phone_item_t items[DSP_BLE_LIST_MAX]; } dsp_ble_phone_list_t;
typedef struct { dsp_ble_phone_list_t items; } dsp_ble_phones_list_rsp_t;
typedef struct { dsp_ble_str_t name_hint; } dsp_ble_invite_create_req_t;
typedef struct { uint8_t token[16]; uint16_t expires_s; } dsp_ble_invite_create_rsp_t;
typedef struct { uint32_t confirm; } dsp_ble_factory_reset_req_t;
typedef struct { uint8_t reason; uint16_t seconds_left; } dsp_ble_ev_session_closing_t;
typedef struct { uint16_t seconds_until; } dsp_ble_ev_suspending_t;
typedef struct { uint8_t action, seconds; } dsp_ble_ev_confirm_t;
typedef struct { uint8_t code; uint32_t detail, aux; } dsp_ble_error_body_t;
typedef struct { uint32_t before_seq; uint8_t max; } dsp_ble_get_history_req_t;
typedef struct {
    uint32_t seq;
    uint8_t type, result, flags;
    int64_t at;
    uint32_t volume_ul, before_ul, after_ul, capacity_ul;
    dsp_ble_str_t schedule_id;
} dsp_ble_hist_item_t;
typedef struct { uint8_t n; dsp_ble_hist_item_t items[DSP_BLE_LIST_MAX]; } dsp_ble_hist_list_t;
typedef struct {
    uint32_t newest_seq, oldest_seq;
    uint8_t flags; /* bit 0 FAULTS */
    dsp_ble_hist_list_t items;
} dsp_ble_get_history_rsp_t;
#define DSP_BLE_HISTORY_F_FAULTS 0x01u
/* Firmware update (protocol 7.8, 10.8). Versions "X.Y.Z"; tag = GitHub release tag. */
typedef struct {
    dsp_ble_str_t running_version;
    bool capable;
    uint8_t last_result; /* dsp_ota_result_t */
    dsp_ble_str_t last_version;
} dsp_ble_update_status_rsp_t;
typedef struct {
    dsp_ble_str_t version, tag;
    uint32_t size;
    uint8_t sha256[32];
} dsp_ble_start_update_req_t;
typedef struct { uint8_t verdict; } dsp_ble_start_update_rsp_t; /* dsp_ota_verdict_t */

typedef union {
    dsp_ble_hello_req_t hello_req;
    dsp_ble_hello_rsp_t hello_rsp;
    dsp_ble_hs_req_t hs_req;
    dsp_ble_hs_rsp_t hs_rsp;
    dsp_ble_slot_t slot; /* ENROLL_RESULT response, PHONE_REMOVE request */
    dsp_ble_status_body_t status;
    dsp_ble_diag_body_t diag;
    dsp_ble_battery_body_t battery;
    dsp_ble_config_body_t config;
    dsp_ble_set_config_req_t set_config_req;
    dsp_ble_revision_t revision;
    dsp_ble_page_t page;
    dsp_ble_list_schedules_rsp_t list_schedules_rsp;
    dsp_ble_id_t id;
    dsp_ble_schedule_body_t schedule;
    dsp_ble_set_revision_t set_revision;
    dsp_ble_update_schedule_req_t update_schedule_req;
    dsp_ble_update_schedule_rsp_t update_schedule_rsp;
    dsp_ble_delete_schedule_req_t delete_schedule_req;
    dsp_ble_manual_dose_req_t manual_dose_req;
    dsp_ble_manual_dose_rsp_t manual_dose_rsp;
    dsp_ble_seq_t seq;
    dsp_ble_get_result_rsp_t get_result_rsp;
    dsp_ble_refill_req_t refill_req;
    dsp_ble_readiness_t readiness;
    dsp_ble_set_time_req_t set_time_req;
    dsp_ble_set_time_rsp_t set_time_rsp;
    dsp_ble_wifi_set_req_t wifi_set_req;
    dsp_ble_wifi_get_rsp_t wifi_get_rsp;
    dsp_ble_phones_list_rsp_t phones_list_rsp;
    dsp_ble_invite_create_req_t invite_create_req;
    dsp_ble_invite_create_rsp_t invite_create_rsp;
    dsp_ble_factory_reset_req_t factory_reset_req;
    dsp_ble_ev_session_closing_t ev_session_closing;
    dsp_ble_ev_suspending_t ev_suspending;
    dsp_ble_ev_confirm_t ev_confirm;
    dsp_ble_get_history_req_t get_history_req;
    dsp_ble_get_history_rsp_t get_history_rsp;
    dsp_ble_update_status_rsp_t update_status_rsp;
    dsp_ble_start_update_req_t start_update_req;
    dsp_ble_start_update_rsp_t start_update_rsp;
    dsp_ble_error_body_t error;
} dsp_ble_body_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t type;
    uint8_t flags;
    uint16_t txn;
    uint16_t len; /* body length */
} dsp_ble_header_t;

typedef struct {
    dsp_ble_header_t h;
    dsp_ble_body_t b; /* plain bodies only */
} dsp_ble_msg_t;

/* ---- Layout tables (also used by tests and the session layer) ------------------------------- */

typedef enum {
    DSP_BLEF_U8 = 1,
    DSP_BLEF_U16,
    DSP_BLEF_U32,
    DSP_BLEF_U64,
    DSP_BLEF_I64,
    DSP_BLEF_BOOL,
    DSP_BLEF_ENUM,   /* u8 in [min, max] or == extra */
    DSP_BLEF_MASK8,  /* only bits in allowed */
    DSP_BLEF_MASK16,
    DSP_BLEF_MASK32,
    DSP_BLEF_CONST32,
    DSP_BLEF_DATE,
    DSP_BLEF_RESERVED,
    DSP_BLEF_BYTES,  /* fixed length = min */
    DSP_BLEF_BLOB,   /* rest of the body */
    DSP_BLEF_STR,
    DSP_BLEF_RECORD,
    DSP_BLEF_LIST,
} dsp_ble_field_kind_t;

typedef enum {
    DSP_BLES_TEXT = 1, /* UTF-8, no control characters */
    DSP_BLES_ID,       /* [A-Za-z0-9._-] */
    DSP_BLES_RAW,      /* any bytes */
    DSP_BLES_WPA,      /* printable ASCII, length 0 or 8..63 */
} dsp_ble_str_kind_t;

struct dsp_ble_layout;

typedef struct {
    const char *name;
    uint8_t kind;          /* dsp_ble_field_kind_t */
    uint8_t str_kind;      /* dsp_ble_str_kind_t */
    uint16_t min, max;     /* STR length; ENUM range; BYTES/RESERVED length (min); LIST max */
    uint32_t allowed;      /* MASK allowed bits; CONST value; ENUM extra (0xFFFFFFFF = none) */
    uint16_t offset;       /* in the body struct */
    uint16_t cond_offset;  /* DATE: bool that must be true for a non-zero date (0xFFFF = none) */
    const struct dsp_ble_layout *sub; /* RECORD / LIST element */
    uint16_t elem_size;    /* LIST */
    uint16_t items_offset; /* LIST: offset of items[] within the list struct */
} dsp_ble_field_t;

typedef struct dsp_ble_layout {
    const dsp_ble_field_t *fields;
    uint8_t count;
} dsp_ble_layout_t;

typedef enum {
    DSP_BLE_CLASS_PLAIN = 1,    /* sent without ENCRYPTED */
    DSP_BLE_CLASS_ENCRYPTED,    /* MUST be ENCRYPTED */
    DSP_BLE_CLASS_EITHER,       /* END_SESSION */
} dsp_ble_class_t;

/* Layout of a body: type + flags (request, response, error, event); NULL if that combination
 * does not exist. *cls (optional) receives the security class of the type. */
const dsp_ble_layout_t *dsp_ble_layout_for(uint8_t type, uint8_t flags, dsp_ble_class_t *cls);

/* ---- Frames ----------------------------------------------------------------------------- */

/* Validates the header of a complete frame (major, flags, type/direction, length rules, size
 * limits, minimum encrypted length). *body points into frame. */
dsp_ble_result_t dsp_ble_parse_header(const uint8_t *frame, size_t len, dsp_ble_header_t *h,
                                      const uint8_t **body);

/* Decodes a PLAIN body (frame body, or plaintext after decryption) for h->type/flags. Trailing
 * bytes are accepted only if h->minor > my_minor. */
dsp_ble_result_t dsp_ble_decode_body(const dsp_ble_header_t *h, const uint8_t *body, size_t len,
                                     uint8_t my_minor, dsp_ble_body_t *out);

/* parse_header + decode_body for a plain frame. ENCRYPTED frames: header only (body untouched). */
dsp_ble_result_t dsp_ble_decode(const uint8_t *frame, size_t len, uint8_t my_minor, dsp_ble_msg_t *out);

/* Encodes the plain body of msg->h.type/flags into out (validating every rule). */
dsp_ble_result_t dsp_ble_encode_body(const dsp_ble_msg_t *msg, uint8_t *out, size_t cap, size_t *len);

/* Encodes a plain frame (header from msg->h except len, which is computed). */
dsp_ble_result_t dsp_ble_encode(const dsp_ble_msg_t *msg, uint8_t *out, size_t cap, size_t *len);

/* ---- Negotiation, Info, advertising -------------------------------------------------------- */

/* Highest common major; false if none (or an empty/invalid range). */
bool dsp_ble_negotiate(uint8_t dev_min, uint8_t dev_max, uint8_t cli_min, uint8_t cli_max,
                       uint8_t *major);

typedef struct {
    uint8_t proto_min, proto_max, claim_state, flags;
    uint32_t caps;
    uint8_t fw_major, fw_minor;
    uint16_t fw_patch, device_tag;
} dsp_ble_info_t;

dsp_ble_result_t dsp_ble_info_encode(const dsp_ble_info_t *in, uint8_t out[DSP_BLE_INFO_LEN]);
dsp_ble_result_t dsp_ble_info_decode(const uint8_t *buf, size_t len, dsp_ble_info_t *out);

typedef struct {
    uint8_t proto_major, flags;
    uint16_t device_tag;
} dsp_ble_advert_t;

dsp_ble_result_t dsp_ble_advert_encode(const dsp_ble_advert_t *in, uint8_t out[DSP_BLE_ADVERT_LEN]);
dsp_ble_result_t dsp_ble_advert_decode(const uint8_t *buf, size_t len, dsp_ble_advert_t *out);

/* ---- Fragmentation ------------------------------------------------------------------------- */

/* Number of fragments for a frame at ATT MTU mtu (0 if mtu < 23, len 0 or > 64 fragments). */
size_t dsp_ble_frag_count(size_t frame_len, uint16_t mtu);

/* Fragment index of the frame into out; returns its length (0 on error). */
size_t dsp_ble_frag_get(const uint8_t *frame, size_t frame_len, uint16_t mtu, size_t index,
                        uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[DSP_BLE_FRAME_MAX];
    size_t len;
    uint8_t next_index;
    bool active;
    uint32_t last_ms;
} dsp_ble_reasm_t;

void dsp_ble_reasm_init(dsp_ble_reasm_t *r);

/* Feeds one fragment received at now_ms (monotonic). Returns OK with *complete = true when a
 * frame is complete (r->buf, r->len; valid until the next push), OK with false when more
 * fragments are needed, or a FRAG error (the partial frame is discarded). */
dsp_ble_result_t dsp_ble_reasm_push(dsp_ble_reasm_t *r, const uint8_t *frag, size_t len,
                                    uint32_t now_ms, bool *complete);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BLEPROTO_H */
