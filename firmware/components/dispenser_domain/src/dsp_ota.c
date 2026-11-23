#include "dsp_ota.h"

#include <stdio.h>
#include <string.h>

size_t dsp_ota_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* One decimal component without leading zeros, <= 65535. Advances *p. */
static bool component(const char *s, size_t len, size_t *p, uint16_t *out)
{
    size_t i = *p;
    uint32_t v = 0;
    size_t digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (digits > 0 && v == 0) {
            return false; /* leading zero */
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 65535u) {
            return false;
        }
        i++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = (uint16_t)v;
    *p = i;
    return true;
}

static bool parse_core(const char *s, size_t len, size_t *p, dsp_semver_t *out)
{
    memset(out, 0, sizeof *out);
    if (!component(s, len, p, &out->major) || *p >= len || s[(*p)++] != '.' ||
        !component(s, len, p, &out->minor) || *p >= len || s[(*p)++] != '.' ||
        !component(s, len, p, &out->patch)) {
        return false;
    }
    return true;
}

bool dsp_ota_parse_release(const char *s, size_t len, dsp_semver_t *out)
{
    size_t p = 0;
    dsp_semver_t v;
    if (s == NULL || out == NULL || len == 0 || len > DSP_OTA_VERSION_MAX || !parse_core(s, len, &p, &v) ||
        p != len) {
        return false;
    }
    *out = v;
    return true;
}

bool dsp_ota_parse_running(const char *s, dsp_semver_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    size_t len = dsp_ota_strnlen(s, 32);
    size_t p = 0;
    dsp_semver_t v;
    if (!parse_core(s, len, &p, &v)) {
        return false;
    }
    if (p < len) {
        if (s[p] != '-' || p + 1 >= len) {
            return false;
        }
        v.pre = true;
    }
    *out = v;
    return true;
}

int dsp_ota_compare(const dsp_semver_t *a, const dsp_semver_t *b)
{
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    if (a->pre != b->pre) {
        return a->pre ? -1 : 1;
    }
    return 0;
}

bool dsp_ota_id_ok(const uint8_t *s, size_t len, size_t max)
{
    if (s == NULL || len == 0 || len > max) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    /* "." and ".." would change the URL path */
    if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.')) {
        return false;
    }
    return true;
}

static bool base_ok(const char *base)
{
    static const char prefix[] = "https://github.com/";
    static const char suffix[] = "/releases/download/";
    if (base == NULL) {
        return false;
    }
    size_t n = dsp_ota_strnlen(base, DSP_OTA_BASE_MAX + 1);
    size_t pl = sizeof prefix - 1, sl = sizeof suffix - 1;
    if (n > DSP_OTA_BASE_MAX || n <= pl + sl + 2 || memcmp(base, prefix, pl) != 0 ||
        memcmp(base + n - sl, suffix, sl) != 0) {
        return false;
    }
    /* exactly "<owner>/<repo>" between them, both id-safe */
    const char *mid = base + pl;
    size_t ml = n - pl - sl;
    const char *slash = memchr(mid, '/', ml);
    if (slash == NULL) {
        return false;
    }
    size_t ol = (size_t)(slash - mid), rl = ml - ol - 1;
    return dsp_ota_id_ok((const uint8_t *)mid, ol, 39) && dsp_ota_id_ok((const uint8_t *)slash + 1, rl, 100);
}

dsp_ota_verdict_t dsp_ota_prepare(const char *base, const char *running, const uint8_t *version,
                                  size_t version_len, const uint8_t *tag, size_t tag_len,
                                  uint32_t size, const uint8_t sha256[32], dsp_ota_job_t *out)
{
    if (out == NULL || sha256 == NULL || !base_ok(base)) {
        return DSP_OTA_V_INVALID_REQUEST;
    }
    memset(out, 0, sizeof *out);
    dsp_semver_t want, have;
    if (version == NULL || !dsp_ota_parse_release((const char *)version, version_len, &want) ||
        !dsp_ota_id_ok(tag, tag_len, DSP_OTA_TAG_MAX) || size == 0 || size > DSP_OTA_SLOT_SIZE) {
        return DSP_OTA_V_INVALID_REQUEST;
    }
    if (!dsp_ota_parse_running(running, &have)) {
        return DSP_OTA_V_INVALID_REQUEST; /* unknown running version: never guess */
    }
    if (dsp_ota_compare(&want, &have) <= 0) {
        return DSP_OTA_V_NOT_NEWER;
    }
    memcpy(out->version, version, version_len);
    memcpy(out->tag, tag, tag_len);
    out->size = size;
    memcpy(out->sha256, sha256, 32);
    int n = snprintf(out->url, sizeof out->url, "%s%s/" DSP_OTA_ASSET_PREFIX "%s" DSP_OTA_ASSET_SUFFIX, base,
                     out->tag, out->version);
    if (n <= 0 || (size_t)n >= sizeof out->url) {
        memset(out, 0, sizeof *out);
        return DSP_OTA_V_INVALID_REQUEST;
    }
    return DSP_OTA_V_ACCEPTED;
}

dsp_ota_verdict_t dsp_ota_check(const dsp_ota_conditions_t *c)
{
    if (c == NULL || !c->capable) {
        return DSP_OTA_V_NOT_CAPABLE;
    }
    if (c->busy) {
        return DSP_OTA_V_BUSY;
    }
    if (!c->wifi_configured) {
        return DSP_OTA_V_NO_WIFI;
    }
    if (!c->clock_trusted || (c->next_dose_known && c->next_dose_at <= c->now + DSP_OTA_DOSE_MARGIN_S)) {
        return DSP_OTA_V_DOSE_SOON;
    }
    if (!c->battery_valid || c->battery_mv < DSP_OTA_MIN_BATTERY_MV || c->battery_low_open) {
        return DSP_OTA_V_BATTERY;
    }
    return DSP_OTA_V_ACCEPTED;
}
