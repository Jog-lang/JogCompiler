#ifndef JOG_CONST_EVAL_H
#define JOG_CONST_EVAL_H

#include "ast.h"
#include "types.h"

#include "diag.h"

/* Constant folding for literal and operator trees. Integer operators are
   evaluated exactly (not modulo 2^n). A missing target types an integer
   result as i64 and a boolean result as bool. A value that does not fit,
   overflow of an operator, and division by zero are compile errors.

   A hex integer literal (0x, underscores ignored) with a [N]byte target
   is the bytes in written order, and it must contain exactly 2*N digits.
   Every other integer literal is a number; a hex literal used as an
   integer is that number, zero-extended into an unsigned target.

   On success, node->type is the folded type. bits is the two's-complement
   value sign-extended to 64 bits when type is signed, and zero-extended
   when type is unsigned, so the numeric value is (int64_t)bits for a
   signed type and bits for an unsigned type. bool uses 0 or 1. For
   JOG_CONST_BYTES, bytes points at len bytes in arena, in written order.

   Intermediate untyped integers are marked JOG_TY_UNTYPED_INT and may be
   wider than i64. An intermediate wider than 4096 bits is a compile error.
   && and || do not fold the operand short-circuiting would skip.

   size(T) and align(T) fold to usize when the argument type already has a
   layout. align(T) is that type's alignment. A type parameter has no number
   until instantiation.

   Returns false if any required pointer is NULL, the expression is not a
   foldable constant, or a diagnostic was emitted. *out is zeroed on
   failure. NULL arguments emit nothing. */

typedef enum JogConstKind {
    JOG_CONST_INT = 0,
    JOG_CONST_BOOL,
    JOG_CONST_BYTES
} JogConstKind;

typedef struct JogConst {
    JogConstKind kind;
    JogType *type;
    uint64_t bits;
    const uint8_t *bytes;
    size_t len;
} JogConst;

bool jog_const_eval(CtArena *arena, CtDiag *diag, JogTypes *types, JogNode *node, JogType *target, JogConst *out);

#endif
