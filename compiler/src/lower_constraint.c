#include "lower_constraint.h"

#include "check_generic.h"
#include "check_struct.h"
#include "lower_generic.h"

#include <string.h>

/* The template AST still names the type parameter. After substitution the
   concrete type is a nominal, and the call or operator becomes the method
   that type declared in its own package. Derived < <= > >= call op<=> and
   compare the tag with zero, which is how Ordering is laid out. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static JogType *peel_ptr(JogType *type, int *was_ptr) {
    if (was_ptr) {
        *was_ptr = 0;
    }
    if (type && type->kind == JOG_TY_PTR && type->as.elem.elem) {
        if (was_ptr) {
            *was_ptr = 1;
        }
        return type->as.elem.elem;
    }
    return type;
}

static int same_pkg(const JogType *type, const JogSymbol *sym) {
    const char *want = type && type->as.nominal.package ? type->as.nominal.package : "";
    const char *have = sym && sym->package && sym->package->path ? sym->package->path : "";
    return strcmp(want, have) == 0;
}

static int builtin_operand(const JogType *type) {
    JogType *base = jog_type_underlying((JogType *)type);
    if (!base) {
        return 0;
    }
    return base->kind == JOG_TY_INT || base->kind == JOG_TY_BOOL || base->kind == JOG_TY_BYTES ||
           base->kind == JOG_TY_FLOAT;
}

static const char *op_name(JogTokenKind op, int *derived) {
    if (derived) {
        *derived = 0;
    }
    switch (op) {
    case JOG_TOK_PLUS:
        return "op+";
    case JOG_TOK_MINUS:
        return "op-";
    case JOG_TOK_STAR:
        return "op*";
    case JOG_TOK_SLASH:
        return "op/";
    case JOG_TOK_PERCENT:
        return "op%";
    case JOG_TOK_AMP:
        return "op&";
    case JOG_TOK_PIPE:
        return "op|";
    case JOG_TOK_CARET:
        return "op^";
    case JOG_TOK_SHL:
        return "op<<";
    case JOG_TOK_SHR:
        return "op>>";
    case JOG_TOK_EQ:
        return "op==";
    case JOG_TOK_NEQ:
        if (derived) {
            *derived = 2;
        }
        return "op==";
    case JOG_TOK_LT:
    case JOG_TOK_LE:
    case JOG_TOK_GT:
    case JOG_TOK_GE:
        if (derived) {
            *derived = 1;
        }
        return "op<=>";
    default:
        return NULL;
    }
}

static JogNode *find_method(Lower *L, JogType *concrete, const char *name) {
    size_t p;
    if (!L || !concrete || !name || !L->program) {
        return NULL;
    }
    for (p = 0; p < L->program->package_count; p++) {
        JogPackage *pkg = L->program->packages ? L->program->packages[p] : NULL;
        size_t f;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file;
            size_t d;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count; d++) {
                JogNode *fn = list_at(file->as.file.decls, d);
                JogType *on = NULL;
                int is_ptr = 0;
                uint32_t path[JOG_STRUCT_PATH];
                uint8_t path_len = 0;
                if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.name || !fn->as.fn.receiver || !fn->symbol) {
                    continue;
                }
                if (strcmp(fn->as.fn.name, name) != 0 || !same_pkg(concrete, fn->symbol)) {
                    continue;
                }
                if (fn->symbol->type_param_count > 0) {
                    continue;
                }
                jog_method_receiver(fn, &on, &is_ptr);
                if (!on || !jog_struct_embed_path(concrete, on, path, &path_len)) {
                    continue;
                }
                (void)is_ptr;
                return fn;
            }
        }
    }
    return NULL;
}

static JirOperand emit_addr(Lower *L, JirOperand local, JogType *value_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    JogType *ptr;
    ptr = jog_type_ptr(L->program->types, value_ty, false);
    if (!ptr || !jog_lower_new_local(L, NULL, ptr, &dst)) {
        return local;
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return local;
    }
    inst->a = local;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_load(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem;
    uint32_t dst;
    JirInst *inst;
    elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem || !jog_lower_new_local(L, NULL, elem, &dst)) {
        jog_lower_fail(L, pos, "missing method receiver");
        return ptr;
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return ptr;
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_field(Lower *L, JirOperand ptr, uint32_t index, JogPos pos) {
    JogType *struct_ty;
    JogType *field_ty;
    JogType *field_ptr;
    uint32_t dst;
    JirInst *inst;
    struct_ty = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!struct_ty || struct_ty->kind != JOG_TY_STRUCT || !struct_ty->as.nominal.fields ||
        index >= struct_ty->as.nominal.field_count) {
        jog_lower_fail(L, pos, "missing method receiver");
        return ptr;
    }
    field_ty = struct_ty->as.nominal.fields[index].type;
    field_ptr = jog_type_ptr(L->program->types, field_ty, false);
    if (!field_ptr || !jog_lower_new_local(L, NULL, field_ptr, &dst)) {
        return ptr;
    }
    inst = jog_lower_emit(L, JIR_FIELD_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return ptr;
    }
    inst->a = ptr;
    inst->aux = index;
    return jog_lower_local_op(L, dst);
}

static JirOperand receiver_value(Lower *L, JogNode *base, JogNode *fn, JogPos pos) {
    JogType *on = NULL;
    int is_ptr = 0;
    JogType *outer;
    int base_ptr = 0;
    uint32_t path[JOG_STRUCT_PATH];
    uint8_t path_len = 0;
    uint8_t i;
    JirOperand value;
    JogType *concrete;
    jog_method_receiver(fn, &on, &is_ptr);
    concrete = base && base->type ? jog_lower_type(L, peel_ptr(base->type, &base_ptr)) : NULL;
    if (!concrete || !on || !jog_struct_embed_path(concrete, on, path, &path_len)) {
        jog_lower_fail(L, pos, "missing method '%s'", fn && fn->as.fn.name ? fn->as.fn.name : "?");
        return jir_none();
    }
    value = jog_lower_expr(L, base);
    if (L->failed) {
        return value;
    }
    if (!base_ptr) {
        value = jog_lower_as_local(L, value, pos);
        value = emit_addr(L, value, concrete, pos);
    }
    outer = concrete;
    for (i = 0; i < path_len && !L->failed; i++) {
        value = emit_field(L, value, path[i], pos);
        if (outer && outer->kind == JOG_TY_STRUCT && outer->as.nominal.fields && path[i] < outer->as.nominal.field_count) {
            outer = outer->as.nominal.fields[path[i]].type;
        }
    }
    if (!is_ptr) {
        value = emit_load(L, value, pos);
    }
    return value;
}

static int emit_method(Lower *L, JogNode *fn, JirOperand recv, JirOperand extra, int has_extra, JogType *ret,
                       JogPos pos, JirOperand *out) {
    int32_t index;
    JirOperand callee;
    JirOperand args[2];
    size_t n;
    uint32_t dst = 0;
    int32_t dst_id = -1;
    JirInst *inst;
    index = jog_lower_find_fn(L, fn);
    if (index < 0) {
        jog_lower_fail(L, pos, "missing lowered function '%s'", fn->as.fn.name ? fn->as.fn.name : "?");
        return 0;
    }
    callee = jir_func_ref(L->module, (uint32_t)index);
    args[0] = recv;
    n = 1;
    if (has_extra) {
        args[1] = extra;
        n = 2;
    }
    if (ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR) {
        if (!jog_lower_new_local(L, NULL, ret, &dst)) {
            return 0;
        }
        dst_id = (int32_t)dst;
    }
    inst = jog_lower_emit(L, JIR_CALL, dst_id, pos);
    if (!inst) {
        return 0;
    }
    inst->a = callee;
    if (!jir_set_args(L->module, inst, args, n)) {
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    if (dst_id >= 0) {
        *out = jog_lower_local_op(L, dst);
    } else {
        *out = jir_none();
    }
    return 1;
}

static int named_slot(JogNode *fn, const char *name) {
    size_t i;
    if (!fn || !name) {
        return -1;
    }
    for (i = 0; i < fn->as.fn.params.count; i++) {
        JogNode *param = list_at(fn->as.fn.params, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int jog_lower_constraint_call(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee;
    JogNode *base;
    JogType *inner;
    JogType *concrete;
    JogNode *fn;
    JirOperand recv;
    JirOperand args_buf[8];
    JirOperand *args;
    size_t nparam;
    size_t i;
    size_t positional = 0;
    int32_t index;
    JirOperand callee_op;
    JogType *ret;
    uint32_t dst = 0;
    int32_t dst_id = -1;
    JirInst *inst;
    unsigned char filled[8];
    if (!L || !node || node->kind != JOG_NODE_CALL || !out) {
        return 0;
    }
    callee = node->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_MEMBER || !callee->as.member.base || !callee->as.member.name) {
        return 0;
    }
    base = callee->as.member.base;
    inner = base->type ? peel_ptr(base->type, NULL) : NULL;
    if (!inner || inner->kind != JOG_TY_TYPE_PARAM) {
        return 0;
    }
    concrete = jog_lower_type(L, inner);
    if (!concrete || builtin_operand(concrete)) {
        jog_lower_fail(L, node->pos, "missing method '%s'", callee->as.member.name);
        return 1;
    }
    fn = find_method(L, concrete, callee->as.member.name);
    if (!fn || !fn->type || fn->type->kind != JOG_TY_FN) {
        jog_lower_fail(L, node->pos, "missing method '%s'", callee->as.member.name);
        return 1;
    }
    nparam = fn->type->as.fn.param_count;
    if (nparam == 0 || nparam > 8) {
        jog_lower_fail(L, node->pos, "missing method '%s'", callee->as.member.name);
        return 1;
    }
    recv = receiver_value(L, base, fn, node->pos);
    if (L->failed) {
        return 1;
    }
    args = args_buf;
    memset(args, 0, sizeof(args_buf));
    memset(filled, 0, sizeof(filled));
    args[0] = recv;
    filled[0] = 1;
    for (i = 0; i < node->as.call.args.count && !L->failed; i++) {
        JogNode *arg = list_at(node->as.call.args, i);
        int slot;
        JirOperand value;
        if (!arg || arg->kind != JOG_NODE_ARG || !arg->as.arg.value) {
            jog_lower_fail(L, node->pos, "expected an argument");
            break;
        }
        value = jog_lower_expr(L, arg->as.arg.value);
        if (arg->as.arg.name) {
            int user = named_slot(fn, arg->as.arg.name);
            if (user < 0) {
                jog_lower_fail(L, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                break;
            }
            slot = user + 1;
        } else {
            slot = (int)positional + 1;
            positional++;
        }
        if (slot < 0 || (size_t)slot >= nparam) {
            jog_lower_fail(L, arg->pos, "too many arguments");
            break;
        }
        args[slot] = value;
        filled[slot] = 1;
    }
    for (i = 0; i < nparam && !L->failed; i++) {
        if (!filled[i]) {
            jog_lower_fail(L, node->pos, "missing argument");
            break;
        }
    }
    if (L->failed) {
        return 1;
    }
    index = jog_lower_find_fn(L, fn);
    if (index < 0) {
        jog_lower_fail(L, node->pos, "missing lowered function '%s'", fn->as.fn.name);
        return 1;
    }
    callee_op = jir_func_ref(L->module, (uint32_t)index);
    ret = jog_lower_type(L, node->type);
    if (ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR) {
        if (!jog_lower_new_local(L, NULL, ret, &dst)) {
            return 1;
        }
        dst_id = (int32_t)dst;
    }
    inst = jog_lower_emit(L, JIR_CALL, dst_id, node->pos);
    if (!inst) {
        return 1;
    }
    inst->a = callee_op;
    if (!jir_set_args(L->module, inst, args, nparam)) {
        jog_lower_fail(L, node->pos, "out of memory");
        return 1;
    }
    if (dst_id >= 0) {
        *out = jog_lower_local_op(L, dst);
    } else {
        *out = jir_none();
    }
    return 1;
}

static int compare_order(Lower *L, JirOperand order, JogTokenKind op, JogType *bool_ty, JogPos pos, JirOperand *out) {
    JirOperand tag;
    JirOperand zero;
    JirBinOp bin;
    uint32_t dst;
    JirInst *inst;
    JogType *tag_ty;
    if (order.type && order.type->kind == JOG_TY_ENUM) {
        tag_ty = order.type->as.nominal.base;
        if (!tag_ty || !jog_lower_new_local(L, NULL, tag_ty, &dst)) {
            return 0;
        }
        inst = jog_lower_emit(L, JIR_ENUM_TAG, (int32_t)dst, pos);
        if (!inst) {
            return 0;
        }
        inst->a = order;
        tag = jog_lower_local_op(L, dst);
    } else if (order.type && jog_type_underlying(order.type) &&
               jog_type_underlying(order.type)->kind == JOG_TY_INT) {
        tag = order;
        tag_ty = order.type;
    } else {
        jog_lower_fail(L, pos, "operator '%s' is not lowered yet", jog_token_spelling(op));
        return 0;
    }
    zero = jir_int(tag_ty, 0);
    if (op == JOG_TOK_GT) {
        bin = JIR_GT;
    } else if (op == JOG_TOK_LT) {
        bin = JIR_LT;
    } else if (op == JOG_TOK_GE) {
        bin = JIR_GE;
    } else {
        bin = JIR_LE;
    }
    if (!jog_lower_new_local(L, NULL, bool_ty, &dst)) {
        return 0;
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return 0;
    }
    inst->aux = (uint32_t)bin;
    inst->a = tag;
    inst->b = zero;
    *out = jog_lower_local_op(L, dst);
    return 1;
}

int jog_lower_constraint_binop(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out) {
    JogType *orig;
    JogType *concrete;
    const char *name;
    int derived = 0;
    JogNode *fn;
    JogType *ret;
    JirOperand called;
    if (!L || !node || node->kind != JOG_NODE_BINARY || !out) {
        return 0;
    }
    orig = node->as.binary.lhs ? node->as.binary.lhs->type : NULL;
    if (!orig || (orig->kind != JOG_TY_TYPE_PARAM && !jog_generic_has_param(orig))) {
        return 0;
    }
    concrete = jog_lower_type(L, orig);
    if (!concrete) {
        jog_lower_fail(L, node->pos, "generic type was not instantiated");
        return 1;
    }
    if (builtin_operand(concrete)) {
        return 0;
    }
    name = op_name(node->as.binary.op, &derived);
    if (!name) {
        return 0;
    }
    fn = find_method(L, concrete, name);
    if (!fn || !fn->type || fn->type->kind != JOG_TY_FN) {
        jog_lower_fail(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.binary.op));
        return 1;
    }
    ret = fn->type->as.fn.ret;
    if (!emit_method(L, fn, lhs, rhs, 1, ret, node->pos, &called)) {
        return 1;
    }
    if (!derived) {
        *out = called;
        return 1;
    }
    if (derived == 2) {
        uint32_t dst;
        JirInst *inst;
        JogType *bool_ty = jog_lower_type(L, node->type);
        if (!jog_lower_new_local(L, NULL, bool_ty, &dst)) {
            return 1;
        }
        inst = jog_lower_emit(L, JIR_UNARY, (int32_t)dst, node->pos);
        if (!inst) {
            return 1;
        }
        inst->aux = (uint32_t)JIR_NOT;
        inst->a = called;
        *out = jog_lower_local_op(L, dst);
        return 1;
    }
    if (!compare_order(L, called, node->as.binary.op, jog_lower_type(L, node->type), node->pos, out)) {
        return 1;
    }
    return 1;
}
