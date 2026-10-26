#include "jogrt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Not in jogrt.h. Jog redeclares this as *const i8. */
extern const int8_t *jogrt_arg_ptr(int32_t i);

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static void test_start_and_args(void) {
    size_t len = 7;

    jogrt_start(0, NULL);
    CHECK(jogrt_argc() == 0);
    CHECK(jogrt_argv(0, &len) == NULL);
    CHECK(len == 0);
    CHECK(jogrt_argv(-1, NULL) == NULL);
    CHECK(jogrt_arg_ptr(0) == NULL);
    CHECK(jogrt_arg_ptr(-1) == NULL);

    jogrt_start(-4, NULL);
    CHECK(jogrt_argc() == 0);
    len = 3;
    CHECK(jogrt_argv(0, &len) == NULL);
    CHECK(len == 0);

    char prog[] = "prog";
    char hello[] = "hello";
    char *args[] = {prog, hello};
    jogrt_start(2, args);
    CHECK(jogrt_argc() == 2);

    len = 0;
    const char *first = (const char *)jogrt_argv(0, &len);
    CHECK(first != NULL && len == 4 && memcmp(first, "prog", 4) == 0);
    CHECK(jogrt_argv(1, NULL) != NULL && memcmp(jogrt_argv(1, NULL), "hello", 5) == 0);

    len = 9;
    const char *second = (const char *)jogrt_argv(1, &len);
    CHECK(second != NULL && len == 5 && memcmp(second, "hello", 5) == 0);
    const int8_t *raw = jogrt_arg_ptr(0);
    CHECK(raw != NULL && memcmp(raw, "prog", 4) == 0);
    CHECK(jogrt_arg_ptr(1) != NULL && memcmp(jogrt_arg_ptr(1), "hello", 5) == 0);
    CHECK(jogrt_arg_ptr(2) == NULL);

    len = 4;
    CHECK(jogrt_argv(2, &len) == NULL);
    CHECK(len == 0);
    len = 4;
    CHECK(jogrt_argv(99, &len) == NULL);
    CHECK(len == 0);
    len = 4;
    CHECK(jogrt_argv(-1, &len) == NULL);
    CHECK(len == 0);

    char *missing[] = {NULL};
    jogrt_start(1, missing);
    CHECK(jogrt_argc() == 1);
    len = 6;
    CHECK(jogrt_argv(0, &len) == NULL);
    CHECK(len == 0);

    jogrt_start(2, NULL);
    CHECK(jogrt_argc() == 2);
    len = 6;
    CHECK(jogrt_argv(0, &len) == NULL);
    CHECK(len == 0);
    CHECK(jogrt_argv(1, NULL) == NULL);

    char empty[] = "";
    char *only_empty[] = {empty};
    jogrt_start(1, only_empty);
    len = 8;
    const char *blank = (const char *)jogrt_argv(0, &len);
    CHECK(blank != NULL && len == 0);
    const int8_t *blank_ptr = jogrt_arg_ptr(0);
    CHECK(blank_ptr != NULL && blank_ptr[0] == 0);

    char replaced[] = "next";
    char *again[] = {replaced};
    jogrt_start(1, again);
    CHECK(jogrt_argc() == 1);
    len = 0;
    const char *next = (const char *)jogrt_argv(0, &len);
    CHECK(next != NULL && len == 4 && memcmp(next, "next", 4) == 0);
}

static void test_finish(void) {
    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_finish() == 0);
}

int main(void) {
    test_start_and_args();
    test_finish();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
