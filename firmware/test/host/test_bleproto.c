/*
 * BLE protocol v1 codec tests (M1.10a): the shared vectors test-vectors/ble_protocol_v1.txt
 * (field-exact decoding, byte-exact encoding, rejection reasons, forward compatibility,
 * negotiation, fragmentation, reassembly, Info record, advertising data) plus property tests
 * (canonical re-encoding of every accepted frame, rejection of every body truncation,
 * deterministic bit-flip and random-input fuzzing of the decoder and the reassembler).
 */
#define _POSIX_C_SOURCE 200809L /* strtok_r (host test only) */
#include "dsp_bleproto.h"
#include "dsp_test.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 32768
#define MAX_TOKENS 256

/* ---- Vector line parsing --------------------------------------------------------------------- */

typedef struct {
    char *key[MAX_TOKENS];
    char *val[MAX_TOKENS];
    int n;
    char *record;
} rec_t;

static void split(char *line, rec_t *r)
{
    r->n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    char *save = NULL;
    r->record = strtok_r(line, " ", &save);
    for (char *t = strtok_r(NULL, " ", &save); t != NULL && r->n < MAX_TOKENS; t = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(t, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        r->key[r->n] = t;
        r->val[r->n] = eq + 1;
        r->n++;
    }
}

static const char *get(const rec_t *r, const char *key)
{
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->key[i], key) == 0) {
            return r->val[i];
        }
    }
    return NULL;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s) / 2;
    if (n > cap) {
        return (size_t)-1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* ---- Generic field text (canonical form of the vectors) ---------------------------------------- */

typedef struct {
    char buf[LINE_MAX_LEN];
    size_t len;
} text_t;

static void tadd(text_t *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tadd(text_t *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(t->buf + t->len, sizeof t->buf - t->len, fmt, ap);
    va_end(ap);
    if (w > 0) {
        t->len += (size_t)w;
    }
}

static void thex(text_t *t, const char *key, const uint8_t *b, size_t n)
{
    tadd(t, "%s%s=x:", t->len ? " " : "", key);
    for (size_t i = 0; i < n; i++) {
        tadd(t, "%02x", b[i]);
    }
}

static void print_layout(const dsp_ble_layout_t *l, const uint8_t *base, const char *prefix, text_t *t)
{
    char key[160];
    for (size_t i = 0; i < l->count; i++) {
        const dsp_ble_field_t *f = &l->fields[i];
        if (f->kind == DSP_BLEF_RESERVED) {
            continue;
        }
        snprintf(key, sizeof key, "%s%s", prefix, f->name);
        const uint8_t *p = base + f->offset;
        const char *sep = t->len ? " " : "";
        switch (f->kind) {
        case DSP_BLEF_U8:
        case DSP_BLEF_ENUM:
        case DSP_BLEF_MASK8:
            tadd(t, "%s%s=%u", sep, key, *p);
            break;
        case DSP_BLEF_BOOL:
            tadd(t, "%s%s=%d", sep, key, *(const bool *)p ? 1 : 0);
            break;
        case DSP_BLEF_U16:
        case DSP_BLEF_MASK16: {
            uint16_t v;
            memcpy(&v, p, sizeof v);
            tadd(t, "%s%s=%u", sep, key, v);
            break;
        }
        case DSP_BLEF_U32:
        case DSP_BLEF_MASK32:
        case DSP_BLEF_CONST32: {
            uint32_t v;
            memcpy(&v, p, sizeof v);
            tadd(t, "%s%s=%" PRIu32, sep, key, v);
            break;
        }
        case DSP_BLEF_U64: {
            uint64_t v;
            memcpy(&v, p, sizeof v);
            tadd(t, "%s%s=%" PRIu64, sep, key, v);
            break;
        }
        case DSP_BLEF_I64: {
            int64_t v;
            memcpy(&v, p, sizeof v);
            tadd(t, "%s%s=%" PRId64, sep, key, v);
            break;
        }
        case DSP_BLEF_DATE: {
            dsp_ble_date_t d;
            memcpy(&d, p, sizeof d);
            tadd(t, "%s%s=%04u-%02u-%02u", sep, key, d.year, d.month, d.day);
            break;
        }
        case DSP_BLEF_BYTES:
            thex(t, key, p, f->min);
            break;
        case DSP_BLEF_BLOB: {
            const dsp_ble_blob_t *b = (const dsp_ble_blob_t *)(const void *)p;
            thex(t, key, b->b, b->len);
            break;
        }
        case DSP_BLEF_STR: {
            const dsp_ble_str_t *s = (const dsp_ble_str_t *)(const void *)p;
            thex(t, key, s->b, s->len);
            break;
        }
        case DSP_BLEF_RECORD: {
            char sub[170];
            snprintf(sub, sizeof sub, "%s.", key);
            print_layout(f->sub, p, sub, t);
            break;
        }
        case DSP_BLEF_LIST: {
            tadd(t, "%s%s.n=%u", sep, key, *p);
            for (size_t k = 0; k < *p; k++) {
                char sub[180];
                snprintf(sub, sizeof sub, "%s.%zu.", key, k);
                print_layout(f->sub, p + f->items_offset + k * f->elem_size, sub, t);
            }
            break;
        }
        default:
            break;
        }
    }
}

/* Sets struct fields from vector tokens (the inverse of print_layout). */
static bool parse_layout(const dsp_ble_layout_t *l, uint8_t *base, const char *prefix, const rec_t *r)
{
    char key[160];
    for (size_t i = 0; i < l->count; i++) {
        const dsp_ble_field_t *f = &l->fields[i];
        if (f->kind == DSP_BLEF_RESERVED) {
            continue;
        }
        snprintf(key, sizeof key, "%s%s", prefix, f->name);
        uint8_t *p = base + f->offset;
        if (f->kind == DSP_BLEF_RECORD) {
            char sub[170];
            snprintf(sub, sizeof sub, "%s.", key);
            if (!parse_layout(f->sub, p, sub, r)) {
                return false;
            }
            continue;
        }
        if (f->kind == DSP_BLEF_LIST) {
            char nk[170];
            snprintf(nk, sizeof nk, "%s.n", key);
            const char *nv = get(r, nk);
            if (nv == NULL) {
                return false;
            }
            *p = (uint8_t)strtoul(nv, NULL, 10);
            for (size_t k = 0; k < *p; k++) {
                char sub[180];
                snprintf(sub, sizeof sub, "%s.%zu.", key, k);
                if (!parse_layout(f->sub, p + f->items_offset + k * f->elem_size, sub, r)) {
                    return false;
                }
            }
            continue;
        }
        const char *v = get(r, key);
        if (v == NULL) {
            printf("missing vector field %s\n", key);
            return false;
        }
        switch (f->kind) {
        case DSP_BLEF_U8:
        case DSP_BLEF_ENUM:
        case DSP_BLEF_MASK8:
            *p = (uint8_t)strtoul(v, NULL, 10);
            break;
        case DSP_BLEF_BOOL:
            *(bool *)p = strcmp(v, "1") == 0;
            break;
        case DSP_BLEF_U16:
        case DSP_BLEF_MASK16: {
            uint16_t x = (uint16_t)strtoul(v, NULL, 10);
            memcpy(p, &x, sizeof x);
            break;
        }
        case DSP_BLEF_U32:
        case DSP_BLEF_MASK32:
        case DSP_BLEF_CONST32: {
            uint32_t x = (uint32_t)strtoul(v, NULL, 10);
            memcpy(p, &x, sizeof x);
            break;
        }
        case DSP_BLEF_U64: {
            uint64_t x = strtoull(v, NULL, 10);
            memcpy(p, &x, sizeof x);
            break;
        }
        case DSP_BLEF_I64: {
            int64_t x = strtoll(v, NULL, 10);
            memcpy(p, &x, sizeof x);
            break;
        }
        case DSP_BLEF_DATE: {
            unsigned y = 0, m = 0, d = 0;
            sscanf(v, "%u-%u-%u", &y, &m, &d);
            dsp_ble_date_t dt = {(uint16_t)y, (uint8_t)m, (uint8_t)d};
            memcpy(p, &dt, sizeof dt);
            break;
        }
        case DSP_BLEF_BYTES:
            if (strncmp(v, "x:", 2) != 0 || unhex(v + 2, p, f->min) != f->min) {
                return false;
            }
            break;
        case DSP_BLEF_BLOB: {
            dsp_ble_blob_t *b = (dsp_ble_blob_t *)(void *)p;
            size_t n = unhex(v + 2, b->b, sizeof b->b);
            if (n == (size_t)-1) {
                return false;
            }
            b->len = (uint16_t)n;
            break;
        }
        case DSP_BLEF_STR: {
            dsp_ble_str_t *s = (dsp_ble_str_t *)(void *)p;
            size_t n = unhex(v + 2, s->b, sizeof s->b);
            if (n == (size_t)-1) {
                return false;
            }
            s->len = (uint8_t)n;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

/* Expected field text of a msg record: every token except the record keys, in order. */
static void vector_fields(const rec_t *r, text_t *t)
{
    t->len = 0;
    t->buf[0] = '\0';
    for (int i = 0; i < r->n; i++) {
        const char *k = r->key[i];
        if (strcmp(k, "name") == 0 || strcmp(k, "kind") == 0 || strcmp(k, "type") == 0 ||
            strcmp(k, "txn") == 0 || strcmp(k, "frame") == 0) {
            continue;
        }
        tadd(t, "%s%s=%s", t->len ? " " : "", k, r->val[i]);
    }
    if (t->len == 0) {
        tadd(t, "-"); /* the vector writes "-" for a body without fields */
    }
}

static uint8_t flags_of(const char *kind)
{
    if (strcmp(kind, "rsp") == 0) {
        return DSP_BLE_F_RESPONSE;
    }
    if (strcmp(kind, "err") == 0) {
        return DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR;
    }
    if (strcmp(kind, "evt") == 0) {
        return DSP_BLE_F_EVENT;
    }
    return 0;
}

/* ---- Valid frames collected for the property tests ---------------------------------------------- */

#define MAX_VALID 128
static uint8_t valid_frames[MAX_VALID][DSP_BLE_FRAME_MAX];
static size_t valid_lens[MAX_VALID];
static bool valid_blob[MAX_VALID];
static int n_valid;

static int counts[16];
enum { C_MSG, C_ENC, C_BAD, C_COMPAT, C_NEG, C_FRAG, C_RBAD, C_ROK, C_INFO, C_IBAD, C_ADV, C_ABAD };

static dsp_ble_msg_t msg, msg2;
static uint8_t frame[DSP_BLE_FRAME_MAX + 64], out[DSP_BLE_FRAME_MAX + 64];
static text_t got_text, want_text;

static void run_msg(const rec_t *r)
{
    const char *name = get(r, "name");
    size_t flen = unhex(get(r, "frame"), frame, sizeof frame);
    dsp_ble_result_t e = dsp_ble_decode(frame, flen, DSP_BLE_MINOR, &msg);
    if (e != DSP_BLE_OK) {
        printf("%s: decode %s\n", name, dsp_ble_result_name(e));
    }
    DSP_CHECK_EQ_INT(DSP_BLE_OK, e);
    DSP_CHECK_EQ_INT(strtoul(get(r, "type"), NULL, 16), msg.h.type);
    DSP_CHECK_EQ_INT(strtoul(get(r, "txn"), NULL, 10), msg.h.txn);
    DSP_CHECK_EQ_INT(flags_of(get(r, "kind")), msg.h.flags);
    const dsp_ble_layout_t *l = dsp_ble_layout_for(msg.h.type, msg.h.flags, NULL);
    DSP_CHECK(l != NULL);
    if (l == NULL) {
        return;
    }
    got_text.len = 0;
    got_text.buf[0] = '\0';
    print_layout(l, (const uint8_t *)&msg.b, "", &got_text);
    if (got_text.len == 0) {
        tadd(&got_text, "-");
    }
    vector_fields(r, &want_text);
    if (strcmp(got_text.buf, want_text.buf) != 0) {
        printf("%s fields:\n  got  %s\n  want %s\n", name, got_text.buf, want_text.buf);
    }
    DSP_CHECK_EQ_STR(want_text.buf, got_text.buf);
    /* Encoding the vector's fields yields exactly the frame. */
    memset(&msg2, 0, sizeof msg2);
    msg2.h.major = DSP_BLE_MAJOR;
    msg2.h.minor = 0;
    msg2.h.type = (uint8_t)strtoul(get(r, "type"), NULL, 16);
    msg2.h.flags = flags_of(get(r, "kind"));
    msg2.h.txn = (uint16_t)strtoul(get(r, "txn"), NULL, 10);
    bool parsed = parse_layout(l, (uint8_t *)&msg2.b, "", r);
    DSP_CHECK(parsed);
    size_t olen = 0;
    e = dsp_ble_encode(&msg2, out, sizeof out, &olen);
    if (e != DSP_BLE_OK || olen != flen || memcmp(out, frame, flen) != 0) {
        printf("%s: encode %s (len %zu vs %zu)\n", name, dsp_ble_result_name(e), olen, flen);
    }
    DSP_CHECK_EQ_INT(DSP_BLE_OK, e);
    DSP_CHECK(olen == flen && memcmp(out, frame, flen) == 0);
    if (n_valid < MAX_VALID) {
        memcpy(valid_frames[n_valid], frame, flen);
        valid_lens[n_valid] = flen;
        valid_blob[n_valid] = msg.h.type == DSP_BLE_T_HS;
        n_valid++;
    }
    counts[C_MSG]++;
}

static void run_record(char *line)
{
    rec_t r;
    split(line, &r);
    if (r.record == NULL) {
        return;
    }
    const char *name = get(&r, "name");
    if (strcmp(r.record, "msg") == 0) {
        run_msg(&r);
    } else if (strcmp(r.record, "enc") == 0) {
        size_t flen = unhex(get(&r, "frame"), frame, sizeof frame);
        dsp_ble_header_t h;
        const uint8_t *body = NULL;
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_parse_header(frame, flen, &h, &body));
        DSP_CHECK(h.flags & DSP_BLE_F_ENCRYPTED);
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(frame, flen, DSP_BLE_MINOR, &msg));
        counts[C_ENC]++;
    } else if (strcmp(r.record, "bad") == 0) {
        size_t flen = unhex(get(&r, "frame"), frame, sizeof frame);
        dsp_ble_result_t e = dsp_ble_decode(frame, flen, DSP_BLE_MINOR, &msg);
        if (strcmp(dsp_ble_result_name(e), get(&r, "expect")) != 0) {
            printf("bad %s: got %s, want %s\n", name, dsp_ble_result_name(e), get(&r, "expect"));
        }
        DSP_CHECK_EQ_STR(get(&r, "expect"), dsp_ble_result_name(e));
        counts[C_BAD]++;
    } else if (strcmp(r.record, "compat") == 0) {
        size_t flen = unhex(get(&r, "frame"), frame, sizeof frame);
        uint8_t my = (uint8_t)strtoul(get(&r, "my_minor"), NULL, 10);
        dsp_ble_result_t e = dsp_ble_decode(frame, flen, my, &msg);
        DSP_CHECK_EQ_INT(strcmp(get(&r, "accept"), "1") == 0 ? DSP_BLE_OK : DSP_BLE_ERR_TRAILING, e);
        counts[C_COMPAT]++;
    } else if (strcmp(r.record, "negotiate") == 0) {
        uint8_t major = 0;
        bool ok = dsp_ble_negotiate((uint8_t)atoi(get(&r, "dev_min")), (uint8_t)atoi(get(&r, "dev_max")),
                                    (uint8_t)atoi(get(&r, "cli_min")), (uint8_t)atoi(get(&r, "cli_max")), &major);
        const char *want = get(&r, "expect");
        if (strcmp(want, "none") == 0) {
            DSP_CHECK(!ok);
        } else {
            DSP_CHECK(ok);
            DSP_CHECK_EQ_INT(atoi(want), major);
        }
        counts[C_NEG]++;
    } else if (strcmp(r.record, "frag") == 0) {
        size_t flen = unhex(get(&r, "frame"), frame, sizeof frame);
        uint16_t mtu = (uint16_t)atoi(get(&r, "mtu"));
        char *frags = (char *)get(&r, "fragments");
        size_t n = dsp_ble_frag_count(flen, mtu);
        dsp_ble_reasm_t ra;
        dsp_ble_reasm_init(&ra);
        size_t i = 0;
        bool done = false;
        char *save = NULL;
        for (char *f = strtok_r(frags, ",", &save); f != NULL; f = strtok_r(NULL, ",", &save), i++) {
            uint8_t want[600];
            size_t wl = unhex(f, want, sizeof want);
            uint8_t gotf[600];
            size_t gl = dsp_ble_frag_get(frame, flen, mtu, i, gotf, sizeof gotf);
            DSP_CHECK(gl == wl && memcmp(gotf, want, wl) == 0);
            DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_reasm_push(&ra, want, wl, (uint32_t)(i * 10), &done));
        }
        DSP_CHECK_EQ_INT(n, i);
        DSP_CHECK(done && ra.len == flen && memcmp(ra.buf, frame, flen) == 0);
        counts[C_FRAG]++;
    } else if (strcmp(r.record, "reasm_bad") == 0 || strcmp(r.record, "reasm_ok") == 0) {
        bool ok_case = strcmp(r.record, "reasm_ok") == 0;
        char *frags = (char *)get(&r, "fragments");
        char *ms = (char *)get(&r, "ms");
        dsp_ble_reasm_t ra;
        dsp_ble_reasm_init(&ra);
        dsp_ble_result_t last = DSP_BLE_OK;
        bool done = false;
        char *s1 = NULL, *s2 = NULL;
        char *t = strtok_r(ms, ",", &s2);
        for (char *f = strtok_r(frags, ",", &s1); f != NULL; f = strtok_r(NULL, ",", &s1)) {
            static uint8_t buf[600];
            size_t fl = unhex(f, buf, sizeof buf);
            last = dsp_ble_reasm_push(&ra, buf, fl, (uint32_t)strtoul(t != NULL ? t : "0", NULL, 10), &done);
            t = strtok_r(NULL, ",", &s2);
            if (last != DSP_BLE_OK) {
                break;
            }
        }
        if (ok_case) {
            size_t flen = unhex(get(&r, "frame"), frame, sizeof frame);
            DSP_CHECK_EQ_INT(DSP_BLE_OK, last);
            DSP_CHECK(done && ra.len == flen && memcmp(ra.buf, frame, flen) == 0);
            counts[C_ROK]++;
        } else {
            if (strcmp(dsp_ble_result_name(last), get(&r, "expect")) != 0) {
                printf("reasm %s: got %s\n", name, dsp_ble_result_name(last));
            }
            DSP_CHECK_EQ_STR(get(&r, "expect"), dsp_ble_result_name(last));
            counts[C_RBAD]++;
        }
    } else if (strcmp(r.record, "info") == 0) {
        uint8_t rec[64];
        size_t rl = unhex(get(&r, "record"), rec, sizeof rec);
        dsp_ble_info_t info;
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_info_decode(rec, rl, &info));
        DSP_CHECK_EQ_INT(atoi(get(&r, "proto_min")), info.proto_min);
        DSP_CHECK_EQ_INT(atoi(get(&r, "proto_max")), info.proto_max);
        DSP_CHECK_EQ_INT(atoi(get(&r, "claim_state")), info.claim_state);
        DSP_CHECK_EQ_INT(atoi(get(&r, "flags")), info.flags);
        DSP_CHECK_EQ_INT(atoi(get(&r, "caps")), info.caps);
        DSP_CHECK_EQ_INT(atoi(get(&r, "fw_major")), info.fw_major);
        DSP_CHECK_EQ_INT(atoi(get(&r, "fw_minor")), info.fw_minor);
        DSP_CHECK_EQ_INT(atoi(get(&r, "fw_patch")), info.fw_patch);
        DSP_CHECK_EQ_INT(atoi(get(&r, "device_tag")), info.device_tag);
        uint8_t enc[DSP_BLE_INFO_LEN];
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_info_encode(&info, enc));
        DSP_CHECK(rl == DSP_BLE_INFO_LEN && memcmp(enc, rec, rl) == 0);
        counts[C_INFO]++;
    } else if (strcmp(r.record, "info_bad") == 0) {
        uint8_t rec[64];
        size_t rl = unhex(get(&r, "record"), rec, sizeof rec);
        dsp_ble_info_t info;
        DSP_CHECK_EQ_STR(get(&r, "expect"), dsp_ble_result_name(dsp_ble_info_decode(rec, rl, &info)));
        counts[C_IBAD]++;
    } else if (strcmp(r.record, "advert") == 0) {
        uint8_t d[8];
        size_t dl = unhex(get(&r, "data"), d, sizeof d);
        dsp_ble_advert_t a;
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_advert_decode(d, dl, &a));
        DSP_CHECK_EQ_INT(atoi(get(&r, "proto_major")), a.proto_major);
        DSP_CHECK_EQ_INT(atoi(get(&r, "flags")), a.flags);
        DSP_CHECK_EQ_INT(atoi(get(&r, "device_tag")), a.device_tag);
        uint8_t enc[DSP_BLE_ADVERT_LEN];
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_advert_encode(&a, enc));
        DSP_CHECK(memcmp(enc, d, dl) == 0);
        counts[C_ADV]++;
    } else if (strcmp(r.record, "advert_bad") == 0) {
        uint8_t d[8];
        size_t dl = unhex(get(&r, "data"), d, sizeof d);
        dsp_ble_advert_t a;
        DSP_CHECK_EQ_STR(get(&r, "expect"), dsp_ble_result_name(dsp_ble_advert_decode(d, dl, &a)));
        counts[C_ABAD]++;
    } else {
        printf("unknown record %s\n", r.record);
        DSP_CHECK(false);
    }
}

static char line[LINE_MAX_LEN];

static void test_shared_vectors(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", DSP_VECTORS_DIR, "ble_protocol_v1.txt");
    FILE *f = fopen(path, "r");
    DSP_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        DSP_CHECK(strchr(line, '\n') != NULL || feof(f));
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        run_record(line);
    }
    fclose(f);
    printf("vectors: %d msg, %d enc, %d bad, %d compat, %d negotiate, %d frag, %d reasm_bad, %d reasm_ok, "
           "%d info, %d info_bad, %d advert, %d advert_bad\n",
           counts[C_MSG], counts[C_ENC], counts[C_BAD], counts[C_COMPAT], counts[C_NEG], counts[C_FRAG],
           counts[C_RBAD], counts[C_ROK], counts[C_INFO], counts[C_IBAD], counts[C_ADV], counts[C_ABAD]);
    DSP_CHECK(counts[C_MSG] >= 61 && counts[C_BAD] >= 56 && counts[C_FRAG] >= 6 && counts[C_RBAD] >= 5);
    DSP_CHECK(counts[C_INFO] >= 3 && counts[C_ADV] >= 2 && counts[C_ABAD] >= 3);
}

/* ---- Properties ----------------------------------------------------------------------------------- */

/* Every accepted frame re-encodes to exactly the same bytes (canonical encoding). */
static void check_canonical(const uint8_t *f, size_t n, int *accepted)
{
    if (dsp_ble_decode(f, n, DSP_BLE_MINOR, &msg) != DSP_BLE_OK || (msg.h.flags & DSP_BLE_F_ENCRYPTED)) {
        return;
    }
    (*accepted)++;
    size_t olen = 0;
    dsp_ble_result_t e = dsp_ble_encode(&msg, out, sizeof out, &olen);
    DSP_CHECK_EQ_INT(DSP_BLE_OK, e);
    DSP_CHECK(olen == n && memcmp(out, f, n) == 0);
}

static void test_truncation_rejected(void)
{
    int checked = 0;
    for (int v = 0; v < n_valid; v++) {
        if (valid_blob[v] || valid_lens[v] == DSP_BLE_HEADER_LEN) {
            continue; /* a blob body accepts any length; empty bodies have no prefix */
        }
        for (size_t cut = DSP_BLE_HEADER_LEN; cut < valid_lens[v]; cut++) {
            memcpy(frame, valid_frames[v], cut);
            frame[6] = (uint8_t)(cut - DSP_BLE_HEADER_LEN);
            frame[7] = (uint8_t)((cut - DSP_BLE_HEADER_LEN) >> 8);
            DSP_CHECK(dsp_ble_decode(frame, cut, DSP_BLE_MINOR, &msg) != DSP_BLE_OK);
            checked++;
        }
    }
    printf("truncation: %d prefixes rejected\n", checked);
    DSP_CHECK(checked >= 1000);
}

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_bitflip_fuzz(void)
{
    int tries = 0, accepted = 0;
    for (int v = 0; v < n_valid; v++) {
        for (size_t pos = 0; pos < valid_lens[v]; pos++) {
            for (int bit = 0; bit < 8; bit++) {
                memcpy(frame, valid_frames[v], valid_lens[v]);
                frame[pos] ^= (uint8_t)(1u << bit);
                check_canonical(frame, valid_lens[v], &accepted);
                tries++;
            }
        }
    }
    printf("bit-flip fuzz: %d mutated frames, %d accepted and canonical\n", tries, accepted);
}

static void test_random_fuzz(void)
{
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        size_t n = rnd() % (DSP_BLE_FRAME_MAX + 32);
        for (size_t k = 0; k < n; k++) {
            frame[k] = (uint8_t)rnd();
        }
        if (n >= DSP_BLE_HEADER_LEN && (rnd() & 1u)) {
            /* make the header plausible so the body decoder is exercised */
            frame[0] = DSP_BLE_MAJOR;
            frame[1] = 0;
            frame[3] &= 0x07u;
            frame[6] = (uint8_t)(n - DSP_BLE_HEADER_LEN);
            frame[7] = (uint8_t)((n - DSP_BLE_HEADER_LEN) >> 8);
        }
        check_canonical(frame, n, &accepted);
    }
    /* Reassembler: random fragment streams never overflow or crash. */
    dsp_ble_reasm_t ra;
    dsp_ble_reasm_init(&ra);
    uint32_t now = 0;
    int completed = 0;
    for (int i = 0; i < 200000; i++) {
        size_t n = rnd() % 40;
        for (size_t k = 0; k < n; k++) {
            frame[k] = (uint8_t)rnd();
        }
        if (n > 0 && (rnd() % 3) != 0) {
            frame[0] = (uint8_t)((frame[0] & 0xC0u) | ra.next_index);
        }
        now += rnd() % 700;
        bool done = false;
        (void)dsp_ble_reasm_push(&ra, frame, n, now, &done);
        DSP_CHECK(ra.len <= DSP_BLE_FRAME_MAX);
        completed += done;
    }
    printf("random fuzz: 200000 frames (%d accepted), 200000 fragments (%d frames completed)\n", accepted,
           completed);
}

static void test_encoder_rejects_invalid_values(void)
{
    memset(&msg, 0, sizeof msg);
    msg.h.major = DSP_BLE_MAJOR;
    msg.h.type = DSP_BLE_T_SET_READINESS;
    msg.b.readiness.readiness = 3;
    size_t olen = 0;
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_BAD_ENUM, dsp_ble_encode(&msg, out, sizeof out, &olen));
    msg.h.type = DSP_BLE_T_GET_SCHEDULE;
    msg.b.id.id.len = 3;
    memcpy(msg.b.id.id.b, "a b", 3);
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_BAD_STRING, dsp_ble_encode(&msg, out, sizeof out, &olen));
    msg.h.type = DSP_BLE_T_EV_BUSY;
    msg.h.flags = 0;
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_NOT_A_REQUEST, dsp_ble_encode(&msg, out, sizeof out, &olen));
    msg.h.flags = DSP_BLE_F_ENCRYPTED;
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_ARG, dsp_ble_encode(&msg, out, sizeof out, &olen));
    /* HS noise blob that would exceed the plain body limit. */
    memset(&msg, 0, sizeof msg);
    msg.h.major = DSP_BLE_MAJOR;
    msg.h.type = DSP_BLE_T_HS;
    msg.b.hs_req.purpose = 1;
    msg.b.hs_req.noise.len = DSP_BLE_PLAIN_BODY_MAX - 1;
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_TOO_LARGE, dsp_ble_encode(&msg, out, sizeof out, &olen));
    msg.b.hs_req.noise.len = DSP_BLE_PLAIN_BODY_MAX - 2;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode(&msg, out, sizeof out, &olen));
    DSP_CHECK_EQ_INT(DSP_BLE_HEADER_LEN + DSP_BLE_PLAIN_BODY_MAX, olen);
    /* Small output buffer. */
    DSP_CHECK_EQ_INT(DSP_BLE_ERR_BUFFER, dsp_ble_encode(&msg, out, 64, &olen));
    /* Security classes. */
    dsp_ble_class_t cls;
    DSP_CHECK(dsp_ble_layout_for(DSP_BLE_T_HELLO, 0, &cls) != NULL && cls == DSP_BLE_CLASS_PLAIN);
    DSP_CHECK(dsp_ble_layout_for(DSP_BLE_T_MANUAL_DOSE, 0, &cls) != NULL && cls == DSP_BLE_CLASS_ENCRYPTED);
    DSP_CHECK(dsp_ble_layout_for(DSP_BLE_T_END_SESSION, 0, &cls) != NULL && cls == DSP_BLE_CLASS_EITHER);
    DSP_CHECK(dsp_ble_layout_for(0x05, 0, &cls) == NULL);
}

int main(void)
{
    DSP_RUN(test_shared_vectors);
    DSP_RUN(test_truncation_rejected);
    DSP_RUN(test_bitflip_fuzz);
    DSP_RUN(test_random_fuzz);
    DSP_RUN(test_encoder_rejects_invalid_values);
    return dsp_test_finish();
}
