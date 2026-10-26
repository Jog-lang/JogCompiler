#ifndef JOG_LOWER_MEMORY_H
#define JOG_LOWER_MEMORY_H

#include "lower_internal.h"

/* Pointers, arrays, slices, bytes, indexing, and pointer arithmetic.
   Not a contract header. Bounds checks are JIR_BOUNDS_CHECK. @unchecked
   skips those checks and leaves integer overflow checked. */

JirOperand jog_lower_memory_expr(Lower *L, JogNode *node);

/* Address of an assignable node: an index, a field, a dereference, or a local.
   A temporary is spilled to a local first. */
JirOperand jog_lower_lvalue_ptr(Lower *L, JogNode *node);
JirOperand jog_lower_memory_unary(Lower *L, JogNode *node);

/* True when node is pointer arithmetic or a pointer comparison. */
bool jog_lower_memory_binary(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out);

/* True when node is an index, a dereference, or a pointer += / -=. */
bool jog_lower_memory_assign(Lower *L, JogNode *node);

/* True when call is bytes(). */
bool jog_lower_memory_call(Lower *L, JogNode *node, JirOperand *out);

/* Stack array of elems, passed as a slice header. count 0 is a zero slice
   (null pointer, length 0). slice_ty is []T, substituted when lowering a
   generic instance. */
JirOperand jog_lower_pack_slice(Lower *L, JogType *slice_ty, const JirOperand *elems, size_t count, JogPos pos);

/* slot addresses a stored pointer. op is PLUS or MINUS. */
void jog_lower_memory_ptr_update(Lower *L, JirOperand slot, JogTokenKind op, JogNode *rhs, JogType *ptr_ty, JogPos pos);

bool jog_lower_memory_unchecked(const JogNode *node);

#endif
