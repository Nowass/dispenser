#ifndef DSP_TEST_H
#define DSP_TEST_H

/*
 * Minimal host test harness (header-only, no external dependency).
 *
 * Usage:
 *   static void test_something(void) { DSP_CHECK(1 + 1 == 2); }
 *   int main(void) { DSP_RUN(test_something); return dsp_test_finish(); }
 *
 * A failed check records the failure and continues with the next check, so one run
 * reports all failures. Exit code is non-zero if any check failed or if no check ran
 * at all (CTest semantics). Counters are per translation unit: each test executable is
 * a single .c file, and CTest runs each executable in its own process.
 * A crash or sanitizer report also ends the process with a non-zero status.
 */

#include <stdio.h>
#include <string.h>

static int dsp_test_checks;
static int dsp_test_failures;
static const char *dsp_test_current = "?";

#define DSP_CHECK(cond)                                                                      \
    do {                                                                                     \
        dsp_test_checks++;                                                                   \
        if (!(cond)) {                                                                       \
            dsp_test_failures++;                                                             \
            fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__,             \
                    dsp_test_current, #cond);                                                \
        }                                                                                    \
    } while (0)

#define DSP_CHECK_EQ_INT(expected, actual)                                                   \
    do {                                                                                     \
        long long dsp_e_ = (long long)(expected);                                            \
        long long dsp_a_ = (long long)(actual);                                              \
        dsp_test_checks++;                                                                   \
        if (dsp_e_ != dsp_a_) {                                                              \
            dsp_test_failures++;                                                             \
            fprintf(stderr, "%s:%d: %s: expected %s == %lld, got %lld\n", __FILE__,          \
                    __LINE__, dsp_test_current, #actual, dsp_e_, dsp_a_);                    \
        }                                                                                    \
    } while (0)

#define DSP_CHECK_EQ_STR(expected, actual)                                                   \
    do {                                                                                     \
        const char *dsp_e_ = (expected);                                                     \
        const char *dsp_a_ = (actual);                                                       \
        dsp_test_checks++;                                                                   \
        if (dsp_e_ == NULL || dsp_a_ == NULL || strcmp(dsp_e_, dsp_a_) != 0) {               \
            dsp_test_failures++;                                                             \
            fprintf(stderr, "%s:%d: %s: expected %s == \"%s\", got \"%s\"\n", __FILE__,      \
                    __LINE__, dsp_test_current, #actual, dsp_e_ ? dsp_e_ : "(null)",         \
                    dsp_a_ ? dsp_a_ : "(null)");                                             \
        }                                                                                    \
    } while (0)

#define DSP_RUN(test_fn)                                                                     \
    do {                                                                                     \
        int dsp_before_ = dsp_test_failures;                                                 \
        dsp_test_current = #test_fn;                                                         \
        test_fn();                                                                           \
        printf("[%s] %s\n", dsp_test_failures == dsp_before_ ? "PASS" : "FAIL", #test_fn);   \
    } while (0)

static inline int dsp_test_finish(void)
{
    printf("%d checks, %d failed\n", dsp_test_checks, dsp_test_failures);
    if (dsp_test_checks == 0) {
        /* A test binary that checks nothing must not count as a pass. */
        fprintf(stderr, "no checks were executed\n");
        return 1;
    }
    return dsp_test_failures == 0 ? 0 : 1;
}

#endif /* DSP_TEST_H */
