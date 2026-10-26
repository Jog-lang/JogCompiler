#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CT_ARENA_CHUNK_SIZE = 8192 };

typedef struct CtChunk {
    struct CtChunk *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
} CtChunk;

struct CtArena {
    CtChunk *first;
    CtChunk *current;
};

static int is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static CtChunk *chunk_new(size_t min_capacity) {
    size_t capacity = CT_ARENA_CHUNK_SIZE;
    if (min_capacity > capacity) {
        capacity = min_capacity;
    }
    if (capacity > SIZE_MAX - sizeof(CtChunk)) {
        return NULL;
    }

    CtChunk *chunk = malloc(sizeof(CtChunk) + capacity);
    if (!chunk) {
        return NULL;
    }
    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->used = 0;
    return chunk;
}

static int chunk_try(CtChunk *chunk, size_t size, size_t align, void **out) {
    uintptr_t base = (uintptr_t)chunk->data;
    uintptr_t current = base + chunk->used;
    uintptr_t aligned = (current + (align - 1)) & ~(uintptr_t)(align - 1);
    if (aligned < current) {
        return 0;
    }

    size_t padding = (size_t)(aligned - current);
    if (chunk->used > chunk->capacity) {
        return 0;
    }
    size_t available = chunk->capacity - chunk->used;
    if (padding > available || size > available - padding) {
        return 0;
    }

    chunk->used += padding + size;
    *out = (void *)aligned;
    return 1;
}

CtArena *ct_arena_create(void) {
    CtArena *arena = calloc(1, sizeof(*arena));
    return arena;
}

void ct_arena_destroy(CtArena *arena) {
    if (!arena) {
        return;
    }
    CtChunk *chunk = arena->first;
    while (chunk) {
        CtChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    free(arena);
}

void ct_arena_reset(CtArena *arena) {
    if (!arena) {
        return;
    }
    for (CtChunk *chunk = arena->first; chunk; chunk = chunk->next) {
        chunk->used = 0;
    }
    arena->current = arena->first;
}

void *ct_arena_alloc(CtArena *arena, size_t size, size_t align) {
    if (!arena || !is_power_of_two(align) || size > SIZE_MAX - (align - 1)) {
        return NULL;
    }

    void *found = NULL;
    if (arena->current && chunk_try(arena->current, size, align, &found)) {
        return found;
    }

    CtChunk *last = arena->current;
    for (CtChunk *chunk = last ? last->next : NULL; chunk; chunk = chunk->next) {
        last = chunk;
        if (chunk_try(chunk, size, align, &found)) {
            arena->current = chunk;
            return found;
        }
    }

    CtChunk *chunk = chunk_new(size + (align - 1));
    if (!chunk) {
        return NULL;
    }
    if (last) {
        last->next = chunk;
    } else {
        arena->first = chunk;
    }
    arena->current = chunk;
    if (!chunk_try(chunk, size, align, &found)) {
        return NULL;
    }
    return found;
}

void *ct_arena_copy(CtArena *arena, const void *src, size_t size) {
    if (!arena || (size > 0 && !src)) {
        return NULL;
    }
    void *dst = ct_arena_alloc(arena, size, 1);
    if (!dst) {
        return NULL;
    }
    if (size > 0) {
        memcpy(dst, src, size);
    }
    return dst;
}

char *ct_arena_strdup(CtArena *arena, const char *src) {
    if (!arena || !src) {
        return NULL;
    }
    size_t length = strlen(src);
    char *dst = ct_arena_alloc(arena, length + 1, 1);
    if (!dst) {
        return NULL;
    }
    memcpy(dst, src, length + 1);
    return dst;
}
