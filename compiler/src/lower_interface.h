#ifndef JOG_LOWER_INTERFACE_H
#define JOG_LOWER_INTERFACE_H

#include "lower_internal.h"

/* Method tables, conversion to an interface, calls, properties, as / as?,
   and is. Not a contract header. An interface value is the two-pointer
   aggregate the C backend already emits. The table itself is a static C
   object, not a JIR data image: function pointers are not relocations.
   Property offsets are size_t slots after the method pointers. */

/* Build the two-pointer value. src is *T or *const T. */
JirOperand jog_lower_iface_convert(Lower *L, JirOperand src, JogType *iface, JogPos pos);

/* True when node is a call through an interface and *out is set. */
bool jog_lower_iface_call(Lower *L, JogNode *node, JirOperand *out);

/* True when node is an interface as or as? and *out is set. */
bool jog_lower_iface_cast(Lower *L, JogNode *node, JirOperand *out);

/* True when node loads a property through an interface and *out is set. */
bool jog_lower_iface_load(Lower *L, JogNode *node, JirOperand *out);

/* True when node stores a property through an interface. */
bool jog_lower_iface_store(Lower *L, JogNode *node);

/* True when node is `v is T` on an interface and *out is the bool. */
bool jog_lower_iface_is(Lower *L, JogNode *node, JirOperand *out);

/* True when node is when on an interface. as_value selects a result. */
bool jog_lower_iface_when(Lower *L, JogNode *node, bool as_value, JirOperand *out);

/* True when raw is an interface value and node was narrowed to *T.
   *out is the data pointer bitcast to node->type. */
bool jog_lower_iface_narrowed(Lower *L, JogNode *node, JirOperand raw, JirOperand *out);

#endif
