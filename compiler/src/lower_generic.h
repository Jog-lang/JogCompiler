#ifndef JOG_LOWER_GENERIC_H
#define JOG_LOWER_GENERIC_H

#include "lower_internal.h"

/* Monomorphized copies of generic functions and methods. Not a contract
   header. The active substitution is Lower.subst_from / subst_to. A type
   parameter in that list is replaced; every other type is rebuilt only when
   a part changes. An alias use is already the substituted target, so a type
   parameter that remains belongs to the function being monomorphized.
   A method instance is jog_<pkg>__<Type>__<method> plus type arguments.
   The template method is not emitted. */

JogType *jog_lower_type(Lower *L, JogType *type);

/* True for `type Name<T> = ...`. The template is not a value and is not
   emitted. Uses lower the substituted target. */
bool jog_lower_is_alias_template(const JogNode *decl);

/* True when a type parameter remains inside type. */
bool jog_lower_type_is_open(const JogType *type);

/* Declare the instance this call needs and return its JIR index. The body
   is queued. -1 when lowering has failed. */
int32_t jog_lower_generic_callee(Lower *L, JogNode *call);

/* The next instance body. Sets L's substitution. False when none remain. */
bool jog_lower_generic_take(Lower *L, JogNode **fn, int32_t *jir_index);

#endif
