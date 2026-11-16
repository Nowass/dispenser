#ifndef DSP_TEXT_H
#define DSP_TEXT_H

/*
 * Text validation shared by the domain model (schedule names, M1.10b.1) and the BLE
 * protocol codec (M1.10a `text` strings), so both apply exactly one rule.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF.
 * no_controls: additionally refuses U+0000-U+001F and U+007F (the protocol's `text` kind).
 * n == 0 is valid; s may be NULL only when n == 0. */
bool dsp_utf8_valid(const uint8_t *s, size_t n, bool no_controls);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TEXT_H */
