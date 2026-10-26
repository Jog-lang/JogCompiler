#include "jogrt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int is_aligned(const void *ptr, size_t align) {
    return ((uintptr_t)ptr & (align - 1)) == 0;
}

static int is_zeroed(const void *ptr, size_t size) {
    const unsigned char *bytes = ptr;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int ranges_overlap(const void *left, size_t left_size, const void *right, size_t right_size) {
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    if (a <= b) {
        return b < a + left_size;
    }
    return a < b + right_size;
}

/* Release rewinds the bump. The next allocation with the same size and
   alignment returns that storage again, and it is zeroed. Bytes in front
   of the mark stay. */
static void test_mark_and_release(void) {
    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_error_mark() == 0);
    jogrt_error_release(0);
    jogrt_error_release(100);
    CHECK(jogrt_error_mark() == 0);

    CHECK(jogrt_error_alloc(1, 0) == NULL);
    CHECK(jogrt_error_alloc(1, 3) == NULL);
    CHECK(jogrt_error_alloc(1, 6) == NULL);
    CHECK(jogrt_error_alloc(SIZE_MAX, 1) == NULL);
    CHECK(jogrt_error_mark() == 0);

    unsigned char *a = jogrt_error_alloc(10, 8);
    CHECK(a != NULL && is_aligned(a, 8) && is_zeroed(a, 10));
    if (!a) {
        return;
    }
    memset(a, 0xab, 10);
    size_t mark_a = jogrt_error_mark();
    CHECK(mark_a != 0);

    unsigned char *b = jogrt_error_alloc(20, 8);
    CHECK(b != NULL && is_zeroed(b, 20));
    CHECK(b && !ranges_overlap(a, 10, b, 20));
    if (!b) {
        return;
    }
    memset(b, 0xcd, 20);
    size_t mark_b = jogrt_error_mark();
    CHECK(mark_b > mark_a);

    unsigned char *c = jogrt_error_alloc(30, 8);
    CHECK(c != NULL && is_zeroed(c, 30));
    CHECK(c && !ranges_overlap(b, 20, c, 30));
    if (!c) {
        return;
    }
    memset(c, 0xef, 30);

    /* A mark past the frontier is not a release. */
    jogrt_error_release(mark_b + 50);
    CHECK(c[0] == 0xef && a[0] == 0xab && b[0] == 0xcd);

    jogrt_error_release(mark_b);
    unsigned char *c2 = jogrt_error_alloc(30, 8);
    CHECK(c2 == c && is_zeroed(c2, 30));
    CHECK(a[0] == 0xab && b[19] == 0xcd);

    jogrt_error_release(mark_a);
    CHECK(a[0] == 0xab && a[9] == 0xab);
    unsigned char *b2 = jogrt_error_alloc(20, 8);
    CHECK(b2 == b && is_zeroed(b2, 20));
    CHECK(!ranges_overlap(a, 10, b2, 20));
    CHECK(a[0] == 0xab && a[9] == 0xab);

    unsigned char *big1 = jogrt_error_alloc(8000, 8);
    CHECK(big1 != NULL && is_zeroed(big1, 8000));
    if (!big1) {
        return;
    }
    memset(big1, 0x22, 8000);
    size_t mark_big = jogrt_error_mark();
    unsigned char *big2 = jogrt_error_alloc(8000, 8);
    CHECK(big2 != NULL && !ranges_overlap(big1, 8000, big2, 8000));
    if (!big2) {
        return;
    }
    memset(big2, 0x11, 8000);
    jogrt_error_release(mark_big);
    CHECK(big1[0] == 0x22 && big1[7999] == 0x22);
    unsigned char *big3 = jogrt_error_alloc(8000, 8);
    CHECK(big3 == big2 && is_zeroed(big3, 8000));

    jogrt_error_release(0);
    CHECK(jogrt_error_mark() == 0);
    unsigned char *restart = jogrt_error_alloc(8, 8);
    CHECK(restart == a && is_zeroed(restart, 8));

    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_error_mark() == 0);
}

static void test_alignment_and_reuse(void) {
    CHECK(jogrt_finish() == 0);

    static const size_t aligns[] = {1, 2, 4, 8, 16, 32, 64, 256};
    for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        unsigned char *block = jogrt_error_alloc(17, aligns[i]);
        CHECK(block != NULL && is_aligned(block, aligns[i]) && is_zeroed(block, 17));
        if (block) {
            memset(block, 0x5a, 17);
        }
    }

    void *zero = jogrt_error_alloc(0, 8);
    CHECK(zero != NULL);

    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_error_mark() == 0);
    unsigned char *again = jogrt_error_alloc(16, 8);
    CHECK(again != NULL && is_aligned(again, 8) && is_zeroed(again, 16));
    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_finish() == 0);
    CHECK(jogrt_error_mark() == 0);
}

int main(void) {
    test_mark_and_release();
    test_alignment_and_reuse();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
