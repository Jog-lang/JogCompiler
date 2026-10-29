#include "lower_memory.h"

#include "lower_error.h"
#include "lower_generic.h"
#include "lower_struct.h"

#include <string.h>

/* Arrays are C structs with the payload at offset 0, so a bitcast from
   *[N]T to *T is the first element. [N]byte bitcasts to *u8 or *byte.
   Slice and bytes headers are (ptr, len). A slice literal in a function
   owns a hidden stack array. A pack argument is that same header: one
   stack array of the elements, or a zero slice when there are none.
   Bounds checks are strict <, so hi <= len is hi < len + 1. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static JogType *builtin(Lower *L, JogBuiltin which) {
    JogType *type = jog_type_builtin(L->program->types, which);
    if (!type) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
    }
    return type;
}

static JirOperand local_op(Lower *L, uint32_t local) {
    if (L->failed) {
        return jir_none();
    }
    return jog_lower_local_op(L, local);
}

static JirOperand emit_a(Lower *L, JirOp op, JogType *type, JirOperand a, uint32_t aux, bool checked, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, op, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = aux;
    inst->checked = checked;
    inst->a = a;
    return local_op(L, dst);
}

static JirOperand emit_ab(Lower *L, JirOp op, JogType *type, JirOperand a, JirOperand b, uint32_t aux, bool checked, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, op, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = aux;
    inst->checked = checked;
    inst->a = a;
    inst->b = b;
    return local_op(L, dst);
}

static JirOperand emit_binop(Lower *L, JirBinOp op, bool checked, JirOperand a, JirOperand b, JogType *type, JogPos pos) {
    return emit_ab(L, JIR_BINARY, type, a, b, (uint32_t)op, checked, pos);
}

static JirOperand emit_bitcast(Lower *L, JirOperand src, JogType *type, JogPos pos) {
    if (L->failed) {
        return jir_none();
    }
    if (src.type == type) {
        return src;
    }
    return emit_a(L, JIR_BITCAST, type, src, 0, false, pos);
}

static JirOperand emit_convert(Lower *L, JirOperand src, JogType *type, JogPos pos) {
    if (src.type == type) {
        return src;
    }
    return emit_a(L, JIR_CONVERT, type, src, 0, true, pos);
}

static JirOperand emit_load(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem) {
        jog_lower_fail(L, pos, "cannot load this pointer");
        return jir_none();
    }
    return emit_a(L, JIR_LOAD, elem, ptr, 0, false, pos);
}

static void emit_store(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
    JirInst *inst;
    if (L->failed || L->terminated) {
        return;
    }
    inst = jog_lower_emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = ptr;
    inst->b = value;
}

static void emit_bounds(Lower *L, JirOperand index, JirOperand len, JogPos pos) {
    JirInst *inst;
    if (L->failed || L->unchecked) {
        return;
    }
    inst = jog_lower_emit(L, JIR_BOUNDS_CHECK, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = index;
    inst->b = len;
}

static JirOperand emit_elem(Lower *L, JirOperand ptr, JirOperand index, JogType *ptr_ty, JogPos pos) {
    if (ptr.type != ptr_ty) {
        ptr = emit_bitcast(L, ptr, ptr_ty, pos);
    }
    return emit_ab(L, JIR_ELEM_ADDR, ptr_ty, ptr, index, 0, false, pos);
}

static JirOperand emit_extract(Lower *L, JirOperand base, uint32_t field, JogType *type, JogPos pos) {
    base = jog_lower_as_local(L, base, pos);
    return emit_a(L, JIR_EXTRACT, type, base, field, false, pos);
}

static JirOperand emit_agg2(Lower *L, JogType *type, JirOperand a, JirOperand b, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    JirOperand args[2];
    if (L->failed || !type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    args[0] = a;
    args[1] = b;
    if (!jir_set_args(L->module, inst, args, 2)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return local_op(L, dst);
}

static JirOperand emit_zero_value(Lower *L, JogType *type, JogPos pos) {
    uint32_t dst;
    if (!type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    jog_lower_zero(L, dst, pos);
    return local_op(L, dst);
}

static JirOperand addr_local(Lower *L, uint32_t local, JogPos pos) {
    JirLocal *slot = L->func ? jir_local_at(L->func, local) : NULL;
    JogType *value = slot ? slot->type : NULL;
    JogType *ptr = value ? jog_type_ptr(L->program->types, value, false) : NULL;
    JirInst *inst;
    uint32_t dst;
    if (!ptr || !jog_lower_new_local(L, NULL, ptr, &dst)) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "out of memory");
        }
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jog_lower_local_op(L, local);
    return local_op(L, dst);
}

static JirOperand usize_add1(Lower *L, JirOperand v, JogPos pos) {
    JogType *usize = builtin(L, JOG_BUILTIN_USIZE);
    if (!usize) {
        return jir_none();
    }
    return emit_binop(L, JIR_ADD, true, v, jir_int(usize, 1), usize, pos);
}

static bool map_binop(JogTokenKind op, JirBinOp *out, bool *checked) {
    *checked = false;
    switch (op) {
    case JOG_TOK_PLUS:
        *out = JIR_ADD;
        *checked = true;
        return true;
    case JOG_TOK_MINUS:
        *out = JIR_SUB;
        *checked = true;
        return true;
    case JOG_TOK_STAR:
        *out = JIR_MUL;
        *checked = true;
        return true;
    case JOG_TOK_SLASH:
        *out = JIR_DIV;
        *checked = true;
        return true;
    case JOG_TOK_PERCENT:
        *out = JIR_REM;
        *checked = true;
        return true;
    case JOG_TOK_AMP:
        *out = JIR_AND;
        return true;
    case JOG_TOK_PIPE:
        *out = JIR_OR;
        return true;
    case JOG_TOK_CARET:
        *out = JIR_XOR;
        return true;
    case JOG_TOK_SHL:
        *out = JIR_SHL;
        *checked = true;
        return true;
    case JOG_TOK_SHR:
        *out = JIR_SHR;
        *checked = true;
        return true;
    case JOG_TOK_EQ:
        *out = JIR_EQ;
        return true;
    case JOG_TOK_NEQ:
        *out = JIR_NE;
        return true;
    case JOG_TOK_LT:
        *out = JIR_LT;
        return true;
    case JOG_TOK_LE:
        *out = JIR_LE;
        return true;
    case JOG_TOK_GT:
        *out = JIR_GT;
        return true;
    case JOG_TOK_GE:
        *out = JIR_GE;
        return true;
    default:
        return false;
    }
}

static JogTokenKind compound_base(JogTokenKind op) {
    switch (op) {
    case JOG_TOK_PLUS_ASSIGN:
        return JOG_TOK_PLUS;
    case JOG_TOK_MINUS_ASSIGN:
        return JOG_TOK_MINUS;
    case JOG_TOK_STAR_ASSIGN:
        return JOG_TOK_STAR;
    case JOG_TOK_SLASH_ASSIGN:
        return JOG_TOK_SLASH;
    case JOG_TOK_PERCENT_ASSIGN:
        return JOG_TOK_PERCENT;
    case JOG_TOK_AMP_ASSIGN:
        return JOG_TOK_AMP;
    case JOG_TOK_PIPE_ASSIGN:
        return JOG_TOK_PIPE;
    case JOG_TOK_CARET_ASSIGN:
        return JOG_TOK_CARET;
    case JOG_TOK_SHL_ASSIGN:
        return JOG_TOK_SHL;
    case JOG_TOK_SHR_ASSIGN:
        return JOG_TOK_SHR;
    default:
        return JOG_TOK_EOF;
    }
}

static JirOperand value_addr(Lower *L, JogNode *node);
static JirOperand index_ptr(Lower *L, JogNode *node);

static JirOperand value_addr(Lower *L, JogNode *node) {
    uint32_t local;
    JirOperand global;
    JirOperand value;
    if (!node || L->failed) {
        return jir_none();
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        return jog_lower_expr(L, node->as.unary.operand);
    }
    if (node->kind == JOG_NODE_INDEX) {
        return index_ptr(L, node);
    }
    if (node->kind == JOG_NODE_MEMBER && node->symbol && node->symbol->kind == JOG_SYM_FIELD) {
        JirOperand field = jog_lower_field_ptr(L, node);
        if (L->failed) {
            return jir_none();
        }
        return field;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        if (jog_lower_find_local(L, node->symbol, &local)) {
            return addr_local(L, local, node->pos);
        }
        global = jog_lower_global_addr(L, node->symbol);
        if (global.kind != JIR_OPND_NONE) {
            return global;
        }
        jog_lower_fail(L, node->pos, "missing address");
        return jir_none();
    }
    value = jog_lower_as_local(L, jog_lower_expr(L, node), node->pos);
    if (L->failed || value.kind != JIR_OPND_LOCAL) {
        return jir_none();
    }
    return addr_local(L, value.as.local, node->pos);
}

JirOperand jog_lower_lvalue_ptr(Lower *L, JogNode *node) {
    return value_addr(L, node);
}

/* Pointer to the indexed element, before the load. */
static JirOperand index_ptr(Lower *L, JogNode *node) {
    JogNode *base_node;
    JogType *base;
    JogType *elem;
    JogType *ptr_ty;
    JogType *usize;
    JirOperand index;
    JirOperand storage;
    JirOperand header;
    JirOperand raw;
    JirOperand len;
    bool view_const = false;
    uint64_t n = 0;
    bool bounded = false;
    if (!node || L->failed) {
        return jir_none();
    }
    base_node = node->as.index.base;
    base = base_node ? jog_lower_type(L, base_node->type) : NULL;
    if (base && base->kind == JOG_TY_DISTINCT && jog_type_bytes_view(base)) {
        base = jog_type_bytes_view(base);
    }
    usize = builtin(L, JOG_BUILTIN_USIZE);
    if (!base || !usize || !base_node) {
        jog_lower_fail(L, node->pos, "cannot index this value");
        return jir_none();
    }
    index = jog_lower_expr(L, node->as.index.index);
    if (L->failed) {
        return jir_none();
    }
    if (base->kind == JOG_TY_PTR) {
        view_const = base->is_const;
        elem = base->as.elem.elem;
        ptr_ty = jog_type_ptr(L->program->types, elem, view_const);
        storage = jog_lower_expr(L, base_node);
        if (!ptr_ty || L->failed) {
            return jir_none();
        }
        return emit_elem(L, storage, index, ptr_ty, node->pos);
    }
    if (base->kind == JOG_TY_SLICE) {
        view_const = base->is_const;
        elem = base->as.elem.elem;
        ptr_ty = jog_type_ptr(L->program->types, elem, view_const);
        header = jog_lower_expr(L, base_node);
        raw = emit_extract(L, header, 0, ptr_ty, node->pos);
        len = emit_extract(L, header, 1, usize, node->pos);
        bounded = true;
    } else if (base->kind == JOG_TY_BYTES) {
        JogType *u8 = builtin(L, JOG_BUILTIN_U8);
        JogType *byte = jog_type_bytes_n(L->program->types, 1);
        view_const = base->is_const;
        ptr_ty = byte ? jog_type_ptr(L->program->types, byte, view_const) : NULL;
        header = jog_lower_expr(L, base_node);
        raw = emit_extract(L, header, 0, u8 ? jog_type_ptr(L->program->types, u8, view_const) : NULL, node->pos);
        len = emit_extract(L, header, 1, usize, node->pos);
        raw = emit_bitcast(L, raw, ptr_ty, node->pos);
        bounded = true;
    } else if (base->kind == JOG_TY_ARRAY || base->kind == JOG_TY_BYTES_N) {
        if (base->kind == JOG_TY_BYTES_N) {
            JogType *byte = jog_type_bytes_n(L->program->types, 1);
            n = base->as.bytes_n.n;
            ptr_ty = byte ? jog_type_ptr(L->program->types, byte, false) : NULL;
        } else {
            n = base->as.array.len;
            ptr_ty = jog_type_ptr(L->program->types, base->as.array.elem, false);
        }
        storage = value_addr(L, base_node);
        raw = emit_bitcast(L, storage, ptr_ty, node->pos);
        len = jir_int(usize, n);
        bounded = true;
    } else {
        jog_lower_fail(L, node->pos, "cannot index this value");
        return jir_none();
    }
    if (L->failed || !ptr_ty) {
        return jir_none();
    }
    if (bounded) {
        emit_bounds(L, index, len, node->pos);
    }
    return emit_elem(L, raw, index, ptr_ty, node->pos);
}

static JirOperand lower_slice(Lower *L, JogNode *node) {
    JogNode *base_node = node->as.slice.base;
    JogType *base = base_node ? jog_lower_type(L, base_node->type) : NULL;
    JogType *result = jog_lower_type(L, node->type);
    if (base && base->kind == JOG_TY_DISTINCT && jog_type_bytes_view(base)) {
        base = jog_type_bytes_view(base);
    }
    JogType *usize;
    JogType *elem = NULL;
    JogType *ptr_ty;
    JogType *agg_ptr;
    JirOperand storage;
    JirOperand header;
    JirOperand first;
    JirOperand len;
    JirOperand lo;
    JirOperand hi;
    JirOperand span;
    JirOperand ptr;
    bool view_const = false;
    uint64_t n = 0;
    if (!base || !result || L->failed) {
        jog_lower_fail(L, node->pos, "cannot slice this value");
        return jir_none();
    }
    usize = builtin(L, JOG_BUILTIN_USIZE);
    if (!usize) {
        return jir_none();
    }
    if (base->kind == JOG_TY_SLICE) {
        view_const = base->is_const;
        elem = base->as.elem.elem;
        ptr_ty = jog_type_ptr(L->program->types, elem, view_const);
        header = jog_lower_expr(L, base_node);
        first = emit_extract(L, header, 0, ptr_ty, node->pos);
        len = emit_extract(L, header, 1, usize, node->pos);
    } else if (base->kind == JOG_TY_BYTES) {
        JogType *u8 = builtin(L, JOG_BUILTIN_U8);
        view_const = base->is_const;
        ptr_ty = u8 ? jog_type_ptr(L->program->types, u8, view_const) : NULL;
        header = jog_lower_expr(L, base_node);
        first = emit_extract(L, header, 0, ptr_ty, node->pos);
        len = emit_extract(L, header, 1, usize, node->pos);
    } else if (base->kind == JOG_TY_ARRAY) {
        elem = base->as.array.elem;
        n = base->as.array.len;
        view_const = result->is_const;
        ptr_ty = jog_type_ptr(L->program->types, elem, view_const);
        storage = value_addr(L, base_node);
        first = emit_bitcast(L, storage, ptr_ty, node->pos);
        len = jir_int(usize, n);
    } else if (base->kind == JOG_TY_BYTES_N) {
        JogType *u8 = builtin(L, JOG_BUILTIN_U8);
        n = base->as.bytes_n.n;
        view_const = result->kind == JOG_TY_BYTES && result->is_const;
        ptr_ty = u8 ? jog_type_ptr(L->program->types, u8, view_const) : NULL;
        storage = value_addr(L, base_node);
        first = emit_bitcast(L, storage, ptr_ty, node->pos);
        len = jir_int(usize, n);
    } else {
        jog_lower_fail(L, node->pos, "cannot slice this value");
        return jir_none();
    }
    if (L->failed || !ptr_ty) {
        return jir_none();
    }
    lo = node->as.slice.lo ? jog_lower_expr(L, node->as.slice.lo) : jir_int(usize, 0);
    hi = node->as.slice.hi ? jog_lower_expr(L, node->as.slice.hi) : len;
    if (L->failed) {
        return jir_none();
    }
    if (!L->unchecked && node->as.slice.lo && node->as.slice.hi) {
        emit_bounds(L, lo, usize_add1(L, hi, node->pos), node->pos);
        emit_bounds(L, hi, usize_add1(L, len, node->pos), node->pos);
    } else if (!L->unchecked && node->as.slice.lo) {
        emit_bounds(L, lo, usize_add1(L, len, node->pos), node->pos);
    } else if (!L->unchecked && node->as.slice.hi) {
        emit_bounds(L, hi, usize_add1(L, len, node->pos), node->pos);
    }
    span = emit_binop(L, JIR_SUB, true, hi, lo, usize, node->pos);
    ptr = emit_elem(L, first, lo, ptr_ty, node->pos);
    {
        JogType *agg = result;
        JogType *view = jog_type_bytes_view(result);
        if (view && result->kind == JOG_TY_DISTINCT) {
            agg = view;
        }
        if (agg->kind == JOG_TY_BYTES) {
            JogType *u8 = builtin(L, JOG_BUILTIN_U8);
            agg_ptr = u8 ? jog_type_ptr(L->program->types, u8, agg->is_const) : NULL;
        } else {
            agg_ptr = jog_type_ptr(L->program->types, agg->as.elem.elem, agg->is_const);
        }
    }
    ptr = emit_bitcast(L, ptr, agg_ptr, node->pos);
    return emit_agg2(L, result, ptr, span, node->pos);
}

static bool fill_array(Lower *L, uint32_t slot, JogType *array_ty, JogList elems, JogPos pos) {
    JogType *elem;
    JogType *elem_ptr;
    JogType *usize;
    JirOperand first;
    size_t i;
    jog_lower_zero(L, slot, pos);
    if (elems.count == 0 || L->failed) {
        return !L->failed;
    }
    usize = builtin(L, JOG_BUILTIN_USIZE);
    if (array_ty->kind == JOG_TY_BYTES_N) {
        elem = jog_type_bytes_n(L->program->types, 1);
    } else {
        elem = array_ty->as.array.elem;
    }
    elem_ptr = elem ? jog_type_ptr(L->program->types, elem, false) : NULL;
    first = emit_bitcast(L, addr_local(L, slot, pos), elem_ptr, pos);
    if (!usize || !elem_ptr || L->failed) {
        return false;
    }
    for (i = 0; i < elems.count && !L->failed; i++) {
        JogNode *arg = list_at(elems, i);
        JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
        JirOperand got;
        JirOperand at;
        if (!value) {
            jog_lower_fail(L, pos, "missing array element");
            return false;
        }
        got = jog_lower_expr(L, value);
        at = emit_elem(L, first, jir_int(usize, (uint64_t)i), elem_ptr, value->pos);
        emit_store(L, at, got, value->pos);
    }
    return !L->failed;
}

static JirOperand lower_slice_lit(Lower *L, JogNode *node, JogType *slice_ty) {
    JogType *elem;
    JogType *array_ty;
    JogType *ptr_ty;
    uint32_t slot;
    size_t count = node->as.array_lit.elems.count;
    if (count == 0) {
        return emit_zero_value(L, slice_ty, node->pos);
    }
    if (!L->ast_fn) {
        jog_lower_fail(L, node->pos, "slice literal in a global initializer does not outlive initialization");
        return jir_none();
    }
    if (slice_ty->kind == JOG_TY_BYTES) {
        JogType *u8 = builtin(L, JOG_BUILTIN_U8);
        /* Elements are byte. The view header still stores *u8. */
        array_ty = jog_type_bytes_n(L->program->types, (uint64_t)count);
        ptr_ty = u8 ? jog_type_ptr(L->program->types, u8, slice_ty->is_const) : NULL;
    } else {
        elem = slice_ty->as.elem.elem;
        if (!elem) {
            return jir_none();
        }
        array_ty = jog_type_array(L->program->types, elem, (uint64_t)count);
        ptr_ty = jog_type_ptr(L->program->types, elem, slice_ty->is_const);
    }
    if (!array_ty || !ptr_ty || !jog_lower_new_local(L, NULL, array_ty, &slot)) {
        if (!L->failed) {
            jog_lower_fail(L, node->pos, "out of memory");
        }
        return jir_none();
    }
    if (!fill_array(L, slot, array_ty, node->as.array_lit.elems, node->pos)) {
        return jir_none();
    }
    return emit_agg2(L, slice_ty, emit_bitcast(L, addr_local(L, slot, node->pos), ptr_ty, node->pos),
                     jir_int(builtin(L, JOG_BUILTIN_USIZE), (uint64_t)count), node->pos);
}

static JirOperand lower_array_lit(Lower *L, JogNode *node) {
    JogType *type = node ? node->type : NULL;
    uint32_t slot;
    if (!type || L->failed) {
        return jir_none();
    }
    if (type->kind == JOG_TY_SLICE || type->kind == JOG_TY_BYTES) {
        return lower_slice_lit(L, node, type);
    }
    if (type->kind != JOG_TY_ARRAY && type->kind != JOG_TY_BYTES_N) {
        jog_lower_fail(L, node->pos, "array literal type is not an array or a slice");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, type, &slot)) {
        return jir_none();
    }
    if (!fill_array(L, slot, type, node->as.array_lit.elems, node->pos)) {
        return jir_none();
    }
    return local_op(L, slot);
}

static JirOperand pointer_add(Lower *L, JirOperand ptr, JirOperand offset, JogType *ptr_ty, JogPos pos) {
    return emit_elem(L, ptr, offset, ptr_ty, pos);
}

static JirOperand pointer_sub_int(Lower *L, JirOperand ptr, JirOperand offset, JogType *ptr_ty, JogPos pos) {
    JogType *isize = builtin(L, JOG_BUILTIN_ISIZE);
    JirOperand as_isize;
    JirOperand neg;
    if (!isize) {
        return jir_none();
    }
    as_isize = emit_convert(L, offset, isize, pos);
    neg = emit_a(L, JIR_UNARY, isize, as_isize, (uint32_t)JIR_NEG, true, pos);
    return emit_elem(L, ptr, neg, ptr_ty, pos);
}

static JirOperand pointer_diff(Lower *L, JirOperand lhs, JirOperand rhs, JogPos pos) {
    JogType *isize = builtin(L, JOG_BUILTIN_ISIZE);
    JogType *elem = lhs.type && lhs.type->kind == JOG_TY_PTR ? lhs.type->as.elem.elem : NULL;
    JirOperand a;
    JirOperand b;
    JirOperand diff;
    uint64_t size;
    if (!isize || !elem || !elem->has_layout || elem->size == 0) {
        jog_lower_fail(L, pos, "pointer arithmetic needs a size");
        return jir_none();
    }
    a = emit_bitcast(L, lhs, isize, pos);
    b = emit_bitcast(L, rhs, isize, pos);
    diff = emit_binop(L, JIR_SUB, false, a, b, isize, pos);
    size = elem->size;
    if (size == 1) {
        return diff;
    }
    return emit_binop(L, JIR_DIV, true, diff, jir_int(isize, size), isize, pos);
}

JirOperand jog_lower_memory_expr(Lower *L, JogNode *node) {
    if (!node || L->failed) {
        return jir_none();
    }
    if (node->kind == JOG_NODE_INDEX) {
        return emit_load(L, index_ptr(L, node), node->pos);
    }
    if (node->kind == JOG_NODE_SLICE) {
        return lower_slice(L, node);
    }
    if (node->kind == JOG_NODE_ARRAY_LIT) {
        return lower_array_lit(L, node);
    }
    jog_lower_fail(L, node->pos, "expression is not lowered yet");
    return jir_none();
}

JirOperand jog_lower_memory_unary(Lower *L, JogNode *node) {
    JirOperand value;
    if (!node || L->failed) {
        return jir_none();
    }
    if (node->as.unary.op == JOG_TOK_STAR) {
        value = jog_lower_expr(L, node->as.unary.operand);
        return emit_load(L, value, node->pos);
    }
    if (node->as.unary.op == JOG_TOK_AMP) {
        value = value_addr(L, node->as.unary.operand);
        if (L->failed) {
            return jir_none();
        }
        if (node->type && node->type->kind == JOG_TY_PTR && value.type != node->type) {
            value = emit_bitcast(L, value, node->type, node->pos);
        }
        return jog_lower_error_box(L, value, node->type, node->pos);
    }
    jog_lower_fail(L, node->pos, "operator is not lowered yet");
    return jir_none();
}

static bool is_error_option_type(const JogType *type) {
    const JogType *elem;
    if (!type || type->kind != JOG_TY_OPTION) {
        return false;
    }
    elem = type->as.elem.elem;
    while (elem && elem->kind == JOG_TY_DISTINCT) {
        elem = elem->as.nominal.base;
    }
    return elem && elem->kind == JOG_TY_INTERFACE && elem->as.nominal.name && strcmp(elem->as.nominal.name, "error") == 0;
}

bool jog_lower_memory_binary(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out) {
    bool lp;
    bool rp;
    JogTokenKind op;
    JirBinOp bin;
    bool checked = false;
    if (!out) {
        return false;
    }
    *out = jir_none();
    lp = lhs.type && lhs.type->kind == JOG_TY_PTR;
    rp = rhs.type && rhs.type->kind == JOG_TY_PTR;
    if (!lp && !rp) {
        return false;
    }
    if (!node || L->failed) {
        return true;
    }
    op = node->as.binary.op;
    /* error? compared with a sentinel pointer is lowered with the option. */
    if ((op == JOG_TOK_EQ || op == JOG_TOK_NEQ) &&
        ((lp && !rp && is_error_option_type(rhs.type)) || (rp && !lp && is_error_option_type(lhs.type)))) {
        return false;
    }
    if (lp && rp) {
        if (op == JOG_TOK_MINUS) {
            *out = pointer_diff(L, lhs, rhs, node->pos);
            return true;
        }
        if (!map_binop(op, &bin, &checked)) {
            jog_lower_fail(L, node->pos, "operator is not lowered yet");
            return true;
        }
        *out = emit_binop(L, bin, false, lhs, rhs, node->type, node->pos);
        return true;
    }
    if (op == JOG_TOK_PLUS) {
        *out = pointer_add(L, lp ? lhs : rhs, lp ? rhs : lhs, node->type, node->pos);
        return true;
    }
    if (op == JOG_TOK_MINUS && lp) {
        *out = pointer_sub_int(L, lhs, rhs, node->type, node->pos);
        return true;
    }
    jog_lower_fail(L, node->pos, "operator is not lowered yet");
    return true;
}

void jog_lower_memory_ptr_update(Lower *L, JirOperand slot, JogTokenKind op, JogNode *rhs, JogType *ptr_ty, JogPos pos) {
    JirOperand cur;
    JirOperand off;
    JirOperand next;
    if (L->failed) {
        return;
    }
    cur = emit_load(L, slot, pos);
    off = jog_lower_expr(L, rhs);
    if (L->failed || L->terminated) {
        return;
    }
    if (op == JOG_TOK_MINUS) {
        next = pointer_sub_int(L, cur, off, ptr_ty, pos);
    } else {
        next = pointer_add(L, cur, off, ptr_ty, pos);
    }
    emit_store(L, slot, next, pos);
}

static JirOperand assign_slot(Lower *L, JogNode *target) {
    if (target->kind == JOG_NODE_INDEX) {
        return index_ptr(L, target);
    }
    return jog_lower_expr(L, target->as.unary.operand);
}

bool jog_lower_memory_assign(Lower *L, JogNode *node) {
    JogNode *target;
    JogTokenKind base;
    JirOperand slot;
    JirOperand value;
    JirBinOp bin;
    bool checked = false;
    uint32_t local;
    if (!node || L->failed) {
        return false;
    }
    target = node->as.assign.target;
    base = compound_base(node->as.assign.op);
    if (target && target->kind == JOG_NODE_IDENT && (base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && target->type &&
        target->type->kind == JOG_TY_PTR) {
        if (!target->symbol) {
            jog_lower_fail(L, target->pos, "missing symbol");
            return true;
        }
        if (jog_lower_find_local(L, target->symbol, &local)) {
            slot = addr_local(L, local, target->pos);
        } else {
            slot = jog_lower_global_addr(L, target->symbol);
            if (slot.kind == JIR_OPND_NONE) {
                JirOperand cap;
                /* The child has no local for a capture. jog_lower_coro_store
                   loads that pointer, then the offset, then stores. */
                if (L->in_coro) {
                    return false;
                }
                if (jog_lower_lambda_addr(L, target->symbol, &cap)) {
                    slot = cap;
                } else {
                    jog_lower_fail(L, target->pos, "missing pointer");
                    return true;
                }
            }
        }
        jog_lower_memory_ptr_update(L, slot, base, node->as.assign.value, target->type, node->pos);
        return true;
    }
    if (!target || (target->kind != JOG_NODE_INDEX &&
                    !(target->kind == JOG_NODE_UNARY && target->as.unary.op == JOG_TOK_STAR))) {
        return false;
    }
    slot = assign_slot(L, target);
    if (L->failed) {
        return true;
    }
    if (base != JOG_TOK_EOF && target->type && target->type->kind == JOG_TY_PTR &&
        (base == JOG_TOK_PLUS || base == JOG_TOK_MINUS)) {
        jog_lower_memory_ptr_update(L, slot, base, node->as.assign.value, target->type, node->pos);
        return true;
    }
    if (base != JOG_TOK_EOF) {
        JirOperand old = emit_load(L, slot, node->pos);
        JirOperand rhs = jog_lower_expr(L, node->as.assign.value);
        if (L->failed || !map_binop(base, &bin, &checked)) {
            if (!L->failed) {
                jog_lower_fail(L, node->pos, "operator is not lowered yet");
            }
            return true;
        }
        value = emit_binop(L, bin, checked, old, rhs, target->type ? target->type : rhs.type, node->pos);
        emit_store(L, slot, value, node->pos);
        return true;
    }
    value = jog_lower_expr(L, node->as.assign.value);
    emit_store(L, slot, value, node->pos);
    return true;
}

bool jog_lower_memory_call(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee;
    JogType *type;
    if (!out) {
        return false;
    }
    *out = jir_none();
    if (!node || node->kind != JOG_NODE_CALL) {
        return false;
    }
    callee = node->as.call.callee;
    if (!callee || !callee->symbol || callee->symbol->kind != JOG_SYM_TYPE) {
        return false;
    }
    type = callee->symbol->type ? callee->symbol->type : callee->type;
    if (!type || type->kind != JOG_TY_BYTES || type->is_const) {
        return false;
    }
    if (node->as.call.args.count > 0 || node->as.call.trailing || node->as.call.contexts.count > 0) {
        jog_lower_fail(L, node->pos, "bytes() does not take arguments");
        return true;
    }
    *out = emit_zero_value(L, node->type ? node->type : type, node->pos);
    return true;
}

JirOperand jog_lower_pack_slice(Lower *L, JogType *slice_ty, const JirOperand *elems, size_t count, JogPos pos) {
    JogType *elem;
    JogType *array_ty;
    JogType *ptr_ty;
    JogType *usize;
    JirOperand first;
    JirOperand header;
    uint32_t slot;
    size_t i;
    if (!L || L->failed) {
        return jir_none();
    }
    slice_ty = jog_lower_type(L, slice_ty);
    if (!slice_ty || slice_ty->kind != JOG_TY_SLICE || jog_lower_type_is_open(slice_ty)) {
        jog_lower_fail(L, pos, "pack is not a slice");
        return jir_none();
    }
    if (count == 0) {
        return emit_zero_value(L, slice_ty, pos);
    }
    if (!elems || !L->func) {
        jog_lower_fail(L, pos, "pack argument does not outlive initialization");
        return jir_none();
    }
    elem = slice_ty->as.elem.elem;
    if (!elem || jog_lower_type_is_open(elem)) {
        jog_lower_fail(L, pos, "generic type was not instantiated");
        return jir_none();
    }
    usize = builtin(L, JOG_BUILTIN_USIZE);
    array_ty = jog_type_array(L->program->types, elem, (uint64_t)count);
    ptr_ty = jog_type_ptr(L->program->types, elem, false);
    if (!usize || !array_ty || !ptr_ty || !array_ty->has_layout) {
        jog_lower_fail(L, pos, "pack element has no size");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, array_ty, &slot)) {
        return jir_none();
    }
    jog_lower_zero(L, slot, pos);
    first = emit_bitcast(L, addr_local(L, slot, pos), ptr_ty, pos);
    for (i = 0; i < count && !L->failed; i++) {
        JirOperand at = emit_elem(L, first, jir_int(usize, (uint64_t)i), ptr_ty, pos);
        emit_store(L, at, elems[i], pos);
    }
    if (L->failed) {
        return jir_none();
    }
    header = first;
    if (slice_ty->is_const) {
        JogType *cptr = jog_type_ptr(L->program->types, elem, true);
        header = emit_bitcast(L, first, cptr, pos);
    }
    return emit_agg2(L, slice_ty, header, jir_int(usize, (uint64_t)count), pos);
}

bool jog_lower_memory_unchecked(const JogNode *node) {
    size_t i;
    if (!node || !node->attrs.items) {
        return false;
    }
    for (i = 0; i < node->attrs.count; i++) {
        JogNode *attr = node->attrs.items[i];
        if (attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
            strcmp(attr->as.attribute.name, "unchecked") == 0) {
            return true;
        }
    }
    return false;
}
