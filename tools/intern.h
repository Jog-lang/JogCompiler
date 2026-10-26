#ifndef CT_INTERN_H
#define CT_INTERN_H

#include "arena.h"

#include <stddef.h>

/* String interning. Every distinct byte sequence maps to one canonical,
   NUL-terminated copy in the arena, so interned strings compare equal
   exactly when their pointers are equal. The table and the copies live in
   the arena passed to ct_intern_create; resetting or destroying it
   invalidates every interned pointer. Strings may contain NUL bytes; the
   length passed in is what counts. */

typedef struct CtIntern CtIntern;

/* NULL if arena is NULL or out of memory. */
CtIntern *ct_intern_create(CtArena *arena);

/* Canonical copy of ptr[0..len). NULL if table is NULL, ptr is NULL while
   len > 0, or the arena is out of memory. */
const char *ct_intern(CtIntern *table, const char *ptr, size_t len);

const char *ct_intern_cstr(CtIntern *table, const char *cstr);

/* Number of distinct strings. 0 if table is NULL. */
size_t ct_intern_count(const CtIntern *table);

#endif
