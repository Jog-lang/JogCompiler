#ifndef JOG_CHECK_CONSTRAINT_H
#define JOG_CHECK_CONSTRAINT_H

#include "check_body.h"

/* Constraints are interfaces. A type parameter with a constraint gets the
   operations every member of that set shares: type-set builtins, required
   methods, and required properties. An unconstrained parameter still has
   no operations. A type-set interface is not a value type. Not a contract
   header. */

/* Resolve param's constraint. *constraint is the interface, or NULL when
   there is none or the constraint is invalid. *bad is set on a signature
   error. False only when the checker stops. */
bool jog_constraint_bind(Check *ck, CheckJob *job, JogNode *param, JogType **constraint, int *bad);

/* NULL when every argument satisfies its parameter's constraint. The error
   type when one does not. NULL when the checker has stopped. */
JogType *jog_constraint_check_args(Check *ck, CheckJob *job, const JogSymbol *sym, JogType *const *args,
                                  size_t count, JogPos pos);

/* True when type is an interface that lists a type set. */
bool jog_constraint_is_typeset(const JogType *type);

/* Check a property, an embedded interface, or a type set on an interface.
   False only when the checker stops. */
bool jog_constraint_iface_member(Check *ck, CheckJob *job, JogNode *iface, JogNode *member);

/* 1 when this operator is allowed on a constrained type parameter (*out is
   set). 0 when the caller keeps its own rule. -1 when the checker stopped. */
int jog_constraint_binary(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out);
int jog_constraint_unary(Check *ck, JogNode *node, JogType *operand, JogType **out);

/* 1 when member is a property (or a method used as a value) of a constrained
   type parameter. outer is that parameter, already peeled from a pointer. */
int jog_constraint_member(Check *ck, CheckJob *job, JogNode *member, JogType *outer, JogType **out);

/* 1 when call is a method required by a constrained type parameter.
   base_ty is the receiver, already checked. */
int jog_constraint_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *base_ty,
                        JogType **out);

#endif
