#include "intern.h"

#include <stdint.h>
#include <string.h>

enum { CT_INTERN_INITIAL_CAPACITY = 64 };

typedef struct CtInternSlot {
    const char *str;
    size_t len;
    uint64_t hash;
} CtInternSlot;

struct CtIntern {
    CtArena *arena;
    CtInternSlot *slots;
    size_t capacity;
    size_t count;
};

static uint64_t fnv1a_n(const char *ptr, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)(unsigned char)ptr[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static CtInternSlot *alloc_slots(CtArena *arena, size_t capacity) {
    if (capacity > SIZE_MAX / sizeof(CtInternSlot)) {
        return NULL;
    }
    CtInternSlot *slots = ct_arena_alloc(arena, capacity * sizeof(*slots), _Alignof(CtInternSlot));
    if (slots) {
        memset(slots, 0, capacity * sizeof(*slots));
    }
    return slots;
}

/* Index of the matching slot, or of the empty slot where it would go.
   The table is never full, so the probe always ends. */
static size_t probe(const CtInternSlot *slots, size_t capacity, const char *ptr, size_t len, uint64_t hash) {
    size_t mask = capacity - 1;
    size_t index = (size_t)(hash & (uint64_t)mask);
    while (slots[index].str) {
        const CtInternSlot *slot = &slots[index];
        if (slot->hash == hash && slot->len == len && (len == 0 || memcmp(slot->str, ptr, len) == 0)) {
            return index;
        }
        index = (index + 1) & mask;
    }
    return index;
}

/* Doubles the table. The old slot array stays in the arena. */
static int grow(CtIntern *table) {
    if (table->capacity > SIZE_MAX / 2) {
        return 0;
    }
    size_t capacity = table->capacity * 2;
    CtInternSlot *slots = alloc_slots(table->arena, capacity);
    if (!slots) {
        return 0;
    }
    for (size_t i = 0; i < table->capacity; i++) {
        const CtInternSlot *old = &table->slots[i];
        if (old->str) {
            slots[probe(slots, capacity, old->str, old->len, old->hash)] = *old;
        }
    }
    table->slots = slots;
    table->capacity = capacity;
    return 1;
}

CtIntern *ct_intern_create(CtArena *arena) {
    if (!arena) {
        return NULL;
    }
    CtIntern *table = ct_arena_alloc(arena, sizeof(*table), _Alignof(CtIntern));
    if (!table) {
        return NULL;
    }
    table->slots = alloc_slots(arena, CT_INTERN_INITIAL_CAPACITY);
    if (!table->slots) {
        return NULL;
    }
    table->arena = arena;
    table->capacity = CT_INTERN_INITIAL_CAPACITY;
    table->count = 0;
    return table;
}

const char *ct_intern(CtIntern *table, const char *ptr, size_t len) {
    if (!table || (len > 0 && !ptr) || len == SIZE_MAX) {
        return NULL;
    }
    uint64_t hash = fnv1a_n(ptr, len);
    size_t index = probe(table->slots, table->capacity, ptr, len, hash);
    if (table->slots[index].str) {
        return table->slots[index].str;
    }

    if ((table->count + 1) * 4 > table->capacity * 3) {
        if (!grow(table)) {
            return NULL;
        }
        index = probe(table->slots, table->capacity, ptr, len, hash);
    }

    char *copy = ct_arena_alloc(table->arena, len + 1, 1);
    if (!copy) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, ptr, len);
    }
    copy[len] = '\0';

    CtInternSlot *slot = &table->slots[index];
    slot->str = copy;
    slot->len = len;
    slot->hash = hash;
    table->count++;
    return copy;
}

const char *ct_intern_cstr(CtIntern *table, const char *cstr) {
    if (!cstr) {
        return NULL;
    }
    return ct_intern(table, cstr, strlen(cstr));
}

size_t ct_intern_count(const CtIntern *table) {
    if (!table) {
        return 0;
    }
    return table->count;
}
