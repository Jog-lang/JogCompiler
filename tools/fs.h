#ifndef CT_FS_H
#define CT_FS_H

#include "arena.h"
#include "da.h"
#include "diag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Directory queries for the package loader. Paths use '/' or the native
   separator. Nothing here follows symlink loops or recurses.

   Every path and name is UTF-8. Windows converts at this boundary and
   calls the W APIs, so a path outside the ANSI code page still opens.
   Other hosts pass the bytes through unchanged. */

typedef enum CtFsKind {
    CT_FS_FILES = 0,
    CT_FS_DIRS = 1
} CtFsKind;

bool ct_fs_is_dir(const char *path);
bool ct_fs_is_file(const char *path);

/* Lists the entries directly inside dir that are regular files or
   directories, depending on kind. Names starting with '.' are skipped.
   With a non-NULL suffix, only names ending in it are kept. Each name, not
   a full path, is copied into arena and pushed onto out as a const char *;
   out must be initialized with item_size sizeof(const char *). The pushed
   names are sorted by byte value, so the order is the same on every
   platform. On failure returns false, emits one error through diag when it
   is non-NULL, and leaves any names already pushed in out. */
bool ct_fs_list(CtArena *arena, CtDiag *diag, const char *dir, CtFsKind kind, const char *suffix, CtDa *out);

/* a + "/" + b in arena, without doubling a trailing separator on a. An
   empty a gives a copy of b. NULL on bad arguments or out of memory. */
char *ct_path_join(CtArena *arena, const char *a, const char *b);

/* fopen with a UTF-8 path. NULL when the path is not valid UTF-8. */
FILE *ct_fs_fopen(const char *path, const char *mode);

/* Deletes a file. True on success. */
bool ct_fs_remove(const char *path);

/* Creates one directory; the parent must exist. True when it was created. */
bool ct_fs_mkdir(const char *path);

/* The environment variable as UTF-8 in arena, or NULL when it is unset. */
char *ct_fs_getenv(CtArena *arena, const char *name);

#if defined(_WIN32)
/* UTF-8 to NUL-terminated UTF-16 in a malloc block the caller frees.
   NULL on invalid UTF-8 or out of memory. */
wchar_t *ct_fs_widen(const char *utf8);

/* NUL-terminated UTF-16 to UTF-8 in arena. NULL on out of memory. */
char *ct_fs_narrow(CtArena *arena, const wchar_t *wide);
#endif

#endif
