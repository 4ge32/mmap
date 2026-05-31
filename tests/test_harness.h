/*
 * test_harness.h - Zero-dependency unit-test harness.
 *
 * No external framework: each test file defines test_* functions, calls them
 * from main(), and returns TEST_SUMMARY(). Failures are counted and reported
 * with file:line; a nonzero exit signals failure to `make test`.
 *
 * ASSERT_WF embeds the runtime well-formedness oracle, giving defense in
 * depth alongside the Frama-C/WP proof of the same invariant.
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <inttypes.h>
#include "mm_internal.h"

static int mm_tests_run = 0;
static int mm_tests_failed = 0;
static const char *mm_current_test = "(none)";

#define RUN_TEST(fn)                                            \
    do {                                                        \
        mm_current_test = #fn;                                  \
        fn();                                                   \
    } while (0)

#define CHECK(cond, ...)                                        \
    do {                                                        \
        mm_tests_run++;                                         \
        if (!(cond)) {                                          \
            mm_tests_failed++;                                  \
            fprintf(stderr, "FAIL [%s] %s:%d: ",                \
                    mm_current_test, __FILE__, __LINE__);       \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

#define ASSERT_TRUE(c)        CHECK((c), "expected true: %s", #c)
#define ASSERT_FALSE(c)       CHECK(!(c), "expected false: %s", #c)

#define ASSERT_EQ_U64(a, b)                                     \
    CHECK((uint64_t)(a) == (uint64_t)(b),                       \
          "%s == %s (%" PRIu64 " vs %" PRIu64 ")",              \
          #a, #b, (uint64_t)(a), (uint64_t)(b))

#define ASSERT_EQ_INT(a, b)                                     \
    CHECK((long)(a) == (long)(b),                               \
          "%s == %s (%ld vs %ld)", #a, #b, (long)(a), (long)(b))

#define ASSERT_STATUS(a, b)                                     \
    CHECK((mm_status)(a) == (mm_status)(b),                     \
          "status %s == %s (%d vs %d)", #a, #b, (int)(a), (int)(b))

#define ASSERT_WF(as)                                           \
    CHECK(as_check_wf(&(as)), "as_wf violated: %s", #as)

#define TEST_SUMMARY()                                          \
    (fprintf(stderr, "%s: %d checks, %d failed\n",              \
             __FILE__, mm_tests_run, mm_tests_failed),          \
     mm_tests_failed == 0 ? 0 : 1)

#endif /* TEST_HARNESS_H */
