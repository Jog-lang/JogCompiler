#include "lower_struct.h"

#include "check_struct.h"
#include "lower_context.h"
#include "lower_generic.h"
#include "lower_memory.h"

#include <string.h>

/* Struct literals become JIR_AGGREGATE in field order. Field reads and
   writes use the address of the root local (or an existing pointer) and
   JIR_FIELD_ADDR, so a promoted field is the embedded field's address.
   A value-receiver call passes the receiver by value. A pointer-receiver
   call passes that address. */

enum { LOWER_PATH = 16 };

typedef struct AddrPath {
    JogNode *root;
    uint32_t index[LOWER_PATH];
    uint8_t len;
    bool ok;
} AddrPath;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
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

static bool append_index(AddrPath *addr, const uint32_t *index, uint8_t n) {
    uint8_t i;
    if ((size_t)addr->len + n > LOWER_PATH) {
        return false;
    }
    for (i = 0; i < n; i++) {
        addr->index[addr->len + i] = index[i];
    }
    addr->len = (uint8_t)(addr->len + n);
    return true;
}

static void collect_addr(Lower *L, JogNode *node, AddrPath *addr) {
    JogStructPath hit;
    JogType *outer;
    const char *file;
    JogPackage *pkg;
    if (!addr->ok || !node) {
        addr->ok = false;
        return;
    }
    if (node->kind == JOG_NODE_MEMBER && node->symbol && node->symbol->kind == JOG_SYM_FIELD && node->as.member.base) {
        collect_addr(L, node->as.member.base, addr);
        if (!addr->ok) {
            return;
        }
        outer = peel_ptr(node->as.member.base->type, NULL);
        if (outer) {
            outer = jog_lower_type(L, outer);
        }
        file = L->ast_fn ? L->ast_fn->pos.file : NULL;
        pkg = L->ast_fn && L->ast_fn->symbol ? L->ast_fn->symbol->package : NULL;
        jog_struct_lookup_field(outer, node->as.member.name, L->ast_fn, pkg, file, &hit);
        if (!hit.found || hit.ambiguous || !append_index(addr, hit.index, hit.len)) {
            jog_lower_fail(L, node->pos, "missing field '%s'", node->as.member.name ? node->as.member.name : "?");
            addr->ok = false;
        }
        return;
    }
    addr->root = node;
}

static JirOperand emit_addr(Lower *L, JirOperand local, JogType *value_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    JogType *ptr;
    if (L->failed || local.kind != JIR_OPND_LOCAL) {
        return local;
    }
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

static JirOperand emit_field_addr(Lower *L, JirOperand ptr, JogType *struct_ty, uint32_t index, JogPos pos) {
    JogField *field;
    JogType *field_ptr;
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return ptr;
    }
    if (!struct_ty || struct_ty->kind != JOG_TY_STRUCT || index >= struct_ty->as.nominal.field_count ||
        !struct_ty->as.nominal.fields) {
        jog_lower_fail(L, pos, "missing field");
        return ptr;
    }
    field = &struct_ty->as.nominal.fields[index];
    field_ptr = jog_type_ptr(L->program->types, field->type, false);
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

static JirOperand emit_load(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem;
    uint32_t dst;
    JirInst *inst;
    if (L->failed) {
        return ptr;
    }
    elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem || !jog_lower_new_local(L, NULL, elem, &dst)) {
        jog_lower_fail(L, pos, "missing field");
        return ptr;
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return ptr;
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static JirOperand along_path(Lower *L, JirOperand ptr, JogType *start, const uint32_t *index, uint8_t len, JogPos pos);

/* Pointer to the value of node. A pointer-typed root is that pointer.
   A value root is the address of its local. An index or a dereference is
   the address of that element, not a copy. Each pointer field in the
   chain is loaded once before the next field, matching the checker. */
static JirOperand pointer_to(Lower *L, JogNode *node) {
    AddrPath addr;
    JirOperand op;
    JogType *root_ty;
    JogType *cur;
    int was_ptr = 0;
    int lvalue = 0;
    memset(&addr, 0, sizeof(addr));
    addr.ok = true;
    collect_addr(L, node, &addr);
    if (!addr.ok || !addr.root || L->failed) {
        return jog_lower_local_op(L, 0);
    }
    root_ty = addr.root->type ? jog_lower_type(L, addr.root->type) : NULL;
    lvalue = addr.root->kind == JOG_NODE_INDEX ||
             (addr.root->kind == JOG_NODE_UNARY && addr.root->as.unary.op == JOG_TOK_STAR);
    if (lvalue) {
        op = jog_lower_lvalue_ptr(L, addr.root);
        cur = root_ty;
    } else {
        cur = peel_ptr(root_ty, &was_ptr);
        op = jir_none();
        if (addr.root->kind == JOG_NODE_IDENT && addr.root->symbol) {
            JirOperand cap;
            if (!was_ptr && jog_lower_lambda_addr(L, addr.root->symbol, &cap)) {
                op = cap;
            } else if (was_ptr && jog_lower_lambda_name(L, addr.root, addr.root->symbol, &cap)) {
                op = cap;
            }
        }
        if (op.kind == JIR_OPND_NONE && !L->failed) {
            op = jog_lower_expr(L, addr.root);
            if (L->failed) {
                return op;
            }
            if (!was_ptr) {
                op = jog_lower_as_local(L, op, addr.root->pos);
                op = emit_addr(L, op, cur, node->pos);
            }
        }
    }
    return along_path(L, op, cur, addr.index, addr.len, node->pos);
}

JirOperand jog_lower_field_ptr(Lower *L, JogNode *node) {
    if (!L || !node) {
        return jir_none();
    }
    return pointer_to(L, node);
}

JirOperand jog_lower_field(Lower *L, JogNode *node) {
    return emit_load(L, pointer_to(L, node), node ? node->pos : (JogPos){0});
}

static void store_at(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
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

static JirOperand emit_binop(Lower *L, JogTokenKind op, JirOperand lhs, JirOperand rhs, JogType *type, JogPos pos) {
    JirBinOp bin = JIR_ADD;
    bool checked = false;
    uint32_t dst;
    JirInst *inst;
    switch (op) {
    case JOG_TOK_PLUS:
        bin = JIR_ADD;
        checked = true;
        break;
    case JOG_TOK_MINUS:
        bin = JIR_SUB;
        checked = true;
        break;
    case JOG_TOK_STAR:
        bin = JIR_MUL;
        checked = true;
        break;
    case JOG_TOK_SLASH:
        bin = JIR_DIV;
        checked = true;
        break;
    case JOG_TOK_PERCENT:
        bin = JIR_REM;
        checked = true;
        break;
    case JOG_TOK_AMP:
        bin = JIR_AND;
        break;
    case JOG_TOK_PIPE:
        bin = JIR_OR;
        break;
    case JOG_TOK_CARET:
        bin = JIR_XOR;
        break;
    case JOG_TOK_SHL:
        bin = JIR_SHL;
        checked = true;
        break;
    case JOG_TOK_SHR:
        bin = JIR_SHR;
        checked = true;
        break;
    default:
        jog_lower_fail(L, pos, "operator '%s' is not lowered yet", jog_token_spelling(op));
        return lhs;
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return lhs;
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return lhs;
    }
    inst->aux = (uint32_t)bin;
    inst->checked = checked;
    inst->a = lhs;
    inst->b = rhs;
    return jog_lower_local_op(L, dst);
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

void jog_lower_field_assign(Lower *L, JogNode *node) {
    JogNode *target;
    JirOperand ptr;
    JirOperand value;
    JogTokenKind base;
    JogPos pos;
    if (!node || L->failed) {
        return;
    }
    target = node->as.assign.target;
    pos = node->pos;
    ptr = pointer_to(L, target);
    if (L->failed) {
        return;
    }
    base = compound_base(node->as.assign.op);
    if (base != JOG_TOK_EOF) {
        if (target && (base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && target->type && target->type->kind == JOG_TY_PTR) {
            jog_lower_memory_ptr_update(L, ptr, base, node->as.assign.value, target->type, pos);
            return;
        }
        JirOperand old = emit_load(L, ptr, pos);
        JirOperand rhs = jog_lower_expr(L, node->as.assign.value);
        if (L->failed || L->terminated) {
            return;
        }
        value = emit_binop(L, base, old, rhs, target->type ? target->type : rhs.type, pos);
    } else if (node->as.assign.op == JOG_TOK_ASSIGN) {
        value = jog_lower_expr(L, node->as.assign.value);
    } else {
        jog_lower_fail(L, pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
        return;
    }
    if (!L->terminated) {
        store_at(L, ptr, value, pos);
    }
}

static JirOperand zero_value(Lower *L, JogType *type, JogPos pos) {
    uint32_t dst;
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jog_lower_local_op(L, 0);
    }
    jog_lower_zero(L, dst, pos);
    return jog_lower_local_op(L, dst);
}

static int field_slot(JogType *type, const char *name) {
    size_t i;
    if (!type || !name || type->kind != JOG_TY_STRUCT) {
        return -1;
    }
    for (i = 0; i < type->as.nominal.field_count; i++) {
        JogField *field = type->as.nominal.fields ? &type->as.nominal.fields[i] : NULL;
        if (field && field->name && strcmp(field->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool lower_struct_lit(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee = node->as.call.callee;
    JogType *type;
    JirOperand *args;
    size_t n;
    size_t i;
    size_t positional = 0;
    uint32_t dst;
    JirInst *inst;
    unsigned char *filled;
    *out = jog_lower_local_op(L, 0);
    if (!callee || !callee->symbol || callee->symbol->kind != JOG_SYM_TYPE || !callee->type || callee->type->kind != JOG_TY_STRUCT) {
        return false;
    }
    type = callee->type;
    n = type->as.nominal.field_count;
    args = NULL;
    filled = NULL;
    if (n > 0) {
        args = ct_arena_alloc(L->program->arena, n * sizeof(*args), _Alignof(JirOperand));
        filled = ct_arena_alloc(L->program->arena, n, 1);
        if (!args || !filled) {
            jog_lower_fail(L, node->pos, "out of memory");
            return true;
        }
        memset(args, 0, n * sizeof(*args));
        memset(filled, 0, n);
    }
    for (i = 0; i < node->as.call.args.count && !L->failed; i++) {
        JogNode *arg = list_at(node->as.call.args, i);
        int slot;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_lower_fail(L, node->pos, "expected an argument");
            break;
        }
        if (arg->as.arg.name) {
            slot = field_slot(type, arg->as.arg.name);
        } else {
            slot = (int)positional;
            positional++;
        }
        if (slot < 0 || (size_t)slot >= n || filled[slot]) {
            jog_lower_fail(L, arg->pos, "missing field");
            break;
        }
        filled[slot] = 1;
        args[slot] = jog_lower_expr(L, arg->as.arg.value);
    }
    for (i = 0; i < n && !L->failed; i++) {
        JogField *field = &type->as.nominal.fields[i];
        JogNode *decl;
        if (filled[i]) {
            continue;
        }
        decl = field->decl;
        if (decl && decl->kind == JOG_NODE_FIELD && decl->as.field.default_value) {
            args[i] = jog_lower_expr(L, decl->as.field.default_value);
            continue;
        }
        args[i] = zero_value(L, field->type, node->pos);
    }
    if (L->failed || !jog_lower_new_local(L, NULL, type, &dst)) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, node->pos);
    if (!inst) {
        return true;
    }
    if (n > 0 && !jir_set_args(L->module, inst, args, n)) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    *out = jog_lower_local_op(L, dst);
    return true;
}

/* Last value parameter when it is a pack. Methods prepend the receiver. */
static int method_pack_slot(JogNode *fn, size_t nparam) {
    JogNode *param;
    size_t ast_n;
    if (!fn || fn->kind != JOG_NODE_FN || nparam == 0) {
        return -1;
    }
    ast_n = fn->as.fn.params.count;
    if (ast_n == 0 || nparam != ast_n + 1) {
        return -1;
    }
    param = list_at(fn->as.fn.params, ast_n - 1);
    if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.pack) {
        return -1;
    }
    return (int)nparam - 1;
}

static int named_param(JogNode *fn, const char *name) {
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

static bool implicit_method(const JogNode *callee) {
    JogNode *base;
    if (!callee || callee->kind != JOG_NODE_MEMBER) {
        return false;
    }
    base = callee->as.member.base;
    if (base && base->symbol && base->symbol->kind == JOG_SYM_TYPE) {
        return false;
    }
    return true;
}

static JirOperand along_path(Lower *L, JirOperand ptr, JogType *start, const uint32_t *index, uint8_t len, JogPos pos) {
    JogType *cur = start;
    uint8_t i;
    for (i = 0; i < len && !L->failed; i++) {
        int was = 0;
        JogType *peeled = peel_ptr(cur, &was);
        if (was) {
            ptr = emit_load(L, ptr, pos);
            cur = peeled;
        }
        ptr = emit_field_addr(L, ptr, cur, index[i], pos);
        if (cur && cur->kind == JOG_TY_STRUCT && cur->as.nominal.fields && index[i] < cur->as.nominal.field_count) {
            cur = cur->as.nominal.fields[index[i]].type;
        } else {
            cur = NULL;
        }
    }
    return ptr;
}

static bool lower_method_call(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *callee = node->as.call.callee;
    JogNode *fn;
    JogType *fn_ty;
    JogType *ret;
    JirOperand *args;
    JirOperand callee_op;
    JirInst *inst;
    size_t nparam;
    size_t i;
    size_t positional = 0;
    uint32_t dst = 0;
    int32_t index;
    int implicit;
    int32_t dst_id = -1;
    unsigned char *filled;
    *out = jog_lower_local_op(L, 0);
    if (!callee || !callee->symbol || callee->symbol->kind != JOG_SYM_METHOD) {
        return false;
    }
    fn = callee->symbol->decl;
    fn_ty = callee->type && callee->type->kind == JOG_TY_FN ? callee->type : (fn ? fn->type : NULL);
    if (!fn || fn->kind != JOG_NODE_FN || !fn_ty || fn_ty->kind != JOG_TY_FN || fn_ty->as.fn.param_count == 0) {
        jog_lower_fail(L, node->pos, "missing method");
        return true;
    }
    if (node->solved_type_arg_count > 0) {
        index = jog_lower_generic_callee(L, node);
    } else {
        index = jog_lower_find_fn(L, fn);
    }
    if (index < 0) {
        jog_lower_fail(L, node->pos, "missing lowered function '%s'", fn->as.fn.name ? fn->as.fn.name : "?");
        return true;
    }
    callee_op = jir_func_ref(L->module, (uint32_t)index);
    nparam = fn_ty->as.fn.param_count;
    implicit = implicit_method(callee);
    args = ct_arena_alloc(L->program->arena, nparam * sizeof(*args), _Alignof(JirOperand));
    filled = ct_arena_alloc(L->program->arena, nparam, 1);
    if (!args || !filled) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    memset(args, 0, nparam * sizeof(*args));
    memset(filled, 0, nparam);
    if (implicit) {
        JogNode *base = callee->as.member.base;
        JogType *on = NULL;
        int is_ptr = 0;
        JogType *outer;
        uint32_t path[JOG_STRUCT_PATH];
        uint8_t path_len = 0;
        JirOperand recv;
        jog_method_receiver(fn, &on, &is_ptr);
        outer = base ? peel_ptr(base->type, NULL) : NULL;
        if (!on || !jog_struct_embed_path(outer, on, path, &path_len)) {
            jog_lower_fail(L, node->pos, "missing method '%s'", fn->as.fn.name ? fn->as.fn.name : "?");
            return true;
        }
        recv = pointer_to(L, base);
        recv = along_path(L, recv, outer, path, path_len, node->pos);
        if (!is_ptr) {
            recv = emit_load(L, recv, node->pos);
        }
        args[0] = recv;
        filled[0] = 1;
    }
    {
        int pack = method_pack_slot(fn, nparam);
        CtDa pack_elems;
        if (!ct_da_init(&pack_elems, sizeof(JirOperand))) {
            jog_lower_fail(L, node->pos, "out of memory");
            return true;
        }
        for (i = 0; i < node->as.call.args.count && !L->failed; i++) {
            JogNode *arg = list_at(node->as.call.args, i);
            int slot = -1;
            JirOperand value;
            if (!arg || arg->kind != JOG_NODE_ARG) {
                jog_lower_fail(L, node->pos, "expected an argument");
                break;
            }
            value = jog_lower_expr(L, arg->as.arg.value);
            if (arg->as.arg.name) {
                int user = named_param(fn, arg->as.arg.name);
                if (user < 0) {
                    jog_lower_fail(L, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                    break;
                }
                slot = user + 1;
                if (pack >= 0 && slot == pack && pack_elems.count > 0) {
                    jog_lower_fail(L, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                    break;
                }
            } else {
                size_t slot_index = implicit ? positional + 1 : positional;
                if (pack >= 0 && slot_index >= (size_t)pack) {
                    if (filled[pack]) {
                        jog_lower_fail(L, arg->pos, "duplicate argument");
                        break;
                    }
                    if (!ct_da_push(&pack_elems, &value)) {
                        jog_lower_fail(L, arg->pos, "out of memory");
                        break;
                    }
                    positional++;
                    continue;
                }
                slot = (int)slot_index;
                positional++;
            }
            if (slot < 0 || (size_t)slot >= nparam) {
                jog_lower_fail(L, arg->pos, "too many arguments");
                break;
            }
            args[slot] = value;
            filled[slot] = 1;
        }
        if (!L->failed && pack >= 0 && !filled[pack]) {
            const JirOperand *els = pack_elems.count > 0 ? (const JirOperand *)pack_elems.data : NULL;
            JogType *slice = fn_ty->as.fn.params ? fn_ty->as.fn.params[pack] : NULL;
            args[pack] = jog_lower_pack_slice(L, slice, els, pack_elems.count, node->pos);
            filled[pack] = !L->failed;
        }
        ct_da_free(&pack_elems);
    }
    for (i = 0; i < nparam && !L->failed; i++) {
        JogNode *param;
        if (filled[i]) {
            continue;
        }
        param = i == 0 ? NULL : list_at(fn->as.fn.params, i - 1);
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.default_value) {
            jog_lower_fail(L, node->pos, "missing argument");
            break;
        }
        args[i] = jog_lower_expr(L, param->as.param.default_value);
        filled[i] = 1;
    }
    if (L->failed) {
        return true;
    }
    if (node->context_arg_count > 0 || fn_ty->as.fn.context_count > 0) {
        size_t ctx_n = node->context_arg_count;
        size_t total = nparam + ctx_n;
        JirOperand *all = ct_arena_alloc(L->program->arena, total > 0 ? total * sizeof(*all) : sizeof(*all), _Alignof(JirOperand));
        if (!all) {
            jog_lower_fail(L, node->pos, "out of memory");
            return true;
        }
        memset(all, 0, (total > 0 ? total : 1) * sizeof(*all));
        if (nparam > 0) {
            memcpy(all, args, nparam * sizeof(*all));
        }
        if (!jog_lower_context_args(L, node, all, nparam)) {
            return true;
        }
        args = all;
        nparam = total;
    }
    ret = fn_ty->as.fn.ret;
    if (ret && ret->kind != JOG_TY_UNIT && ret->kind != JOG_TY_ERROR) {
        if (!jog_lower_new_local(L, NULL, ret, &dst)) {
            return true;
        }
        dst_id = (int32_t)dst;
    }
    inst = jog_lower_emit(L, JIR_CALL, dst_id, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = callee_op;
    if (!jir_set_args(L->module, inst, args, nparam)) {
        jog_lower_fail(L, node->pos, "out of memory");
        return true;
    }
    if (dst_id >= 0) {
        *out = jog_lower_local_op(L, dst);
    }
    return true;
}

bool jog_lower_struct_call(Lower *L, JogNode *node, JirOperand *out) {
    if (!node || node->kind != JOG_NODE_CALL || !out) {
        return false;
    }
    if (lower_struct_lit(L, node, out)) {
        return true;
    }
    return lower_method_call(L, node, out);
}

static void walk_self(JogNode *node, JogNode *fn, JogSymbol **out);

static void walk_list(JogList list, JogNode *fn, JogSymbol **out) {
    size_t i;
    for (i = 0; i < list.count && !*out; i++) {
        JogNode *item = list_at(list, i);
        if (item && item->kind == JOG_NODE_ARG) {
            walk_self(item->as.arg.value, fn, out);
        } else {
            walk_self(item, fn, out);
        }
    }
}

static void walk_self(JogNode *node, JogNode *fn, JogSymbol **out) {
    if (!node || *out) {
        return;
    }
    if (node->kind == JOG_NODE_FN && node != fn) {
        return;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol && node->symbol->decl == fn) {
        *out = node->symbol;
        return;
    }
    switch (node->kind) {
    case JOG_NODE_MEMBER:
        walk_self(node->as.member.base, fn, out);
        return;
    case JOG_NODE_CALL:
        walk_self(node->as.call.callee, fn, out);
        walk_list(node->as.call.args, fn, out);
        walk_self(node->as.call.trailing, fn, out);
        return;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        walk_self(node->as.unary.operand, fn, out);
        return;
    case JOG_NODE_BINARY:
        walk_self(node->as.binary.lhs, fn, out);
        walk_self(node->as.binary.rhs, fn, out);
        return;
    case JOG_NODE_CAST:
        walk_self(node->as.cast.expr, fn, out);
        return;
    case JOG_NODE_IF:
        walk_self(node->as.if_.cond, fn, out);
        walk_self(node->as.if_.then_block, fn, out);
        walk_self(node->as.if_.else_branch, fn, out);
        return;
    case JOG_NODE_BLOCK:
        walk_list(node->as.block.stmts, fn, out);
        return;
    case JOG_NODE_EXPR_STMT:
        walk_self(node->as.expr_stmt.expr, fn, out);
        return;
    case JOG_NODE_ASSIGN:
        walk_self(node->as.assign.target, fn, out);
        walk_self(node->as.assign.value, fn, out);
        return;
    case JOG_NODE_RETURN:
        walk_list(node->as.ret.values, fn, out);
        return;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
        walk_self(node->as.binding.init, fn, out);
        return;
    case JOG_NODE_FOR:
        walk_self(node->as.for_.expr, fn, out);
        walk_self(node->as.for_.body, fn, out);
        return;
    case JOG_NODE_INTERP:
        walk_list(node->as.interp.parts, fn, out);
        return;
    case JOG_NODE_INDEX:
        walk_self(node->as.index.base, fn, out);
        walk_self(node->as.index.index, fn, out);
        return;
    case JOG_NODE_SLICE:
        walk_self(node->as.slice.base, fn, out);
        walk_self(node->as.slice.lo, fn, out);
        walk_self(node->as.slice.hi, fn, out);
        return;
    case JOG_NODE_WHEN:
        walk_self(node->as.when.subject, fn, out);
        walk_list(node->as.when.arms, fn, out);
        return;
    case JOG_NODE_WHEN_ARM:
        walk_list(node->as.when_arm.conds, fn, out);
        walk_self(node->as.when_arm.body, fn, out);
        return;
    case JOG_NODE_IS:
        walk_self(node->as.is.expr, fn, out);
        return;
    case JOG_NODE_TUPLE:
        walk_list(node->as.tuple.elems, fn, out);
        return;
    case JOG_NODE_ARRAY_LIT:
        walk_list(node->as.array_lit.elems, fn, out);
        return;
    case JOG_NODE_GO:
        walk_list(node->as.go.args, fn, out);
        walk_list(node->as.go.contexts, fn, out);
        walk_self(node->as.go.body, fn, out);
        return;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        walk_self(node->as.defer.body, fn, out);
        return;
    case JOG_NODE_LAMBDA:
        walk_self(node->as.lambda.body, fn, out);
        return;
    case JOG_NODE_DESTRUCTURE:
        walk_list(node->as.destructure.targets, fn, out);
        walk_self(node->as.destructure.value, fn, out);
        return;
    case JOG_NODE_WITH:
        walk_list(node->as.with.values, fn, out);
        walk_self(node->as.with.body, fn, out);
        return;
    case JOG_NODE_ATTR_BLOCK:
        walk_self(node->as.attr_block.body, fn, out);
        return;
    case JOG_NODE_PAT_VARIANT:
        walk_list(node->as.pat_variant.fields, fn, out);
        return;
    case JOG_NODE_PAT_FIELD:
        walk_self(node->as.pat_field.pattern, fn, out);
        return;
    case JOG_NODE_PAT_IN:
    case JOG_NODE_PAT_EXPR:
        walk_self(node->as.pat_expr.expr, fn, out);
        return;
    default:
        return;
    }
}

JogSymbol *jog_lower_self_symbol(JogNode *fn) {
    JogSymbol *found = NULL;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return NULL;
    }
    walk_self(fn->as.fn.body, fn, &found);
    return found;
}

const char *jog_lower_param_name(JogNode *fn, size_t index) {
    size_t ast;
    JogNode *param;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return NULL;
    }
    ast = index;
    if (fn->as.fn.receiver) {
        if (index == 0) {
            return "self";
        }
        ast = index - 1;
    }
    if (ast < fn->as.fn.params.count) {
        param = list_at(fn->as.fn.params, ast);
        if (param && param->kind == JOG_NODE_PARAM && !is_discard(param->as.param.name)) {
            return param->as.param.name;
        }
        return NULL;
    }
    ast -= fn->as.fn.params.count;
    if (ast < fn->as.fn.contexts.count) {
        param = list_at(fn->as.fn.contexts, ast);
        if (param && param->kind == JOG_NODE_PARAM && !is_discard(param->as.param.name)) {
            return param->as.param.name;
        }
    }
    return NULL;
}
