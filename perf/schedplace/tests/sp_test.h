/* Minimal self-contained test harness (no third-party dependencies). */
#ifndef SP_TEST_H
#define SP_TEST_H

#include <stdio.h>
#include <string.h>

static int sp_test_checks = 0;
static int sp_test_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++sp_test_checks;                                                        \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++sp_test_failures;                                                  \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        ++sp_test_checks;                                                        \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                 \
            fprintf(stderr, __VA_ARGS__);                                        \
            fputc('\n', stderr);                                                 \
            ++sp_test_failures;                                                  \
        }                                                                        \
    } while (0)

#define CHECK_STR(got, want)                                                     \
    CHECK_MSG(strcmp((got), (want)) == 0, "%s != %s", (got), (want))

static int sp_test_report(const char *name) {
    if (sp_test_failures) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, sp_test_failures,
                sp_test_checks);
        return 1;
    }
    printf("%s: %d checks passed\n", name, sp_test_checks);
    return 0;
}

#endif
