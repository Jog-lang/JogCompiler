#ifndef CT_MAP_H
#define CT_MAP_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

/* String-to-pointer map. The map, its bucket arrays, and copied keys live
   in the arena passed to ct_map_create. Resetting or destroying that arena
   invalidates the map. There is no per-entry free; growth abandons the
   previous bucket array until the arena is reset or destroyed. Iteration
   order is unspecified. */

typedef struct CtMap CtMap;

/* Allocates the map and an initial 8-slot table. NULL if arena is NULL
   or the arena is out of memory. */
CtMap *ct_map_create(CtArena *arena);

/* Insert or replace. Copies key on first insert only; a later replace
   keeps that key pointer and does not change the live count. Returns
   false if map or key is NULL, or the arena is out of memory. The empty
   string is a valid key. value may be NULL. */
bool ct_map_put(CtMap *map, const char *key, void *value);

/* Returns false if map, key, or out is NULL, or the key is absent.
   On success writes *out, which may be NULL. */
bool ct_map_get(const CtMap *map, const char *key, void **out);

/* Sets a tombstone on a live entry. Returns true only when one was
   removed. A NULL map or NULL key returns false. */
bool ct_map_remove(CtMap *map, const char *key);

/* Number of live entries. 0 if map is NULL. */
size_t ct_map_count(const CtMap *map);

#endif
