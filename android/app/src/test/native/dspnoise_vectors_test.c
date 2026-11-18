/*
 * Native vector runner for the Android Noise core (M1.10e.1, docs/ANDROID_BLE_DESIGN.md 3.5, 6.1).
 * Built twice from the same sources as the app library (dspnoise_core.c, with DSPN_TEST_HOOKS for
 * the responder role and fixed ephemerals):
 *   - for the Linux host (developer machines, CI);
 *   - with the Android NDK for x86_64 as a STATIC executable (bionic, FORTIFY on), run directly on
 *     the Linux CI host — the Android compilation of noise-c + libsodium + core, without emulator.
 *
 * Three separate checks, never mixed:
 *   1. official Noise conformance vectors (noise-c's cacophony.txt / noise-c-basic.txt):
 *      Noise_XXpsk3_25519_ChaChaPoly_SHA256 and Noise_IK_25519_ChaChaPoly_SHA256, 0 skipped;
 *   2. test-vectors/ble_session_v1.txt PSK records (psk, psk_not);
 *   3. test-vectors/ble_handshake_v1.txt Dispenser transcripts (Rust snow): every phone and device
 *      frame, handshake hash and encrypted frame byte for byte, plus the negative records.
 *
 * Usage: dspnoise_vectors <noise-c tests/vector dir> <test-vectors dir>
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dspnoise_core.h"
#include "json-reader.h"

static int g_fail, g_checks;

static void check(int ok, const char *fmt, ...)
{
    g_checks++;
    if (!ok) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
        g_fail++;
    }
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    if (s == NULL || strcmp(s, "-") == 0) {
        return 0;
    }
    if (strncmp(s, "x:", 2) == 0) {
        s += 2;
    }
    size_t n = strlen(s) / 2;
    if (n > cap) {
        check(0, "hex value longer than %zu", cap);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            check(0, "bad hex");
            return 0;
        }
        out[i] = (uint8_t)v;
    }
    return n;
}

/* ---- 1. official vectors --------------------------------------------------------------------- */

#define OV_MSG 16
typedef struct {
    char name[64];
    /* 64-byte key buffers: other suites in the files use Curve448 (56-byte) keys; only the two
       25519 protocols are run */
    uint8_t ipro[128], rpro[128], is[64], ie[64], irs[64], rs[64], re[64], ipsk[64], rpsk[64], hh[64];
    size_t ipro_len, rpro_len, hh_len;
    int has_irs, has_ipsk, has_rpsk;
    uint8_t pay[OV_MSG][1024], ct[OV_MSG][1200];
    size_t pay_len[OV_MSG], ct_len[OV_MSG];
    int nmsg;
} ov_t;

static int run_official(const ov_t *v)
{
    int pattern = strcmp(v->name, "Noise_IK_25519_ChaChaPoly_SHA256") == 0 ? DSPN_IK : DSPN_XXPSK3;
    dspn_hs *i = NULL, *r = NULL;
    dspn_cs *ic = NULL, *rc = NULL;
    int ok = dspn_hs_create(&i, pattern, 1, v->ipro, v->ipro_len, v->is, v->has_irs ? v->irs : NULL,
                            v->has_ipsk ? v->ipsk : NULL, v->ie) == DSPN_OK &&
             dspn_hs_create(&r, pattern, 0, v->rpro, v->rpro_len, v->rs, NULL, v->has_rpsk ? v->rpsk : NULL,
                            v->re) == DSPN_OK;
    uint8_t out[700], pl[700];
    size_t n = 0, pn = 0;
    for (int m = 0; ok && m < v->nmsg; m++) {
        int from_i = m % 2 == 0;
        if (ic == NULL) {
            dspn_hs *w = from_i ? i : r, *rd = from_i ? r : i;
            ok = dspn_hs_write(w, v->pay[m], v->pay_len[m], out, sizeof out, &n) == DSPN_OK && n == v->ct_len[m] &&
                 memcmp(out, v->ct[m], n) == 0 && dspn_hs_read(rd, out, n, pl, sizeof pl, &pn) == DSPN_OK &&
                 pn == v->pay_len[m] && memcmp(pl, v->pay[m], pn) == 0;
            if (ok && dspn_hs_finished(i)) {
                uint8_t h1[32], h2[32];
                ok = dspn_hs_finished(r) && dspn_hs_hash(i, h1) == DSPN_OK && dspn_hs_hash(r, h2) == DSPN_OK &&
                     memcmp(h1, h2, 32) == 0 && (v->hh_len == 0 || memcmp(h1, v->hh, 32) == 0) &&
                     dspn_hs_split(i, &ic) == DSPN_OK && dspn_hs_split(r, &rc) == DSPN_OK;
            }
        } else {
            static const uint8_t none[1] = {0};
            dspn_cs *w = from_i ? ic : rc, *rd = from_i ? rc : ic;
            ok = dspn_cs_encrypt(w, none, 0, v->pay[m], v->pay_len[m], out, sizeof out, &n) == DSPN_OK &&
                 n == v->ct_len[m] && memcmp(out, v->ct[m], n) == 0 &&
                 dspn_cs_decrypt(rd, none, 0, out, n, pl, sizeof pl, &pn) == DSPN_OK && pn == v->pay_len[m] &&
                 memcmp(pl, v->pay[m], pn) == 0;
        }
    }
    dspn_hs_free(i);
    dspn_hs_free(r);
    dspn_cs_free(ic);
    dspn_cs_free(rc);
    return ok && ic != NULL;
}

/* Reads one string value after a name token. */
static const char *json_str(JSONReader *rd)
{
    json_next_token(rd); /* ':' */
    json_next_token(rd);
    return rd->token == JSON_TOKEN_STRING ? rd->str_value : NULL;
}

static void skip_value(JSONReader *rd)
{
    int depth = 0;
    do {
        if (rd->token == JSON_TOKEN_LBRACE || rd->token == JSON_TOKEN_LSQUARE) {
            depth++;
        } else if (rd->token == JSON_TOKEN_RBRACE || rd->token == JSON_TOKEN_RSQUARE) {
            depth--;
        }
        if (depth > 0) {
            json_next_token(rd);
        }
    } while (depth > 0 && rd->token != JSON_TOKEN_END);
}

static void official_file(const char *path, int *xx, int *ik)
{
    FILE *f = fopen(path, "r");
    check(f != NULL, "cannot open %s", path);
    if (f == NULL) {
        return;
    }
    JSONReader rd;
    json_init(&rd, path, f);
    static ov_t v;
    int in_vector = 0;
    while (json_next_token(&rd) != JSON_TOKEN_END) {
        if (rd.token == JSON_TOKEN_LBRACE && !in_vector) {
            /* objects directly inside the "vectors" array start a vector (the top object has none of
               our keys and is harmless) */
            memset(&v, 0, sizeof v);
            in_vector = 1;
            continue;
        }
        if (rd.token == JSON_TOKEN_RBRACE && in_vector) {
            in_vector = 0;
            if (strcmp(v.name, "Noise_XXpsk3_25519_ChaChaPoly_SHA256") == 0 ||
                strcmp(v.name, "Noise_IK_25519_ChaChaPoly_SHA256") == 0) {
                int ok = run_official(&v);
                check(ok, "official vector %s", v.name);
                if (v.name[6] == 'X') {
                    (*xx)++;
                } else {
                    (*ik)++;
                }
            }
            continue;
        }
        if (rd.token != JSON_TOKEN_STRING || !in_vector) {
            continue;
        }
        const char *k = rd.str_value;
        if (strcmp(k, "vectors") == 0) {
            json_next_token(&rd); /* ':' */
            json_next_token(&rd); /* '[' */
            in_vector = 0;
            continue;
        }
        char key[48];
        snprintf(key, sizeof key, "%s", k);
        if (strcmp(key, "messages") == 0) {
            json_next_token(&rd);
            json_next_token(&rd); /* '[' */
            while (json_next_token(&rd) == JSON_TOKEN_LBRACE || rd.token == JSON_TOKEN_COMMA) {
                if (rd.token == JSON_TOKEN_COMMA) {
                    continue;
                }
                while (json_next_token(&rd) == JSON_TOKEN_STRING || rd.token == JSON_TOKEN_COMMA) {
                    if (rd.token == JSON_TOKEN_COMMA) {
                        continue;
                    }
                    char mk[24];
                    snprintf(mk, sizeof mk, "%s", rd.str_value);
                    const char *val = json_str(&rd);
                    if (v.nmsg < OV_MSG && val != NULL) {
                        if (strcmp(mk, "payload") == 0) {
                            v.pay_len[v.nmsg] = unhex(val, v.pay[v.nmsg], sizeof v.pay[0]);
                        } else if (strcmp(mk, "ciphertext") == 0) {
                            v.ct_len[v.nmsg] = unhex(val, v.ct[v.nmsg], sizeof v.ct[0]);
                        }
                    }
                }
                if (v.nmsg < OV_MSG) {
                    v.nmsg++;
                }
            }
            continue;
        }
        if (strcmp(key, "init_psks") == 0 || strcmp(key, "resp_psks") == 0) {
            json_next_token(&rd);
            json_next_token(&rd); /* '[' */
            json_next_token(&rd);
            if (rd.token == JSON_TOKEN_STRING) {
                if (key[0] == 'i') {
                    v.has_ipsk = unhex(rd.str_value, v.ipsk, sizeof v.ipsk) == 32;
                } else {
                    v.has_rpsk = unhex(rd.str_value, v.rpsk, sizeof v.rpsk) == 32;
                }
            }
            while (rd.token != JSON_TOKEN_RSQUARE && rd.token != JSON_TOKEN_END) {
                json_next_token(&rd);
            }
            continue;
        }
        json_next_token(&rd); /* ':' */
        json_next_token(&rd);
        if (rd.token != JSON_TOKEN_STRING) {
            skip_value(&rd);
            continue;
        }
        const char *val = rd.str_value;
        if (strcmp(key, "protocol_name") == 0 || strcmp(key, "name") == 0) {
            snprintf(v.name, sizeof v.name, "%s", val);
        } else if (strcmp(key, "init_prologue") == 0) {
            v.ipro_len = unhex(val, v.ipro, sizeof v.ipro);
        } else if (strcmp(key, "resp_prologue") == 0) {
            v.rpro_len = unhex(val, v.rpro, sizeof v.rpro);
        } else if (strcmp(key, "init_static") == 0) {
            unhex(val, v.is, sizeof v.is);
        } else if (strcmp(key, "init_ephemeral") == 0) {
            unhex(val, v.ie, sizeof v.ie);
        } else if (strcmp(key, "init_remote_static") == 0) {
            v.has_irs = unhex(val, v.irs, sizeof v.irs) == 32;
        } else if (strcmp(key, "resp_static") == 0) {
            unhex(val, v.rs, sizeof v.rs);
        } else if (strcmp(key, "resp_ephemeral") == 0) {
            unhex(val, v.re, sizeof v.re);
        } else if (strcmp(key, "handshake_hash") == 0) {
            v.hh_len = unhex(val, v.hh, sizeof v.hh);
        }
    }
    check(rd.errors == 0, "JSON errors in %s", path);
    json_free(&rd);
    fclose(f);
}

/* ---- 2./3. line vectors ---------------------------------------------------------------------- */

static const char *tok(const char *line, const char *key, char *out, size_t cap)
{
    char pat[48];
    snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(pat);
    size_t n = strcspn(p, " \n");
    if (n >= cap) {
        return NULL;
    }
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

static int session_vectors(const char *dir)
{
    char path[600];
    snprintf(path, sizeof path, "%s/ble_session_v1.txt", dir);
    FILE *f = fopen(path, "r");
    check(f != NULL, "cannot open %s", path);
    if (f == NULL) {
        return 0;
    }
    static char line[4096];
    char v[1200];
    int n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        int is_psk = strncmp(line, "psk ", 4) == 0, is_not = strncmp(line, "psk_not ", 8) == 0;
        if (!is_psk && !is_not) {
            continue;
        }
        uint8_t setup[16], token[16], want[32], wrong[32], got[32];
        int purpose = atoi(tok(line, "purpose", v, sizeof v));
        size_t sl = unhex(tok(line, "setup", v, sizeof v), setup, 16);
        size_t tl = unhex(tok(line, "token", v, sizeof v), token, 16);
        unhex(tok(line, "psk", v, sizeof v), want, 32);
        int rc = dspn_psk((uint8_t)purpose, setup, tl == 16 ? token : NULL, got);
        if (purpose == 1) {
            check(rc == DSPN_E_ARG, "SESSION has no PSK (record %d)", n); /* IK: the PSK is not used */
        } else {
            check(sl == 16 && rc == DSPN_OK && memcmp(got, want, 32) == 0, "psk record %d", n);
        }
        if (is_not && purpose != 1) {
            unhex(tok(line, "wrong", v, sizeof v), wrong, 32);
            check(memcmp(got, wrong, 32) != 0, "psk_not record %d produced the wrong value", n);
        }
        n++;
    }
    fclose(f);
    return n;
}

#define HV_F 8
typedef struct {
    char id[24];
    int purpose;
    uint8_t pro[64], psk[32], ps[32], ppub[32], pe[32], ds[32], dpub[32], de[32], hash[32];
    size_t pro_len;
    struct {
        int n, d2p, enc;
        uint8_t b[600];
        size_t len;
        uint8_t header[8], plain[512];
        size_t plain_len;
    } f[HV_F];
    int nf;
} hv_t;

static hv_t HV[8];
static int g_nhv;

static hv_t *hv(const char *id)
{
    for (int i = 0; i < g_nhv; i++) {
        if (strcmp(HV[i].id, id) == 0) {
            return &HV[i];
        }
    }
    return NULL;
}

/* runs transcript h; returns the number of matching frames (phone and device) */
static int run_transcript(hv_t *h, dspn_cs **phone_cs_out, dspn_cs **dev_cs_out)
{
    int pattern = h->purpose == 1 ? DSPN_IK : DSPN_XXPSK3;
    dspn_hs *p = NULL, *d = NULL;
    dspn_cs *pc = NULL, *dc = NULL;
    int ok = dspn_hs_create(&p, pattern, 1, h->pro, h->pro_len, h->ps, pattern == DSPN_IK ? h->dpub : NULL,
                            pattern == DSPN_XXPSK3 ? h->psk : NULL, h->pe) == DSPN_OK &&
             dspn_hs_create(&d, pattern, 0, h->pro, h->pro_len, h->ds, NULL, pattern == DSPN_XXPSK3 ? h->psk : NULL,
                            h->de) == DSPN_OK;
    check(ok, "%s: handshake objects", h->id);
    int matched = 0;
    uint8_t out[700], pl[700];
    size_t n = 0, pn = 0;
    for (int i = 0; ok && i < h->nf; i++) {
        if (!h->f[i].enc) {
            /* HS frame: header (8) + [purpose (requests)] + msg_index + noise */
            size_t off = h->f[i].d2p ? 9 : 10;
            dspn_hs *w = h->f[i].d2p ? d : p, *rd = h->f[i].d2p ? p : d;
            const uint8_t *payload = NULL;
            uint8_t name[40];
            size_t name_len = 0;
            if (!h->f[i].d2p && h->f[i].n == 3) {
                /* XX message 3 payload = the name record; recover it by decrypting on the device side */
                ok = 0;
            }
            if (!h->f[i].d2p && h->f[i].n == 3) {
                /* read first (device), then reproduce with the same payload (deterministic) */
                ok = dspn_hs_read(rd, h->f[i].b + off, h->f[i].len - off, name, sizeof name, &name_len) == DSPN_OK;
                check(ok, "%s: device reads message 3", h->id);
                payload = name;
                /* the phone writes message 3 with the decrypted payload; must equal the vector bytes */
                ok = ok && dspn_hs_write(w, payload, name_len, out, sizeof out, &n) == DSPN_OK && n == h->f[i].len - off &&
                     memcmp(out, h->f[i].b + off, n) == 0;
                check(ok, "%s: phone message 3 equals the vector", h->id);
            } else {
                ok = dspn_hs_write(w, NULL, 0, out, sizeof out, &n) == DSPN_OK && n == h->f[i].len - off &&
                     memcmp(out, h->f[i].b + off, n) == 0;
                check(ok, "%s: frame %d (%s) equals the vector", h->id, h->f[i].n, h->f[i].d2p ? "device" : "phone");
                ok = ok && dspn_hs_read(rd, out, n, pl, sizeof pl, &pn) == DSPN_OK && pn == 0;
            }
            matched += ok;
            if (ok && dspn_hs_finished(p)) {
                uint8_t h1[32], h2[32];
                ok = dspn_hs_finished(d) && dspn_hs_hash(p, h1) == DSPN_OK && dspn_hs_hash(d, h2) == DSPN_OK &&
                     memcmp(h1, h->hash, 32) == 0 && memcmp(h2, h->hash, 32) == 0;
                check(ok, "%s: handshake hash", h->id);
                uint8_t rsp[32], rsd[32];
                ok = ok && dspn_hs_remote_static(p, rsp) == DSPN_OK && memcmp(rsp, h->dpub, 32) == 0 &&
                     dspn_hs_remote_static(d, rsd) == DSPN_OK && memcmp(rsd, h->ppub, 32) == 0;
                check(ok, "%s: remote static keys", h->id);
                ok = ok && dspn_hs_split(p, &pc) == DSPN_OK && dspn_hs_split(d, &dc) == DSPN_OK;
            }
        } else {
            dspn_cs *w = h->f[i].d2p ? dc : pc, *rd = h->f[i].d2p ? pc : dc;
            ok = w != NULL &&
                 dspn_cs_encrypt(w, h->f[i].header, 8, h->f[i].plain, h->f[i].plain_len, out, sizeof out, &n) == DSPN_OK &&
                 n + 8 == h->f[i].len && memcmp(out, h->f[i].b + 8, n) == 0 &&
                 dspn_cs_decrypt(rd, h->f[i].b, 8, h->f[i].b + 8, n, pl, sizeof pl, &pn) == DSPN_OK &&
                 pn == h->f[i].plain_len && memcmp(pl, h->f[i].plain, pn) == 0;
            check(ok, "%s: encrypted frame %d", h->id, h->f[i].n);
            matched += ok;
        }
    }
    dspn_hs_free(p);
    dspn_hs_free(d);
    if (phone_cs_out != NULL) {
        *phone_cs_out = pc;
    } else {
        dspn_cs_free(pc);
    }
    if (dev_cs_out != NULL) {
        *dev_cs_out = dc;
    } else {
        dspn_cs_free(dc);
    }
    return matched;
}

static void handshake_vectors(const char *dir, int *transcripts, int *frames, int *negatives)
{
    char path[600];
    snprintf(path, sizeof path, "%s/ble_handshake_v1.txt", dir);
    FILE *f = fopen(path, "r");
    check(f != NULL, "cannot open %s", path);
    if (f == NULL) {
        return;
    }
    static char line[4096];
    char v[1500];
    /* pass 1: transcripts */
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "hs ", 3) == 0 && g_nhv < 8) {
            hv_t *h = &HV[g_nhv++];
            memset(h, 0, sizeof *h);
            snprintf(h->id, sizeof h->id, "%s", tok(line, "id", v, sizeof v));
            h->purpose = atoi(tok(line, "purpose", v, sizeof v));
            h->pro_len = unhex(tok(line, "prologue", v, sizeof v), h->pro, sizeof h->pro);
            unhex(tok(line, "psk", v, sizeof v), h->psk, 32);
            unhex(tok(line, "phone_static", v, sizeof v), h->ps, 32);
            unhex(tok(line, "phone_static_pub", v, sizeof v), h->ppub, 32);
            unhex(tok(line, "phone_ephemeral", v, sizeof v), h->pe, 32);
            unhex(tok(line, "device_static", v, sizeof v), h->ds, 32);
            unhex(tok(line, "device_static_pub", v, sizeof v), h->dpub, 32);
            unhex(tok(line, "device_ephemeral", v, sizeof v), h->de, 32);
        } else if (strncmp(line, "hash ", 5) == 0) {
            hv_t *h = hv(tok(line, "id", v, sizeof v));
            if (h != NULL) {
                unhex(tok(line, "handshake_hash", v, sizeof v), h->hash, 32);
            }
        } else if (strncmp(line, "frame ", 6) == 0 || strncmp(line, "enc ", 4) == 0) {
            hv_t *h = hv(tok(line, "id", v, sizeof v));
            if (h == NULL || h->nf >= HV_F) {
                check(0, "frame record without transcript");
                continue;
            }
            int k = h->nf++;
            h->f[k].n = atoi(tok(line, "n", v, sizeof v));
            h->f[k].d2p = strcmp(tok(line, "dir", v, sizeof v), "d2p") == 0;
            h->f[k].enc = line[0] == 'e';
            h->f[k].len = unhex(tok(line, "bytes", v, sizeof v), h->f[k].b, sizeof h->f[k].b);
            if (h->f[k].enc) {
                unhex(tok(line, "header", v, sizeof v), h->f[k].header, 8);
                h->f[k].plain_len = unhex(tok(line, "plain", v, sizeof v), h->f[k].plain, sizeof h->f[k].plain);
            }
        }
    }
    for (int i = 0; i < g_nhv; i++) {
        *frames += run_transcript(&HV[i], NULL, NULL);
        (*transcripts)++;
    }
    /* pass 2: negatives (Noise-level; authorization-level cases checked as far as Noise allows) */
    rewind(f);
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "neg ", 4) != 0) {
            continue;
        }
        char id[48], base_id[24], expect[24], change[200];
        snprintf(id, sizeof id, "%s", tok(line, "id", v, sizeof v));
        snprintf(base_id, sizeof base_id, "%s", tok(line, "base", v, sizeof v));
        snprintf(expect, sizeof expect, "%s", tok(line, "expect", v, sizeof v));
        snprintf(change, sizeof change, "%s", tok(line, "change", v, sizeof v));
        int frame = atoi(tok(line, "frame", v, sizeof v));
        static uint8_t b[700];
        size_t bl = unhex(tok(line, "bytes", v, sizeof v), b, sizeof b);
        hv_t *h = hv(base_id);
        check(h != NULL, "neg %s: base", id);
        if (h == NULL) {
            continue;
        }
        int pattern = h->purpose == 1 ? DSPN_IK : DSPN_XXPSK3;
        uint8_t out[700], pl[700];
        size_t n = 0, pn = 0;
        int result = 0;
        if (frame <= 3 && strncmp(expect, "device_", 7) == 0) {
            dspn_hs *d = NULL;
            dspn_hs_create(&d, pattern, 0, h->pro, h->pro_len, h->ds, NULL, pattern == DSPN_XXPSK3 ? h->psk : NULL, h->de);
            int read_ok = 1;
            if (frame == 3) { /* base message 1, then the device's message 2 */
                read_ok = dspn_hs_read(d, h->f[0].b + 10, h->f[0].len - 10, pl, sizeof pl, &pn) == DSPN_OK &&
                          dspn_hs_write(d, NULL, 0, out, sizeof out, &n) == DSPN_OK;
            }
            int rejected = read_ok && dspn_hs_read(d, b + 10, bl - 10, pl, sizeof pl, &pn) != DSPN_OK;
            if (strcmp(id, "session_unknown_phone_key") == 0) {
                /* Noise accepts; the device's authorization refuses the unknown static key */
                uint8_t rsd[32];
                result = !rejected && dspn_hs_remote_static(d, rsd) == DSPN_OK && memcmp(rsd, h->ppub, 32) != 0;
            } else {
                result = rejected;
            }
            dspn_hs_free(d);
        } else if (strncmp(expect, "device_drops", 12) == 0 || strcmp(expect, "phone_rejects") == 0) {
            if (frame >= 4) { /* tampered encrypted frame: the receiver's AEAD fails */
                dspn_cs *pc = NULL, *dc = NULL;
                run_transcript(h, &pc, &dc);
                /* fresh session state: rebuild and consume nothing before the tampered frame */
                dspn_cs_free(pc);
                dspn_cs_free(dc);
                dspn_hs *p = NULL, *d = NULL;
                dspn_hs_create(&p, pattern, 1, h->pro, h->pro_len, h->ps, NULL, h->psk, h->pe);
                dspn_hs_create(&d, pattern, 0, h->pro, h->pro_len, h->ds, NULL, h->psk, h->de);
                for (int k = 0; k < 3; k++) {
                    dspn_hs *w = h->f[k].d2p ? d : p, *rd = h->f[k].d2p ? p : d;
                    size_t off = h->f[k].d2p ? 9 : 10;
                    if (k == 2) {
                        dspn_hs_read(rd, h->f[k].b + off, h->f[k].len - off, pl, sizeof pl, &pn);
                        dspn_hs_write(w, pl, pn, out, sizeof out, &n);
                    } else {
                        dspn_hs_write(w, NULL, 0, out, sizeof out, &n);
                        dspn_hs_read(rd, out, n, pl, sizeof pl, &pn);
                    }
                }
                dspn_hs_split(p, &pc);
                dspn_hs_split(d, &dc);
                dspn_hs_free(p);
                dspn_hs_free(d);
                dspn_cs *rd = strcmp(expect, "phone_rejects") == 0 ? pc : dc;
                result = bl < 8 + DSPN_TAG_LEN ||
                         dspn_cs_decrypt(rd, b, 8, b + 8, bl - 8, pl, sizeof pl, &pn) != DSPN_OK;
                dspn_cs_free(pc);
                dspn_cs_free(dc);
            } else { /* phone with a wrong prologue (from `change`) fails reading message 2 */
                uint8_t wpro[64];
                size_t wl = unhex(strchr(change, '=') + 1, wpro, sizeof wpro);
                dspn_hs *p = NULL;
                dspn_hs_create(&p, pattern, 1, wpro, wl, h->ps, NULL, h->psk, h->pe);
                dspn_hs_write(p, NULL, 0, out, sizeof out, &n);
                result = dspn_hs_read(p, b + 9, bl - 9, pl, sizeof pl, &pn) != DSPN_OK;
                dspn_hs_free(p);
            }
        } else if (strcmp(expect, "phone_aborts") == 0) {
            /* the phone reads message 2 fine, but the device key differs from the invitation's */
            uint8_t other[32];
            unhex(strchr(change, '=') + 1, other, 32);
            dspn_hs *p = NULL;
            dspn_hs_create(&p, pattern, 1, h->pro, h->pro_len, h->ps, NULL, h->psk, h->pe);
            dspn_hs_write(p, NULL, 0, out, sizeof out, &n);
            uint8_t rs[32];
            result = dspn_hs_read(p, b + 9, bl - 9, pl, sizeof pl, &pn) == DSPN_OK &&
                     dspn_hs_remote_static(p, rs) == DSPN_OK && memcmp(rs, other, 32) != 0 &&
                     memcmp(rs, h->dpub, 32) == 0;
            dspn_hs_free(p);
        }
        check(result, "negative %s (%s)", id, expect);
        (*negatives)++;
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <noise-c tests/vector dir> <test-vectors dir>\n", argv[0]);
        return 2;
    }
    check(dspn_init() == DSPN_OK, "sodium_init");
    int xx = 0, ik = 0;
    char path[600];
    snprintf(path, sizeof path, "%s/cacophony.txt", argv[1]);
    official_file(path, &xx, &ik);
    snprintf(path, sizeof path, "%s/noise-c-basic.txt", argv[1]);
    official_file(path, &xx, &ik);
    check(xx >= 1 && ik >= 2, "official vectors: XXpsk3 %d, IK %d (need >= 1, >= 2)", xx, ik);
    int psk = session_vectors(argv[2]);
    check(psk >= 30, "PSK records: %d", psk);
    int transcripts = 0, frames = 0, negatives = 0;
    handshake_vectors(argv[2], &transcripts, &frames, &negatives);
    check(transcripts == 6 && frames == 33 && negatives == 10, "transcripts %d, negatives %d", transcripts, negatives);
    printf("dspnoise vectors: official XXpsk3 %d IK %d (0 skipped); ble_session PSK records %d; "
           "ble_handshake transcripts %d, frames %d, negatives %d; rng %s; %d checks, %d failed\n",
           xx, ik, psk, transcripts, frames, negatives, dspn_rng_name(), g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
