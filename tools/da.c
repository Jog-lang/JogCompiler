#include "da.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int size_mul(size_t left, size_t right, size_t *out) {
    if (left != 0 && right > SIZE_MAX / left) {
        return 0;
    }
    *out = left * right;
    return 1;
}

static unsigned char *item_ptr(CtDa *da, size_t index) {
    return (unsigned char *)da->data + index * da->item_size;
}

static int grow(CtDa *da) {
    size_t capacity = 8;
    if (da->capacity != 0) {
        if (da->capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity = da->capacity * 2;
    }

    size_t bytes = 0;
    if (!size_mul(capacity, da->item_size, &bytes)) {
        return 0;
    }

    void *data = realloc(da->data, bytes);
    if (!data) {
        return 0;
    }
    da->data = data;
    da->capacity = capacity;
    return 1;
}

bool ct_da_init(CtDa *da, size_t item_size) {
    if (!da) {
        return false;
    }
    da->data = NULL;
    da->count = 0;
    da->capacity = 0;
    da->item_size = 0;
    if (item_size == 0) {
        return false;
    }
    da->item_size = item_size;
    return true;
}

void ct_da_free(CtDa *da) {
    if (!da) {
        return;
    }
    free(da->data);
    da->data = NULL;
    da->count = 0;
    da->capacity = 0;
    da->item_size = 0;
}

bool ct_da_push(CtDa *da, const void *item) {
    if (!da || !item || da->item_size == 0) {
        return false;
    }
    if (da->count == da->capacity) {
        if (!grow(da)) {
            return false;
        }
    }
    memcpy(item_ptr(da, da->count), item, da->item_size);
    da->count++;
    return true;
}

bool ct_da_swap_remove(CtDa *da, size_t index) {
    if (!da || index >= da->count) {
        return false;
    }
    size_t last = da->count - 1;
    if (index != last) {
        memcpy(item_ptr(da, index), item_ptr(da, last), da->item_size);
    }
    da->count--;
    return true;
}

void *ct_da_at(CtDa *da, size_t index) {
    if (!da || index >= da->count) {
        return NULL;
    }
    return item_ptr(da, index);
}
