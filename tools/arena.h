#ifndef CT_ARENA_H
#define CT_ARENA_H

#include <stddef.h>

/* Bump allocator. Objects live until ct_arena_reset or ct_arena_destroy;
   there is no per-object free. Reset keeps the malloc chunks and rewinds
   the bump pointer, which invalidates every pointer previously returned.
   Destroy frees the chunks and the arena. A NULL result means out of
   memory or a bad argument, not an empty allocation. */

typedef struct CtArena CtArena;

CtArena *ct_arena_create(void);
void ct_arena_destroy(CtArena *arena);
void ct_arena_reset(CtArena *arena);

/* align must be a power of two. size 0 returns a non-NULL pointer and
   reserves no bytes. */
void *ct_arena_alloc(CtArena *arena, size_t size, size_t align);

/* Byte copy, aligned to 1. NULL if arena is NULL, src is NULL while
   size > 0, or the arena is out of memory. */
void *ct_arena_copy(CtArena *arena, const void *src, size_t size);

/* Copy including the terminating NUL. NULL if arena or src is NULL,
   or the arena is out of memory. */
char *ct_arena_strdup(CtArena *arena, const char *src);

#endif
