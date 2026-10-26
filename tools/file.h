#ifndef CT_FILE_H
#define CT_FILE_H

#include "arena.h"
#include "diag.h"

#include <stdbool.h>
#include <stddef.h>

/* Whole-file contents. path and data live in the arena passed to
   ct_file_read. data is NUL-terminated; size does not include that byte.
   Both pointers die with the arena (including on reset). */

typedef struct CtFile {
    const char *path;
    const char *data;
    size_t size;
} CtFile;

/* Read all of path (UTF-8, see fs.h) into arena and copy path into arena as well.
   On success fills out and returns true. On failure zeroes out when it
   is non-NULL, returns false, and emits one error through diag when the
   diagnostic list can store it. diag may be NULL if the caller does not
   want a message. diag may use a different arena than the file bytes. */
bool ct_file_read(CtArena *arena, CtDiag *diag, const char *path, CtFile *out);

#endif
