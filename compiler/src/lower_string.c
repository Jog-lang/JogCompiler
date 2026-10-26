#include "lower_string.h"

#include "check_string.h"

#include <string.h>

/* Each piece is evaluated once. fmt writes into the caller buffer and
   returns the cursor as a str plus an error? when a piece did not fit.
   An interpolated value measures with a null destination, allocates that
   many bytes with jogrt_fmt_alloc, then writes. */

enum { PIECE_INT = 1, PIECE_UINT = 2, PIECE_BOOL = 3, PIECE_BYTES = 4, PIECE_METHOD = 5 };

typedef struct Piece {
    JirOperand value;
    int kind;
    JogPos pos;
    JogStringFormat method;
} Piece;

typedef struct FmtTypes {
    JogType *i32;
    JogType *i64;
    JogType *u64;
    JogType *usize;
    JogType *boolean;
    JogType *ptr_u8;
    JogType *ptr_const_u8;
    JogType *ptr_i32;
} FmtTypes;

static int piece_kind(JogType *type) {
    JogType *base = jog_type_underlying(type);
    if (!base) {
        return 0;
    }
    if (base->kind == JOG_TY_BOOL) {
        return PIECE_BOOL;
    }
    if (base->kind == JOG_TY_BYTES) {
        return PIECE_BYTES;
    }
    if (base->kind == JOG_TY_UNTYPED_INT) {
        return PIECE_INT;
    }
    if (base->kind == JOG_TY_INT) {
        return base->as.integer.is_signed ? PIECE_INT : PIECE_UINT;
    }
    return 0;
}

static bool load_types(Lower *L, FmtTypes *t, JogPos pos) {
    JogType *u8;
    JogTypes *types = L->program->types;
    memset(t, 0, sizeof(*t));
    u8 = jog_type_builtin(types, JOG_BUILTIN_U8);
    t->i32 = jog_type_builtin(types, JOG_BUILTIN_I32);
    t->i64 = jog_type_builtin(types, JOG_BUILTIN_I64);
    t->u64 = jog_type_builtin(types, JOG_BUILTIN_U64);
    t->usize = jog_type_builtin(types, JOG_BUILTIN_USIZE);
    t->boolean = jog_type_builtin(types, JOG_BUILTIN_BOOL);
    if (!u8 || !t->i32 || !t->i64 || !t->u64 || !t->usize || !t->boolean) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    t->ptr_u8 = jog_type_ptr(types, u8, false);
    t->ptr_const_u8 = jog_type_ptr(types, u8, true);
    t->ptr_i32 = jog_type_ptr(types, t->i32, false);
    if (!t->ptr_u8 || !t->ptr_const_u8 || !t->ptr_i32) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    return true;
}

static int32_t find_extern(Lower *L, const char *name) {
    size_t i;
    for (i = 0; i < L->module->funcs.count; i++) {
        JirFunc *fn = jir_func(L->module, (uint32_t)i);
        if (fn && fn->name && strcmp(fn->name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t ensure_extern(Lower *L, const char *name, JogType **params, size_t n, JogType *ret, JogPos pos) {
    int32_t found = find_extern(L, name);
    JogType *fn;
    int32_t index;
    if (found >= 0) {
        return found;
    }
    fn = jog_type_fn(L->program->types, n ? params : NULL, n, ret, NULL, 0, false, false, true);
    if (!fn) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    index = jir_func_add(L->module, name, name, JIR_LINK_EXTERN, fn, NULL);
    if (index < 0) {
        jog_lower_fail(L, pos, "out of memory");
        return -1;
    }
    return index;
}

static JirOperand call_extern(Lower *L, int32_t fn, JirOperand *args, size_t n, JogPos pos) {
    JirFunc *func;
    JogType *ret;
    uint32_t dst;
    JirInst *inst;
    if (fn < 0 || L->failed) {
        return jir_none();
    }
    func = jir_func(L->module, (uint32_t)fn);
    ret = func && func->type && func->type->kind == JOG_TY_FN ? func->type->as.fn.ret : NULL;
    if (!ret || !jog_lower_new_local(L, NULL, ret, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_CALL, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jir_func_ref(L->module, (uint32_t)fn);
    if (!jir_set_args(L->module, inst, args, n)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static JirOperand convert_to(Lower *L, JirOperand value, JogType *dst_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (value.type == dst_ty) {
        return value;
    }
    if (!jog_lower_new_local(L, NULL, dst_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_CONVERT, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = value;
    inst->checked = false;
    return jog_lower_local_op(L, dst);
}

static JirOperand aggregate(Lower *L, JogType *type, JirOperand a, JirOperand b, bool two, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    JirOperand args[2];
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    args[0] = a;
    args[1] = b;
    if (!jir_set_args(L->module, inst, args, two ? 2 : 1)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static bool extract_field(Lower *L, JirOperand src, uint32_t index, JogType *field, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, field, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->a = src;
    inst->aux = index;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static int32_t scalar_fn(Lower *L, const FmtTypes *t, int kind, JogPos pos) {
    JogType *params[5];
    const char *name;
    JogType *value;
    params[0] = t->ptr_u8;
    params[1] = t->usize;
    params[2] = t->usize;
    params[4] = t->ptr_i32;
    if (kind == PIECE_BOOL) {
        name = "jogrt_fmt_bool";
        value = t->boolean;
    } else if (kind == PIECE_UINT) {
        name = "jogrt_fmt_u64";
        value = t->u64;
    } else {
        name = "jogrt_fmt_i64";
        value = t->i64;
    }
    params[3] = value;
    return ensure_extern(L, name, params, 5, t->usize, pos);
}

static int32_t bytes_fn(Lower *L, const FmtTypes *t, JogPos pos) {
    JogType *params[6];
    params[0] = t->ptr_u8;
    params[1] = t->usize;
    params[2] = t->usize;
    params[3] = t->ptr_const_u8;
    params[4] = t->usize;
    params[5] = t->ptr_i32;
    return ensure_extern(L, "jogrt_fmt_bytes", params, 6, t->usize, pos);
}

static JirOperand binary_op(Lower *L, JirBinOp op, JirOperand a, JirOperand b, JogType *ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !ty || !jog_lower_new_local(L, NULL, ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)op;
    inst->a = a;
    inst->b = b;
    return jog_lower_local_op(L, dst);
}

static JirOperand load_op(Lower *L, JirOperand ptr, JogType *ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !ty || !jog_lower_new_local(L, NULL, ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static void store_op(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
    JirInst *inst;
    if (L->failed) {
        return;
    }
    inst = jog_lower_emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return;
    }
    inst->a = ptr;
    inst->b = value;
}

static void null_check(Lower *L, JirOperand ptr, JogPos pos) {
    JirInst *check;
    if (L->failed) {
        return;
    }
    check = jog_lower_emit(L, JIR_NULL_CHECK, -1, pos);
    if (!check) {
        return;
    }
    check->a = ptr;
}

static JirOperand take_addr(Lower *L, JirOperand local, JogType *ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (local.kind != JIR_OPND_LOCAL) {
        jog_lower_fail(L, pos, "format receiver is not a local");
        return jir_none();
    }
    if (L->failed || !ptr_ty || !jog_lower_new_local(L, NULL, ptr_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = local;
    return jog_lower_local_op(L, dst);
}

static JirOperand elem_addr(Lower *L, JirOperand ptr, JirOperand index, JogType *ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !ptr_ty || !jog_lower_new_local(L, NULL, ptr_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ELEM_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    inst->b = index;
    return jog_lower_local_op(L, dst);
}

static JirOperand field_ptr(Lower *L, JirOperand ptr, JogType *struct_ty, uint32_t index, JogPos pos) {
    JogField *field;
    JogType *got;
    uint32_t dst;
    JirInst *inst;
    bool is_const;
    if (!struct_ty || struct_ty->kind != JOG_TY_STRUCT || !struct_ty->as.nominal.fields ||
        index >= struct_ty->as.nominal.field_count) {
        jog_lower_fail(L, pos, "format method is missing a field");
        return jir_none();
    }
    field = &struct_ty->as.nominal.fields[index];
    is_const = ptr.type && ptr.type->is_const;
    got = jog_type_ptr(L->program->types, field->type, is_const);
    if (L->failed || !got || !jog_lower_new_local(L, NULL, got, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_FIELD_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    inst->aux = index;
    return jog_lower_local_op(L, dst);
}

/* A value receiver is the hole, or a load through one pointer. A pointer
   receiver is the address of that value. An embed path walks from the
   outer value to the receiver. */
static JirOperand user_receiver(Lower *L, JirOperand value, const JogStringFormat *fmt, JogType *self_ty, JogPos pos) {
    JogType *outer;
    JogType *cur;
    JogType *ptr_ty;
    JirOperand base;
    JirOperand copy;
    int from_ptr;
    uint8_t i;
    from_ptr = value.type && value.type->kind == JOG_TY_PTR;
    outer = from_ptr ? value.type->as.elem.elem : value.type;
    if (!fmt || !outer || !self_ty) {
        jog_lower_fail(L, pos, "format method is missing a receiver");
        return jir_none();
    }
    if (fmt->path_len == 0 && !fmt->is_ptr) {
        if (!from_ptr) {
            return value;
        }
        null_check(L, value, pos);
        return load_op(L, value, self_ty, pos);
    }
    if (fmt->path_len == 0 && fmt->is_ptr) {
        if (from_ptr) {
            null_check(L, value, pos);
            if (self_ty->is_const || !value.type->is_const) {
                return value;
            }
            copy = jog_lower_as_local(L, load_op(L, value, outer, pos), pos);
            return take_addr(L, copy, self_ty, pos);
        }
        return take_addr(L, jog_lower_as_local(L, value, pos), self_ty, pos);
    }
    if (from_ptr) {
        null_check(L, value, pos);
        if (value.type->is_const) {
            copy = jog_lower_as_local(L, load_op(L, value, outer, pos), pos);
            ptr_ty = jog_type_ptr(L->program->types, outer, false);
            base = take_addr(L, copy, ptr_ty, pos);
        } else {
            base = value;
        }
    } else {
        ptr_ty = jog_type_ptr(L->program->types, outer, false);
        base = take_addr(L, jog_lower_as_local(L, value, pos), ptr_ty, pos);
    }
    cur = outer;
    for (i = 0; i < fmt->path_len && !L->failed; i++) {
        base = field_ptr(L, base, cur, fmt->index[i], pos);
        if (cur && cur->kind == JOG_TY_STRUCT && cur->as.nominal.fields && fmt->index[i] < cur->as.nominal.field_count) {
            cur = cur->as.nominal.fields[fmt->index[i]].type;
        }
    }
    if (L->failed) {
        return jir_none();
    }
    if (!fmt->is_ptr) {
        return load_op(L, base, self_ty, pos);
    }
    return base;
}

/* Measure passes a null empty buffer. A write passes the bytes still
   free at the cursor. The method's str length is how far the cursor
   moves, and its error sets the overflow flag. */
static JirOperand format_user(Lower *L, const FmtTypes *t, JirOperand dst, JirOperand cap, JirOperand at, JirOperand ovptr,
                              Piece piece) {
    JogNode *fn = piece.method.fn;
    JogType *fn_ty;
    JogType *self_ty;
    JogType *buf_ty;
    JogType *ret;
    JirOperand recv;
    JirOperand ptr;
    JirOperand len;
    JirOperand buf;
    JirOperand args[2];
    JirOperand result;
    JirOperand text;
    JirOperand err;
    JirOperand written;
    JirOperand has;
    JirOperand has_i;
    JirOperand old;
    JirOperand flag;
    int32_t index;
    JogPos pos = piece.pos;
    if (!fn || fn->kind != JOG_NODE_FN || !fn->type || fn->type->kind != JOG_TY_FN || fn->type->as.fn.param_count < 2 ||
        !fn->type->as.fn.params) {
        jog_lower_fail(L, pos, "missing format method");
        return jir_none();
    }
    fn_ty = fn->type;
    self_ty = fn_ty->as.fn.params[0];
    buf_ty = fn_ty->as.fn.params[1];
    ret = fn_ty->as.fn.ret;
    index = jog_lower_find_fn(L, fn);
    if (index < 0) {
        jog_lower_fail(L, pos, "missing format method");
        return jir_none();
    }
    recv = user_receiver(L, piece.value, &piece.method, self_ty, pos);
    if (L->failed) {
        return jir_none();
    }
    if (dst.kind == JIR_OPND_NULL) {
        ptr = jir_null(t->ptr_u8);
        len = jir_int(t->usize, 0);
    } else {
        ptr = elem_addr(L, dst, at, t->ptr_u8, pos);
        len = binary_op(L, JIR_SUB, cap, at, t->usize, pos);
    }
    buf = aggregate(L, buf_ty, ptr, len, true, pos);
    if (L->failed) {
        return jir_none();
    }
    args[0] = recv;
    args[1] = buf;
    result = call_extern(L, index, args, 2, pos);
    if (L->failed || !ret || ret->kind != JOG_TY_TUPLE || ret->as.tuple.count < 2 || !ret->as.tuple.elems) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "format method result has no type");
        }
        return jir_none();
    }
    if (!extract_field(L, result, 0, ret->as.tuple.elems[0], pos, &text) ||
        !extract_field(L, result, 1, ret->as.tuple.elems[1], pos, &err) ||
        !extract_field(L, text, 1, t->usize, pos, &written) || !extract_field(L, err, 0, t->boolean, pos, &has)) {
        return jir_none();
    }
    at = binary_op(L, JIR_ADD, at, written, t->usize, pos);
    has_i = convert_to(L, has, t->i32, pos);
    old = load_op(L, ovptr, t->i32, pos);
    flag = binary_op(L, JIR_OR, old, has_i, t->i32, pos);
    store_op(L, ovptr, flag, pos);
    return at;
}

static JirOperand format_piece(Lower *L, const FmtTypes *t, JirOperand dst, JirOperand cap, JirOperand at, JirOperand ovptr,
                               Piece piece) {
    JirOperand args[6];
    JirOperand value;
    int32_t fn;
    if (piece.kind == PIECE_METHOD) {
        return format_user(L, t, dst, cap, at, ovptr, piece);
    }
    if (piece.kind == PIECE_BYTES) {
        JogType *base = jog_type_underlying(piece.value.type);
        JogType *ptr_ty = base && base->is_const ? t->ptr_const_u8 : t->ptr_u8;
        JirOperand ptr;
        JirOperand len;
        if (!extract_field(L, piece.value, 0, ptr_ty, piece.pos, &ptr) ||
            !extract_field(L, piece.value, 1, t->usize, piece.pos, &len)) {
            return jir_none();
        }
        fn = bytes_fn(L, t, piece.pos);
        args[0] = dst;
        args[1] = cap;
        args[2] = at;
        args[3] = ptr;
        args[4] = len;
        args[5] = ovptr;
        return call_extern(L, fn, args, 6, piece.pos);
    }
    if (piece.kind == PIECE_BOOL) {
        value = piece.value;
    } else if (piece.kind == PIECE_UINT) {
        value = convert_to(L, piece.value, t->u64, piece.pos);
    } else {
        value = convert_to(L, piece.value, t->i64, piece.pos);
    }
    if (L->failed) {
        return jir_none();
    }
    fn = scalar_fn(L, t, piece.kind, piece.pos);
    args[0] = dst;
    args[1] = cap;
    args[2] = at;
    args[3] = value;
    args[4] = ovptr;
    return call_extern(L, fn, args, 5, piece.pos);
}

static JirOperand format_all(Lower *L, const FmtTypes *t, JirOperand dst, JirOperand cap, JirOperand at, JirOperand ovptr,
                             const Piece *pieces, size_t count) {
    size_t i;
    for (i = 0; i < count && !L->failed; i++) {
        at = format_piece(L, t, dst, cap, at, ovptr, pieces[i]);
    }
    return at;
}

static bool collect_one(Lower *L, JogNode *node, Piece *piece) {
    JirOperand value;
    int kind;
    JogStringFormat fmt;
    const JogPackage *pkg;
    const char *file;
    int status;
    if (!node) {
        jog_lower_fail(L, (JogPos){0}, "string piece is missing");
        return false;
    }
    memset(piece, 0, sizeof(*piece));
    value = jog_lower_as_local(L, jog_lower_expr(L, node), node->pos);
    if (L->failed || L->terminated) {
        return false;
    }
    kind = piece_kind(value.type ? value.type : node->type);
    if (kind == 0) {
        pkg = L->ast_fn && L->ast_fn->symbol ? L->ast_fn->symbol->package : NULL;
        file = L->ast_fn ? L->ast_fn->pos.file : NULL;
        status = jog_string_format_method(NULL, NULL, L->program, value.type ? value.type : node->type, pkg, file, &fmt);
        if (status == 1) {
            kind = PIECE_METHOD;
            piece->method = fmt;
        }
    }
    if (kind == 0) {
        jog_lower_fail(L, node->pos, "type cannot be formatted");
        return false;
    }
    piece->value = value;
    piece->kind = kind;
    piece->pos = node->pos;
    return true;
}

static bool collect_template(Lower *L, JogNode *tmpl, Piece **out, size_t *out_count) {
    size_t count;
    size_t i;
    Piece *pieces;
    if (!tmpl) {
        jog_lower_fail(L, (JogPos){0}, "fmt template must be a string");
        return false;
    }
    if (tmpl->kind == JOG_NODE_INTERP) {
        count = tmpl->as.interp.parts.count;
    } else {
        count = 1;
    }
    pieces = NULL;
    if (count > 0) {
        pieces = ct_arena_alloc(L->program->arena, count * sizeof(*pieces), _Alignof(Piece));
        if (!pieces) {
            jog_lower_fail(L, tmpl->pos, "out of memory");
            return false;
        }
    }
    if (tmpl->kind == JOG_NODE_INTERP) {
        for (i = 0; i < count; i++) {
            JogNode *part = tmpl->as.interp.parts.items ? tmpl->as.interp.parts.items[i] : NULL;
            if (!collect_one(L, part, &pieces[i])) {
                return false;
            }
        }
    } else if (!collect_one(L, tmpl, &pieces[0])) {
        return false;
    }
    *out = pieces;
    *out_count = count;
    return true;
}

static bool overflow_ptr(Lower *L, const FmtTypes *t, JogPos pos, JirOperand *ov, JirOperand *ovptr) {
    uint32_t flag;
    uint32_t ptr;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, t->i32, &flag) || !jog_lower_new_local(L, NULL, t->ptr_i32, &ptr)) {
        return false;
    }
    jog_lower_zero(L, flag, pos);
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)ptr, pos);
    if (!inst) {
        return false;
    }
    inst->a = jog_lower_local_op(L, flag);
    *ov = jog_lower_local_op(L, flag);
    *ovptr = jog_lower_local_op(L, ptr);
    return !L->failed;
}

static JirOperand error_value(Lower *L, const FmtTypes *t, JogType *iface, JogPos pos) {
    int32_t fn;
    JirOperand ptr;
    fn = ensure_extern(L, "jogrt_fmt_error", NULL, 0, t->ptr_u8, pos);
    ptr = call_extern(L, fn, NULL, 0, pos);
    if (L->failed) {
        return jir_none();
    }
    return aggregate(L, iface, ptr, ptr, true, pos);
}

static JirOperand overflow_flag(Lower *L, const FmtTypes *t, JirOperand ov, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, t->boolean, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->aux = (uint32_t)JIR_NE;
    inst->a = ov;
    inst->b = jir_int(t->i32, 0);
    return jog_lower_local_op(L, dst);
}

static JirOperand finish_fmt(Lower *L, const FmtTypes *t, JogType *result, JirOperand ptr, JirOperand at, JirOperand ov,
                             JogPos pos) {
    JogType *str_ty;
    JogType *err_ty;
    JogType *iface;
    JirOperand text;
    JirOperand has;
    JirOperand err_iface;
    JirOperand err_opt;
    if (!result || result->kind != JOG_TY_TUPLE || result->as.tuple.count < 2 || !result->as.tuple.elems) {
        jog_lower_fail(L, pos, "fmt result has no type");
        return jir_none();
    }
    str_ty = result->as.tuple.elems[0];
    err_ty = result->as.tuple.elems[1];
    iface = err_ty && err_ty->kind == JOG_TY_OPTION ? err_ty->as.elem.elem : NULL;
    text = aggregate(L, str_ty, ptr, at, true, pos);
    has = overflow_flag(L, t, ov, pos);
    err_iface = error_value(L, t, iface, pos);
    if (L->failed) {
        return jir_none();
    }
    err_opt = aggregate(L, err_ty, has, err_iface, true, pos);
    if (L->failed) {
        return jir_none();
    }
    return aggregate(L, result, text, err_opt, true, pos);
}

static bool is_fmt_call(const JogNode *node) {
    const JogNode *callee;
    if (!node || node->kind != JOG_NODE_CALL) {
        return false;
    }
    callee = node->as.call.callee;
    return callee && callee->kind == JOG_NODE_IDENT && !callee->symbol && callee->as.ident.name &&
           callee->as.ident.type_args.count == 0 && strcmp(callee->as.ident.name, "fmt") == 0;
}

int jog_lower_string_call(Lower *L, JogNode *node, JirOperand *out) {
    FmtTypes t;
    Piece *pieces = NULL;
    size_t count = 0;
    JogNode *buf_arg;
    JogNode *tmpl_arg;
    JogNode *buf;
    JogNode *tmpl;
    JirOperand buffer;
    JirOperand ptr;
    JirOperand cap;
    JirOperand at;
    JirOperand ov;
    JirOperand ovptr;
    JogPos pos;
    if (!is_fmt_call(node)) {
        return 0;
    }
    pos = node->pos;
    if (!out) {
        jog_lower_fail(L, pos, "fmt result is missing");
        return 1;
    }
    *out = jir_none();
    if (node->as.call.args.count != 2 || !node->as.call.args.items) {
        jog_lower_fail(L, pos, "fmt expects a buffer and a template");
        return 1;
    }
    buf_arg = node->as.call.args.items[0];
    tmpl_arg = node->as.call.args.items[1];
    buf = buf_arg && buf_arg->kind == JOG_NODE_ARG ? buf_arg->as.arg.value : NULL;
    tmpl = tmpl_arg && tmpl_arg->kind == JOG_NODE_ARG ? tmpl_arg->as.arg.value : NULL;
    if (!load_types(L, &t, pos) || !collect_template(L, tmpl, &pieces, &count)) {
        return 1;
    }
    buffer = jog_lower_as_local(L, jog_lower_expr(L, buf), buf ? buf->pos : pos);
    if (L->failed || !extract_field(L, buffer, 0, t.ptr_u8, pos, &ptr) || !extract_field(L, buffer, 1, t.usize, pos, &cap)) {
        return 1;
    }
    if (!overflow_ptr(L, &t, pos, &ov, &ovptr)) {
        return 1;
    }
    at = format_all(L, &t, ptr, cap, jir_int(t.usize, 0), ovptr, pieces, count);
    if (L->failed) {
        return 1;
    }
    *out = finish_fmt(L, &t, node->type, ptr, at, ov, pos);
    return 1;
}

JirOperand jog_lower_string_interp(Lower *L, JogNode *node) {
    FmtTypes t;
    Piece *pieces = NULL;
    size_t count = 0;
    JirOperand ov;
    JirOperand ovptr;
    JirOperand measured;
    JirOperand ptr;
    JirOperand at;
    int32_t alloc_fn;
    JogPos pos;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    pos = node->pos;
    if (!load_types(L, &t, pos) || !collect_template(L, node, &pieces, &count) || !overflow_ptr(L, &t, pos, &ov, &ovptr)) {
        return jir_none();
    }
    measured = format_all(L, &t, jir_null(t.ptr_u8), jir_int(t.usize, 0), jir_int(t.usize, 0), ovptr, pieces, count);
    if (L->failed) {
        return jir_none();
    }
    alloc_fn = ensure_extern(L, "jogrt_fmt_alloc", &t.usize, 1, t.ptr_u8, pos);
    ptr = call_extern(L, alloc_fn, &measured, 1, pos);
    if (L->failed) {
        return jir_none();
    }
    {
        JirInst *check = jog_lower_emit(L, JIR_NULL_CHECK, -1, pos);
        if (!check) {
            return jir_none();
        }
        check->a = ptr;
    }
    at = format_all(L, &t, ptr, measured, jir_int(t.usize, 0), ovptr, pieces, count);
    if (L->failed) {
        return jir_none();
    }
    return aggregate(L, node->type ? node->type : jog_type_builtin(L->program->types, JOG_BUILTIN_CONST_BYTES), ptr, at, true,
                     pos);
}
