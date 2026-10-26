#ifndef JOG_IFACE_TABLE_H
#define JOG_IFACE_TABLE_H

#include "ir.h"

/* One method slot in a concrete type's table for an interface.
   func is the thunk. fn_type is that thunk's extern function type, which
   is one C function pointer. Not a contract header. */
typedef struct JirIfaceSlot {
    int32_t func;
    JogType *fn_type;
} JirIfaceSlot;

/* Vtable for one (concrete, interface) pair. global is a dummy u8 whose
   name emit_c prints as the real static table. type_id_global is the
   unique static byte whose address is the concrete type's type id.
   The C object is { type_id, size, align, method pointers..., offsets... }.
   offsets[i] is the byte offset of property i inside concrete. */
struct JirIfaceTable {
    const char *name;
    const char *type_id_name;
    int32_t global;
    int32_t type_id_global;
    JogType *concrete;
    JogType *iface;
    JirIfaceSlot *slots;
    size_t slot_count;
    uint64_t *offsets;
    size_t offset_count;
};

#endif
