#ifndef JOG_LOWER_CONSTRAINT_H
#define JOG_LOWER_CONSTRAINT_H

#include "lower_internal.h"

/* Monomorphized constraint operations. A builtin operand (an integer, bool,
   str, or a distinct type over one of those) keeps the ordinary operator.
   A nominal type calls the method the constraint required. Not a contract
   header. */

/* 1 when call is a method on a constrained type parameter. *out is the
   result. 0 when it is not. */
int jog_lower_constraint_call(Lower *L, JogNode *node, JirOperand *out);

/* 1 when node is an operator on a constrained type parameter that is not a
   builtin operand. 0 when lowering should keep the ordinary operator. */
int jog_lower_constraint_binop(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out);

#endif
