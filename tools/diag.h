#ifndef CT_DIAG_H
#define CT_DIAG_H

#include "arena.h"

#include <stdio.h>

/* Diagnostics are stored in the caller-owned arena passed to
   ct_diag_create. Resetting or destroying that arena invalidates the
   CtDiag and every message. Printing does not clear the list. */

typedef enum CtSeverity {
    CT_SEVERITY_NOTE = 0,
    CT_SEVERITY_WARNING = 1,
    CT_SEVERITY_ERROR = 2
} CtSeverity;

/* line 0 means the message has no source position. column is meaningful
   only when line is non-zero. file is copied into the arena when the
   message is emitted; NULL file is allowed. */
typedef struct CtPos {
    const char *file;
    unsigned line;
    unsigned column;
} CtPos;

typedef struct CtDiag CtDiag;

CtDiag *ct_diag_create(CtArena *arena);

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
void ct_diag_emit(CtDiag *diag, CtSeverity severity, CtPos pos, const char *fmt, ...);

/* Number of messages emitted with CT_SEVERITY_ERROR. */
size_t ct_diag_error_count(const CtDiag *diag);

/* Write every message in emission order. Does not clear the list.
   With a source position:
     file:line:column: note|warning|error: message
   With line 0 and a file name:
     file: note|warning|error: message
   With no file:
     note|warning|error: message
   A NULL diag or out is ignored. */
void ct_diag_print(const CtDiag *diag, FILE *out);

/* One JSON object per line, in emission order. Does not clear the list.
     {"file":"...","line":N,"column":N,"severity":"...","message":"..."}
   line and column are the stored positions: 1-based when the message has
   a source position, and 0 when it does not. severity is "note",
   "warning", or "error". Strings escape quotes, backslashes, and
   controls; other bytes, including non-ASCII UTF-8, are copied through.
   A NULL diag or out is ignored. */
void ct_diag_print_json(const CtDiag *diag, FILE *out);

#endif
