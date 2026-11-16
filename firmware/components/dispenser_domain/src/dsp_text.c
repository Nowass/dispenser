#include "dsp_text.h"

/* Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, max U+10FFFF. */
bool dsp_utf8_valid(const uint8_t *s, size_t n, bool no_controls)
{
    if (s == NULL) {
        return n == 0;
    }
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            if (no_controls && (c < 0x20 || c == 0x7F)) {
                return false;
            }
            i++;
            continue;
        }
        size_t need;
        uint32_t cp;
        uint32_t min;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
            cp = c & 0x1Fu;
            min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            cp = c & 0x0Fu;
            min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            cp = c & 0x07u;
            min = 0x10000;
        } else {
            return false;
        }
        if (n - i - 1 < need) {
            return false;
        }
        for (size_t k = 1; k <= need; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += need + 1;
    }
    return true;
}
