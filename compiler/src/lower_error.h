#ifndef JOG_LOWER_ERROR_H
#define JOG_LOWER_ERROR_H

#include "lower_internal.h"

/* Tuples, multiple returns, destructuring, null, defer, errdefer, and panic.
   Not a contract header. defer and errdefer become cleanup blocks on every
   exit edge, last in first out. errdefer runs only on a return whose last
   result is a non-null error?. panic is JIR_PANIC and does not run cleanups.
   return a, b aggregates a tuple result. A tuple field load dereferences one
   pointer. Destructuring stores each element into an lvalue. A concrete
   error is the sentinel address in both words of the error interface, then
   a present error? when the slot is an option. */

bool jog_lower_error_open(Lower *L);
void jog_lower_error_close(Lower *L);
void jog_lower_error_reset(Lower *L);

void jog_lower_error_note(Lower *L, JogNode *node);
void jog_lower_error_block_end(Lower *L, size_t base, JogPos pos);
void jog_lower_error_exit(Lower *L, size_t base, uint32_t target, JogPos pos);
void jog_lower_error_return(Lower *L, JogNode *node);

void jog_lower_error_binding(Lower *L, JogNode *node);
void jog_lower_error_destructure(Lower *L, JogNode *node);

JirOperand jog_lower_error_tuple(Lower *L, JogNode *node);
JirOperand jog_lower_error_field(Lower *L, JogNode *node);

/* Address of a nameless tuple field. A pointer base is already that
   address. lower_assign stores through this pointer. */
JirOperand jog_lower_tuple_field_ptr(Lower *L, JogNode *member);
JirOperand jog_lower_error_null(Lower *L, JogNode *node);

/* src when slot is not error or error?, or src is already that type.
   A pointer becomes an interface whose words are both that address.
   An error value wrapped for error? is a present option. none after a failure. */
JirOperand jog_lower_error_box(Lower *L, JirOperand src, JogType *slot, JogPos pos);

/* True when node is an option, null, or sentinel comparison and *out is set. */
bool jog_lower_error_compare(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out);

/* True when call is the builtin panic. The block is terminated. */
bool jog_lower_error_panic(Lower *L, JogNode *node);

#endif
