#ifndef JOG_CHECK_MEMORY_H
#define JOG_CHECK_MEMORY_H

#include "check_body.h"

/* Pointers, arrays, slices, bytes, indexing, and pointer arithmetic.
   Not a contract header. There are no implicit conversions: *T does not
   become *const T, and []T does not become []const T. */

/* 1 when this call is bytes(). 0 when the caller should keep checking.
   -1 when the checker has stopped. */
int jog_check_memory_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call);

JogType *jog_check_memory_unary(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_memory_index(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_memory_slice(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);
JogType *jog_check_memory_array_lit(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node);

/* 1 when node is pointer arithmetic or a pointer comparison and *out is set.
   0 when neither side is a pointer. -1 when the checker has stopped. */
int jog_check_memory_binary(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out);

/* Index or dereference assignment, including compound updates. */
void jog_check_memory_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign);

/* Right-hand side of p += n or p -= n. 1 when checked (an error may already
   be reported). -1 when the checker has stopped. */
int jog_check_memory_ptr_compound(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *rhs, JogType *ptr_ty);

/* 1 when the block should still be checked. -1 when the checker has stopped.
   @unchecked is accepted. Any other attribute is reported and the body is
   still checked. */
int jog_check_memory_attr(Check *ck, JogNode *node);

#endif
