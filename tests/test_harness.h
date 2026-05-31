/*
 * test_harness.h - Zero-dependency unit-test harness.
 *
 * No external framework: each test file declares a suite name with
 * TEST_SUITE(), defines test_* functions, runs them via RUN_TEST(fn, "desc"),
 * and returns TEST_SUMMARY(). Each test carries a human-readable description
 * so output is self-explanatory.
 *
 * Output:
 *  - Always: a per-test "[PASS]/[FAIL] <desc> (<n> checks)" line, and a final
 *    suite summary. Failing checks print file:line + message.
 *  - Under GitHub Actions (GITHUB_ACTIONS set): ::group::/::endgroup:: folds
 *    and ::error:: annotations.
 *  - When JUNIT_XML names a path: a JUnit XML report (one <testcase> per test)
 *    so a CI reporter can surface each test in the Checks UI.
 *
 * ASSERT_WF embeds the runtime well-formedness oracle, giving defense in depth
 * alongside the Frama-C/WP proof of the same invariant.
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "mm_internal.h"

#define MM_MAX_TESTS 64
#define MM_MSG_CAP   1024

struct mm_test_rec {
    const char *name;
    int checks;
    int fails;
    char msg[MM_MSG_CAP]; /* accumulated failure lines (for JUnit <failure>) */
};

static struct mm_test_rec mm_recs[MM_MAX_TESTS];
static int mm_test_count = 0;          /* number of tests registered */
static int mm_tests_run = 0;           /* total checks across all tests */
static int mm_tests_failed = 0;        /* total failed checks */
static struct mm_test_rec *mm_cur = NULL;
static const char *mm_suite_name = "suite";

static int mm__in_ci(void)
{
    return getenv("GITHUB_ACTIONS") != NULL;
}

/* Bounded append of `s` onto NUL-terminated `dst` (capacity `cap`). Hand-rolled
 * (not snprintf) so the compiler raises no -Wformat-truncation on accumulation. */
static void mm__append(char *dst, size_t cap, const char *s)
{
    size_t i = strlen(dst);
    while (*s != '\0' && i + 1 < cap)
        dst[i++] = *s++;
    dst[i] = '\0';
}

/* Record + report a single failed check. */
static void mm__fail(const char *file, int line, const char *fmt, ...)
{
    char buf[MM_MSG_CAP];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    char num[16];
    (void)snprintf(num, sizeof num, "%d", line);

    const char *tn = mm_cur ? mm_cur->name : "(none)";
    fprintf(stderr, "FAIL [%s] %s:%s: %s\n", tn, file, num, buf);

    if (mm_cur != NULL) {
        char *m = mm_cur->msg;
        size_t cap = sizeof mm_cur->msg;
        mm__append(m, cap, file);
        mm__append(m, cap, ":");
        mm__append(m, cap, num);
        mm__append(m, cap, ": ");
        mm__append(m, cap, buf);
        mm__append(m, cap, "\n");
    }
    if (mm__in_ci())
        fprintf(stderr, "::error file=%s,line=%s::%s\n", file, num, buf);
}

#define RUN_TEST(fn, desc)                                          \
    do {                                                            \
        if (mm_test_count < MM_MAX_TESTS) {                         \
            mm_cur = &mm_recs[mm_test_count++];                     \
            mm_cur->name = (desc);                                  \
            mm_cur->checks = 0;                                     \
            mm_cur->fails = 0;                                      \
            mm_cur->msg[0] = '\0';                                  \
            if (mm__in_ci())                                        \
                fprintf(stderr, "::group::%s\n", (desc));           \
            fn();                                                   \
            if (mm_cur->fails != 0)                                 \
                fprintf(stderr, "[FAIL] %s (%d/%d checks failed)\n",\
                        (desc), mm_cur->fails, mm_cur->checks);     \
            else                                                    \
                fprintf(stderr, "[PASS] %s (%d checks)\n",          \
                        (desc), mm_cur->checks);                    \
            if (mm__in_ci())                                        \
                fprintf(stderr, "::endgroup::\n");                  \
        }                                                           \
    } while (0)

#define CHECK(cond, ...)                                            \
    do {                                                            \
        mm_tests_run++;                                             \
        if (mm_cur != NULL)                                         \
            mm_cur->checks++;                                       \
        if (!(cond)) {                                              \
            mm_tests_failed++;                                      \
            if (mm_cur != NULL)                                     \
                mm_cur->fails++;                                    \
            mm__fail(__FILE__, __LINE__, __VA_ARGS__);             \
        }                                                           \
    } while (0)

#define ASSERT_TRUE(c)        CHECK((c), "expected true: %s", #c)
#define ASSERT_FALSE(c)       CHECK(!(c), "expected false: %s", #c)

#define ASSERT_EQ_U64(a, b)                                         \
    CHECK((uint64_t)(a) == (uint64_t)(b),                           \
          "%s == %s (%" PRIu64 " vs %" PRIu64 ")",                  \
          #a, #b, (uint64_t)(a), (uint64_t)(b))

#define ASSERT_EQ_INT(a, b)                                         \
    CHECK((long)(a) == (long)(b),                                   \
          "%s == %s (%ld vs %ld)", #a, #b, (long)(a), (long)(b))

#define ASSERT_STATUS(a, b)                                         \
    CHECK((mm_status)(a) == (mm_status)(b),                         \
          "status %s == %s (%d vs %d)", #a, #b, (int)(a), (int)(b))

#define ASSERT_WF(as)                                               \
    CHECK(as_check_wf(&(as)), "as_wf violated: %s", #as)

/* Set the suite name (call once at the top of main, before RUN_TEST). */
#define TEST_SUITE(name) (mm_suite_name = (name))

/* Write `s` to f with XML metacharacters escaped. */
static void mm__xml_escape(FILE *f, const char *s)
{
    for (; *s != '\0'; s++) {
        switch (*s) {
        case '&':  fputs("&amp;", f);  break;
        case '<':  fputs("&lt;", f);   break;
        case '>':  fputs("&gt;", f);   break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:   fputc(*s, f);       break;
        }
    }
}

static int mm__failed_tests(void)
{
    int n = 0;
    for (int i = 0; i < mm_test_count; i++)
        if (mm_recs[i].fails != 0)
            n++;
    return n;
}

/* Emit a JUnit XML report to `path`. */
static void mm__write_junit(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "WARN: cannot write JUnit XML to %s\n", path);
        return;
    }
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", f);
    fputs("<testsuites>\n", f);
    fputs("  <testsuite name=\"", f);
    mm__xml_escape(f, mm_suite_name);
    fprintf(f, "\" tests=\"%d\" failures=\"%d\">\n",
            mm_test_count, mm__failed_tests());

    for (int i = 0; i < mm_test_count; i++) {
        struct mm_test_rec *r = &mm_recs[i];
        fputs("    <testcase name=\"", f);
        mm__xml_escape(f, r->name);
        fputs("\" classname=\"", f);
        mm__xml_escape(f, mm_suite_name);
        fprintf(f, "\" assertions=\"%d\"", r->checks);
        if (r->fails != 0) {
            fprintf(f, ">\n      <failure message=\"%d/%d checks failed\">",
                    r->fails, r->checks);
            mm__xml_escape(f, r->msg);
            fputs("</failure>\n    </testcase>\n", f);
        } else {
            fputs("/>\n", f);
        }
    }

    fputs("  </testsuite>\n</testsuites>\n", f);
    fclose(f);
}

static void mm__maybe_write_junit(void)
{
    const char *path = getenv("JUNIT_XML");
    if (path != NULL && path[0] != '\0')
        mm__write_junit(path);
}

#define TEST_SUMMARY()                                              \
    (mm__maybe_write_junit(),                                       \
     fprintf(stderr, "%s: %d tests, %d checks, %d failed\n",        \
             mm_suite_name, mm_test_count, mm_tests_run,            \
             mm_tests_failed),                                      \
     mm_tests_failed == 0 ? 0 : 1)

#endif /* TEST_HARNESS_H */
