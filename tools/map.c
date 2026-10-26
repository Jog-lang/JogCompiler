#include "map.h"

#include <stdint.h>
#include <string.h>

enum {
    CT_SLOT_EMPTY = 0,
    CT_SLOT_LIVE = 1,
    CT_SLOT_TOMBSTONE = 2
};

enum { CT_MAP_INITIAL_CAPACITY = 8 };

typedef struct CtSlot {
    const char *key;
    void *value;
    unsigned char state;
} CtSlot;

struct CtMap {
    CtArena *arena;
    CtSlot *slots;
    size_t capacity;
    size_t count;
    size_t tombstones;
};

static uint64_t fnv1a(const char *key) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const unsigned char *byte = (const unsigned char *)key;
    while (*byte != 0) {
        hash ^= (uint64_t)*byte;
        hash *= 0x100000001b3ULL;
        byte++;
    }
    return hash;
}

static size_t slot_index(uint64_t hash, size_t capacity) {
    return (size_t)(hash & (uint64_t)(capacity - 1));
}

/* Stops at an empty slot. Tombstones are skipped. *matched is 1 only for
   a live key. The returned index is a live match, the first tombstone in
   the probe, or the empty slot where a new key would go. SIZE_MAX means
   the table has no empty slot and no tombstone. */
static size_t locate(const CtMap *map, const char *key, int *matched) {
    size_t mask = map->capacity - 1;
    size_t index = slot_index(fnv1a(key), map->capacity);
    size_t first_tombstone = SIZE_MAX;

    for (size_t n = 0; n < map->capacity; n++) {
        const CtSlot *slot = &map->slots[index];
        if (slot->state == CT_SLOT_EMPTY) {
            *matched = 0;
            if (first_tombstone != SIZE_MAX) {
                return first_tombstone;
            }
            return index;
        }
        if (slot->state == CT_SLOT_TOMBSTONE) {
            if (first_tombstone == SIZE_MAX) {
                first_tombstone = index;
            }
        } else if (slot->state == CT_SLOT_LIVE && strcmp(slot->key, key) == 0) {
            *matched = 1;
            return index;
        }
        index = (index + 1) & mask;
    }

    *matched = 0;
    return first_tombstone;
}

static int load_reached(const CtMap *map) {
    return map->count + map->tombstones >= (map->capacity / 4) * 3;
}

static int rehash_into(CtSlot *slots, size_t capacity, const char *key, void *value) {
    size_t mask = capacity - 1;
    size_t index = slot_index(fnv1a(key), capacity);
    for (size_t n = 0; n < capacity; n++) {
        if (slots[index].state == CT_SLOT_EMPTY) {
            slots[index].key = key;
            slots[index].value = value;
            slots[index].state = CT_SLOT_LIVE;
            return 1;
        }
        index = (index + 1) & mask;
    }
    return 0;
}

/* Doubles the bucket array from the same arena and rehashes live entries.
   The previous array is left behind until the arena is reset or destroyed.
   Tombstones are dropped. Stored key pointers are reused, not copied. */
static int map_grow(CtMap *map) {
    if (map->capacity > SIZE_MAX / 2) {
        return 0;
    }
    size_t new_capacity = map->capacity * 2;
    if (new_capacity > SIZE_MAX / sizeof(CtSlot)) {
        return 0;
    }

    CtSlot *fresh = ct_arena_alloc(map->arena, new_capacity * sizeof(CtSlot), _Alignof(CtSlot));
    if (!fresh) {
        return 0;
    }
    memset(fresh, 0, new_capacity * sizeof(CtSlot));

    for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].state != CT_SLOT_LIVE) {
            continue;
        }
        if (!rehash_into(fresh, new_capacity, map->slots[i].key, map->slots[i].value)) {
            return 0;
        }
    }

    map->slots = fresh;
    map->capacity = new_capacity;
    map->tombstones = 0;
    return 1;
}

CtMap *ct_map_create(CtArena *arena) {
    if (!arena) {
        return NULL;
    }

    CtMap *map = ct_arena_alloc(arena, sizeof(*map), _Alignof(CtMap));
    if (!map) {
        return NULL;
    }

    CtSlot *slots = ct_arena_alloc(
        arena, (size_t)CT_MAP_INITIAL_CAPACITY * sizeof(*slots), _Alignof(CtSlot));
    if (!slots) {
        return NULL;
    }
    memset(slots, 0, (size_t)CT_MAP_INITIAL_CAPACITY * sizeof(*slots));

    map->arena = arena;
    map->slots = slots;
    map->capacity = (size_t)CT_MAP_INITIAL_CAPACITY;
    map->count = 0;
    map->tombstones = 0;
    return map;
}

bool ct_map_put(CtMap *map, const char *key, void *value) {
    if (!map || !key) {
        return false;
    }

    int matched = 0;
    size_t index = locate(map, key, &matched);
    if (matched) {
        map->slots[index].value = value;
        return true;
    }

    if (index == SIZE_MAX || load_reached(map)) {
        if (!map_grow(map)) {
            return false;
        }
        matched = 0;
        index = locate(map, key, &matched);
        if (matched) {
            map->slots[index].value = value;
            return true;
        }
        if (index == SIZE_MAX) {
            return false;
        }
    }

    char *copied = ct_arena_strdup(map->arena, key);
    if (!copied) {
        return false;
    }

    CtSlot *slot = &map->slots[index];
    if (slot->state == CT_SLOT_TOMBSTONE) {
        map->tombstones--;
    }
    slot->key = copied;
    slot->value = value;
    slot->state = CT_SLOT_LIVE;
    map->count++;
    return true;
}

bool ct_map_get(const CtMap *map, const char *key, void **out) {
    if (!map || !key || !out) {
        return false;
    }

    int matched = 0;
    size_t index = locate(map, key, &matched);
    if (!matched) {
        return false;
    }
    *out = map->slots[index].value;
    return true;
}

bool ct_map_remove(CtMap *map, const char *key) {
    if (!map || !key) {
        return false;
    }

    int matched = 0;
    size_t index = locate(map, key, &matched);
    if (!matched) {
        return false;
    }

    map->slots[index].state = CT_SLOT_TOMBSTONE;
    map->count--;
    map->tombstones++;
    return true;
}

size_t ct_map_count(const CtMap *map) {
    if (!map) {
        return 0;
    }
    return map->count;
}
