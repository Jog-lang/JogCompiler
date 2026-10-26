#ifndef JOG_CHECK_GENERIC_H
#define JOG_CHECK_GENERIC_H

#include "check_body.h"

/* Unconstrained function, struct, enum, alias, and method type parameters.
   Each use is monomorphized. A transparent alias is the substituted target,
   not a new nominal type. A method's receiver names declare type parameters
   for every instance. Constraints, generic interfaces, and distinct types
   with type parameters are later steps. Not a contract header. */

/* Bind JOG_TY_TYPE_PARAM for fn's type parameters. Sets *bad on a
   signature error and keeps going. False only when the checker stops. */
bool jog_generic_bind_params(Check *ck, CheckJob *job, bool *bad);

/* Bind a struct or enum's type parameters onto its symbol. Same rules as a
   function, without the extern, export, test, and main rejects. */
bool jog_generic_bind_struct_params(Check *ck, CheckJob *job, bool *bad);

/* Bind a transparent alias's type parameters onto its symbol. */
bool jog_generic_bind_alias_params(Check *ck, CheckJob *job, bool *bad);

/* Prepend the receiver's type-parameter names to any type parameters
   jog_generic_bind_params already stored. expect is the struct or enum
   type-parameter count. owner is that struct or enum; each parameter keeps
   the constraint of the owner parameter in the same position. *skip_recv
   is set when the receiver must not be resolved. False only when the
   checker stops. */
bool jog_generic_bind_receiver_params(Check *ck, CheckJob *job, JogSymbol *owner, size_t expect, bool *bad,
                                      int *skip_recv);

/* Leading type parameters that came from the receiver. 0 when the method's
   only type parameters are its own, or when sym is not a method. */
size_t jog_generic_method_recv_params(const JogSymbol *sym);

/* One struct for this argument list. An open list is not laid out.
   NULL on out of memory. */
JogType *jog_generic_struct_instance(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *const *args,
                                    size_t count);

/* Resolve args against a generic struct. The error type on failure.
   NULL when the checker has stopped. */
JogType *jog_generic_struct_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos);

/* Fill struct instances that were named before the template fields existed. */
bool jog_generic_struct_finish(JogTypes *types, CtArena *arena, JogSymbol *sym);

/* One enum for this argument list. An open list is not laid out.
   NULL on out of memory. */
JogType *jog_generic_enum_instance(JogTypes *types, CtArena *arena, JogSymbol *sym, JogType *const *args,
                                  size_t count);

/* Resolve args against a generic enum. The error type on failure.
   NULL when the checker has stopped. */
JogType *jog_generic_enum_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos);

/* Fill enum instances that were named before the template variants existed. */
bool jog_generic_enum_finish(JogTypes *types, CtArena *arena, JogSymbol *sym);

/* Substitute args into a generic alias. The result is the target type, and
   it stays open when an argument still names a type parameter. The error
   type on failure. NULL when the checker has stopped. */
JogType *jog_generic_alias_apply(Check *ck, CheckJob *job, JogSymbol *sym, JogList args, JogPos pos);

/* A default whose type mentions one of fn's type parameters is rejected. */
void jog_generic_reject_param_defaults(Check *ck, JogNode *fn, bool *bad);

/* The type parameter named name on the job's function, or NULL. */
JogType *jog_generic_lookup_param(const CheckJob *job, const char *name);

/* True when type is a type parameter or contains one. */
bool jog_generic_has_param(const JogType *type);

/* params[i] is replaced by args[i]. Types with no matching parameter are
   rebuilt only when a part changed, so interned identity is kept. */
JogType *jog_generic_subst(JogTypes *types, CtArena *arena, JogType *type, JogType *const *params, JogType *const *args,
                          size_t count);

typedef JogType *(*JogGenericCheckExpr)(void *env, JogNode *node, JogType *target);

/* Substituted signature of a generic call. Stores the solved arguments on
   the call. NULL when the checker has stopped. The error type on failure. */
JogType *jog_check_generic_instantiate(Check *ck, CheckJob *job, void *env, JogNode *call, JogNode *callee,
                                      JogType *fn_type, JogGenericCheckExpr check_expr);

/* Monomorphize a method call. recv is the peeled receiver value, or the
   explicit type for Type.method(x). implicit is 1 when self is not written.
   Receiver type parameters come from recv. The method's own type parameters
   are inferred or taken from callee's explicit type arguments. Stores every
   solved argument on the call, receiver parameters first. NULL when the
   checker has stopped. The error type on failure. */
JogType *jog_check_generic_method(Check *ck, CheckJob *job, void *env, JogNode *call, JogNode *callee, JogType *recv,
                                 int implicit, JogGenericCheckExpr check_expr);

#endif
