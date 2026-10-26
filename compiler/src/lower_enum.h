#ifndef JOG_LOWER_ENUM_H
#define JOG_LOWER_ENUM_H

#include "lower_internal.h"

/* Enum construction, when, patterns, Option sugar, and is.
   Not a contract header. when on an enum or on integer literals is a
   JIR SWITCH. Ranges, options, conditions, refutable payloads, and
   struct patterns are compare chains. null and niche *T? stay in
   lower_error.c. An in-pattern a..b is half-open, matching for-loops.
   An omitted variant field with a default is lowered at the call. */

JirOperand jog_lower_enum_when(Lower *L, JogNode *node, bool as_value);
JirOperand jog_lower_enum_is(Lower *L, JogNode *node);
JirOperand jog_lower_enum_force(Lower *L, JogNode *node);
JirOperand jog_lower_enum_elvis(Lower *L, JogNode *node);
JirOperand jog_lower_enum_safe(Lower *L, JogNode *node);
JirOperand jog_lower_enum_dot(Lower *L, JogNode *node);

/* True when the call or member builds a variant. */
bool jog_lower_enum_call(Lower *L, JogNode *node, JirOperand *out);
bool jog_lower_enum_member(Lower *L, JogNode *node, JirOperand *out);

/* True when cast reads an enum tag. */
bool jog_lower_enum_cast(Lower *L, JogNode *node, JirOperand *out);

#endif
