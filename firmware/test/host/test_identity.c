#include "dsp_identity.h"
#include "dsp_test.h"

#include <string.h>

static const dsp_identity_t VALID = {
    .fw_version = "0.1.0-dev",
    .target = "esp32c3",
    .sdk_version = "host",
};

static void test_formats_full_identity(void)
{
    char buf[128];
    DSP_CHECK_EQ_INT(DSP_IDENTITY_OK, dsp_identity_format(&VALID, buf, sizeof buf));
    DSP_CHECK_EQ_STR("Dispenser V2 firmware 0.1.0-dev (esp32c3, host)", buf);
}

static void test_exact_fit_is_not_truncated(void)
{
    const char *expected = "Dispenser V2 firmware 0.1.0-dev (esp32c3, host)";
    char buf[64];
    size_t exact = strlen(expected) + 1;
    DSP_CHECK_EQ_INT(DSP_IDENTITY_OK, dsp_identity_format(&VALID, buf, exact));
    DSP_CHECK_EQ_STR(expected, buf);
}

static void test_small_buffer_truncates_and_terminates(void)
{
    char buf[8];
    memset(buf, 'X', sizeof buf);
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_TRUNCATED, dsp_identity_format(&VALID, buf, sizeof buf));
    DSP_CHECK_EQ_STR("Dispens", buf);
}

static void test_rejects_missing_arguments(void)
{
    char buf[32] = "unchanged";
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(NULL, buf, sizeof buf));
    DSP_CHECK_EQ_STR("", buf);
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(&VALID, NULL, 16));
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(&VALID, buf, 0));
}

static void test_rejects_empty_or_null_fields(void)
{
    char buf[64];
    dsp_identity_t id = VALID;
    id.fw_version = "";
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(&id, buf, sizeof buf));
    DSP_CHECK_EQ_STR("", buf);

    id = VALID;
    id.target = NULL;
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(&id, buf, sizeof buf));

    id = VALID;
    id.sdk_version = NULL;
    DSP_CHECK_EQ_INT(DSP_IDENTITY_ERR_ARG, dsp_identity_format(&id, buf, sizeof buf));
}

static void test_version_file_reaches_host_build(void)
{
    /* DSP_FW_VERSION is injected by test/host/CMakeLists.txt from firmware/version.txt,
     * the same file ESP-IDF uses for the app version. */
    DSP_CHECK(strlen(DSP_FW_VERSION) > 0);
    DSP_CHECK(strchr(DSP_FW_VERSION, '\n') == NULL);
}

int main(void)
{
    DSP_RUN(test_formats_full_identity);
    DSP_RUN(test_exact_fit_is_not_truncated);
    DSP_RUN(test_small_buffer_truncates_and_terminates);
    DSP_RUN(test_rejects_missing_arguments);
    DSP_RUN(test_rejects_empty_or_null_fields);
    DSP_RUN(test_version_file_reaches_host_build);
    return dsp_test_finish();
}
