#include "dsp_identity.h"

#include <stdio.h>

static int is_present(const char *s)
{
    return s != NULL && s[0] != '\0';
}

dsp_identity_result_t dsp_identity_format(const dsp_identity_t *id, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return DSP_IDENTITY_ERR_ARG;
    }
    buf[0] = '\0';
    if (id == NULL || !is_present(id->fw_version) || !is_present(id->target) ||
        !is_present(id->sdk_version)) {
        return DSP_IDENTITY_ERR_ARG;
    }

    int n = snprintf(buf, buf_len, "%s firmware %s (%s, %s)", DSP_PRODUCT_NAME, id->fw_version,
                     id->target, id->sdk_version);
    if (n < 0) {
        buf[0] = '\0';
        return DSP_IDENTITY_ERR_ARG;
    }
    if ((size_t)n >= buf_len) {
        return DSP_IDENTITY_ERR_TRUNCATED;
    }
    return DSP_IDENTITY_OK;
}
