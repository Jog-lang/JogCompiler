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

static void test_alloc_and_free(void) {
    CHECK(jogrt_alloc(8, 0) == NULL);
    CHECK(jogrt_alloc(8, 3) == NULL);
    CHECK(jogrt_alloc(8, 6) == NULL);
    CHECK(jogrt_alloc(SIZE_MAX, 8) == NULL);

    static const size_t aligns[] = {1, 2, 4, 8, 16, 32, 64, 256, 4096};
    for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        size_t align = aligns[i];
        unsigned char *block = jogrt_alloc(24, align);
        CHECK(block != NULL && is_aligned(block, align) && is_zeroed(block, 24));
        if (block) {
            memset(block, 0xa5, 24);
            CHECK(block[0] == 0xa5 && block[23] == 0xa5);
            jogrt_free(block, 24, align);
        }
    }

    unsigned char *zero_a = jogrt_alloc(0, 8);
    unsigned char *zero_b = jogrt_alloc(0, 8);
    CHECK(zero_a != NULL && zero_b != NULL && zero_a != zero_b);
    if (zero_a && zero_b) {
        zero_a[0] = 1;
        zero_b[0] = 2;
        CHECK(zero_a[0] == 1 && zero_b[0] == 2);
        jogrt_free(zero_a, 0, 8);
        jogrt_free(zero_b, 0, 8);
    }

    unsigned char *left = jogrt_alloc(32, 16);
    unsigned char *right = jogrt_alloc(32, 16);
    CHECK(left != NULL && right != NULL);
    CHECK(left && right && !ranges_overlap(left, 32, right, 32));
    if (left && right) {
        memset(left, 1, 32);
        memset(right, 2, 32);
        CHECK(left[31] == 1 && right[0] == 2);
        jogrt_free(right, 32, 16);
        jogrt_free(left, 32, 16);
    }

    void *blocks[32];
    for (int i = 0; i < 32; i++) {
        blocks[i] = jogrt_alloc((size_t)i + 1, 16);
        CHECK(blocks[i] != NULL && is_aligned(blocks[i], 16));
        if (blocks[i]) {
            memset(blocks[i], 0x5a, (size_t)i + 1);
        }
    }
    for (int i = 0; i < 32; i++) {
        unsigned char *bytes = blocks[i];
        CHECK(bytes != NULL && bytes[0] == 0x5a && bytes[i] == 0x5a);
        jogrt_free(blocks[i], (size_t)i + 1, 16);
    }

    jogrt_free(NULL, 0, 1);
    jogrt_free(NULL, 32, 8);
}

static void test_realloc(void) {
    CHECK(jogrt_realloc(NULL, 0, 8, 0) == NULL);
    CHECK(jogrt_realloc(NULL, 0, 8, 3) == NULL);
    CHECK(jogrt_realloc(NULL, 0, SIZE_MAX, 8) == NULL);

    unsigned char *fresh = jogrt_realloc(NULL, 99, 4, 8);
    CHECK(fresh != NULL && is_aligned(fresh, 8) && is_zeroed(fresh, 4));
    if (fresh) {
        fresh[0] = 5;
        jogrt_free(fresh, 4, 8);
    }

    unsigned char *block = jogrt_alloc(8, 8);
    CHECK(block != NULL && is_zeroed(block, 8));
    if (!block) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        block[i] = (unsigned char)(i + 1);
    }

    unsigned char *same = jogrt_realloc(block, 8, 8, 3);
    CHECK(same == NULL);
    CHECK(block[0] == 1 && block[7] == 8);

    unsigned char *huge = jogrt_realloc(block, 8, SIZE_MAX, 8);
    CHECK(huge == NULL);
    CHECK(block[0] == 1 && block[7] == 8);

    unsigned char *grown = jogrt_realloc(block, 8, 16, 8);
    CHECK(grown != NULL && is_aligned(grown, 8));
    if (!grown) {
        jogrt_free(block, 8, 8);
        return;
    }
    CHECK(grown[0] == 1 && grown[7] == 8);
    CHECK(is_zeroed(grown + 8, 8));

    unsigned char *shrunk = jogrt_realloc(grown, 16, 4, 8);
    CHECK(shrunk != NULL);
    if (!shrunk) {
        jogrt_free(grown, 16, 8);
        return;
    }
    CHECK(shrunk[0] == 1 && shrunk[3] == 4);

    unsigned char *empty = jogrt_realloc(shrunk, 4, 0, 8);
    CHECK(empty != NULL);
    if (!empty) {
        jogrt_free(shrunk, 4, 8);
        return;
    }
    empty[0] = 3;
    jogrt_free(empty, 0, 8);

    unsigned char *kept = jogrt_alloc(4, 8);
    CHECK(kept != NULL);
    if (kept) {
        kept[0] = 9;
        unsigned char *cleared = jogrt_realloc(kept, 0, 4, 8);
        CHECK(cleared != NULL && is_zeroed(cleared, 4));
        jogrt_free(cleared, 4, 8);
    }
}

int main(void) {
    test_alloc_and_free();
    test_realloc();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
