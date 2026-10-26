#ifndef JOG_LOWER_STRUCT_H
#define JOG_LOWER_STRUCT_H

#include "lower_internal.h"

/* Struct literals, field reads and writes, and method calls.
   Not a contract header. */

/* True when call is a struct literal or a method call. *out is the result
   value, or none for a unit method. */
bool jog_lower_struct_call(Lower *L, JogNode *node, JirOperand *out);

JirOperand jog_lower_field(Lower *L, JogNode *node);

/* Address of a field, without loading it. */
JirOperand jog_lower_field_ptr(Lower *L, JogNode *node);
void jog_lower_field_assign(Lower *L, JogNode *node);

/* The self symbol bound in fn's body, or NULL when self is unused. */
JogSymbol *jog_lower_self_symbol(JogNode *fn);

/* Parameter name for JIR slot index, including the leading self. */
const char *jog_lower_param_name(JogNode *fn, size_t index);

#endif
