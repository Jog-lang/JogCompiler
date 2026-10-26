#ifndef CT_STR_H
#define CT_STR_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

/* Non-owning string view. ptr may be NULL only when len is 0. The bytes
   are not required to be NUL-terminated. */
typedef struct CtStr {
    const char *ptr;
    size_t len;
} CtStr;

/* View of a NUL-terminated string. NULL becomes the empty view. */
CtStr ct_str(const char *cstr);
CtStr ct_str_n(const char *ptr, size_t len);

bool ct_str_eq(CtStr a, CtStr b);
bool ct_str_eq_cstr(CtStr a, const char *b);
bool ct_str_starts_with(CtStr s, CtStr prefix);
bool ct_str_ends_with(CtStr s, CtStr suffix);

/* NUL-terminated copy in arena. NULL if arena is NULL or out of memory. */
char *ct_str_dup(CtArena *arena, CtStr s);

/* Growable byte buffer backed by realloc and free, for building output
   such as generated source. The buffer is always NUL-terminated once it
   has data. A failed append sets failed and leaves the contents as they
   were; later appends still try, so callers may check failed once at the
   end. Pointers into data die on any growing append and on ct_sb_free. */
typedef struct CtSb {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} CtSb;

void ct_sb_init(CtSb *sb);

/* Frees the buffer. NULL is ignored. The object can be used again. */
void ct_sb_free(CtSb *sb);

/* Sets len to 0 and keeps the buffer. Does not clear failed. */
void ct_sb_clear(CtSb *sb);

bool ct_sb_append(CtSb *sb, const char *data, size_t len);
bool ct_sb_append_cstr(CtSb *sb, const char *cstr);
bool ct_sb_append_str(CtSb *sb, CtStr s);
bool ct_sb_putc(CtSb *sb, char c);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
bool ct_sb_appendf(CtSb *sb, const char *fmt, ...);

/* The contents as a NUL-terminated string. "" when nothing has been
   appended. Valid until the next append, clear, or free. */
const char *ct_sb_cstr(const CtSb *sb);

CtStr ct_sb_view(const CtSb *sb);

#endif
