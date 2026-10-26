#include "lower_internal.h"

#include "lower_generic.h"
#include "lower_memory.h"

#include <stdlib.h>
#include <string.h>

/* Non-escaping lambda captures. The caller builds a stack tuple of *void,
   one slot per captured local, and the closure env points at it. The
   lambda function takes that void * after its Jog parameters, so the code
   pointer is the lambda itself. The tuple dies with the caller frame.
   Escaping copies are not lowered. go inside a capturing lambda is not
   lowered: the child must not read this env. */

typedef struct LamCap {
    JogSymbol *symbol;
    uint32_t parent_local;
    int indirect;
    uint32_t indirect_slot;
} LamCap;

typedef struct LamScan {
    Lower *L;
    LamCap *caps;
    size_t count;
    size_t cap;
    int failed;
} LamScan;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static JogType *universe_type(Lower *L, const char *name) {
    JogPackage *root;
    JogPackageScope *pkg;
    const char *interned;
    JogSymbol *symbol;
    if (!L || !L->program || !L->program->root || !L->program->intern) {
        return NULL;
    }
    root = L->program->root;
    pkg = root->scope;
    if (!pkg || !pkg->universe) {
        return NULL;
    }
    interned = ct_intern_cstr(L->program->intern, name);
    if (!interned) {
        return NULL;
    }
    symbol = jog_package_scope_lookup(pkg->universe, interned);
    return symbol ? symbol->type : NULL;
}

static int is_capture_kind(JogSymbolKind kind) {
    return kind == JOG_SYM_VARIABLE || kind == JOG_SYM_CONSTANT || kind == JOG_SYM_PARAMETER || kind == JOG_SYM_CONTEXT;
}

static int outer_slot(Lower *L, const JogSymbol *symbol, uint32_t *slot) {
    LamCap *caps;
    size_t i;
    if (!L->in_lambda || L->in_coro || !L->lambda_caps || !symbol) {
        return 0;
    }
    caps = L->lambda_caps;
    for (i = 0; i < L->lambda_cap_count; i++) {
        if (caps[i].symbol == symbol) {
            *slot = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

static void add_cap(LamScan *s, JogSymbol *symbol, JogPos pos) {
    uint32_t local = 0;
    uint32_t slot = 0;
    size_t i;
    int indirect = 0;
    LamCap *grown;
    size_t ncap;
    if (!s || s->failed || !symbol || !is_capture_kind(symbol->kind)) {
        return;
    }
    if (jog_lower_find_local(s->L, symbol, &local)) {
        indirect = 0;
    } else if (outer_slot(s->L, symbol, &slot)) {
        indirect = 1;
    } else {
        return;
    }
    for (i = 0; i < s->count; i++) {
        if (s->caps[i].symbol == symbol) {
            return;
        }
    }
    if (s->count == s->cap) {
        ncap = s->cap ? s->cap * 2 : 4;
        grown = realloc(s->caps, ncap * sizeof(*grown));
        if (!grown) {
            jog_lower_fail(s->L, pos, "out of memory");
            s->failed = 1;
            return;
        }
        s->caps = grown;
        s->cap = ncap;
    }
    s->caps[s->count].symbol = symbol;
    s->caps[s->count].parent_local = local;
    s->caps[s->count].indirect = indirect;
    s->caps[s->count].indirect_slot = slot;
    s->count++;
}

static void scan_node(LamScan *s, JogNode *node);

static void scan_list(LamScan *s, JogList list) {
    size_t i;
    for (i = 0; i < list.count && !s->failed; i++) {
        scan_node(s, list_at(list, i));
    }
}

static void scan_context_args(LamScan *s, JogNode *node) {
    size_t i;
    if (!node || !node->context_args) {
        return;
    }
    for (i = 0; i < node->context_arg_count && !s->failed; i++) {
        scan_node(s, node->context_args[i]);
    }
}

static void scan_node(LamScan *s, JogNode *node) {
    if (!node || !s || s->failed || s->L->failed) {
        return;
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        if (node->symbol) {
            add_cap(s, node->symbol, node->pos);
        }
        break;
    case JOG_NODE_BLOCK:
    case JOG_NODE_INTERP:
    case JOG_NODE_TUPLE:
        scan_list(s, node->kind == JOG_NODE_BLOCK       ? node->as.block.stmts
                      : node->kind == JOG_NODE_INTERP ? node->as.interp.parts
                                                      : node->as.tuple.elems);
        break;
    case JOG_NODE_EXPR_STMT:
        scan_node(s, node->as.expr_stmt.expr);
        break;
    case JOG_NODE_ASSIGN:
        scan_node(s, node->as.assign.target);
        scan_node(s, node->as.assign.value);
        break;
    case JOG_NODE_DESTRUCTURE:
        scan_list(s, node->as.destructure.targets);
        scan_node(s, node->as.destructure.value);
        break;
    case JOG_NODE_WITH:
        scan_list(s, node->as.with.values);
        scan_node(s, node->as.with.body);
        break;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        scan_node(s, node->as.defer.body);
        break;
    case JOG_NODE_ATTR_BLOCK:
        scan_node(s, node->as.attr_block.body);
        break;
    case JOG_NODE_RETURN:
        scan_list(s, node->as.ret.values);
        break;
    case JOG_NODE_FOR:
        scan_node(s, node->as.for_.expr);
        scan_node(s, node->as.for_.body);
        break;
    case JOG_NODE_VAL:
    case JOG_NODE_VAR:
        scan_node(s, node->as.binding.init);
        break;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        scan_node(s, node->as.unary.operand);
        break;
    case JOG_NODE_BINARY:
        scan_node(s, node->as.binary.lhs);
        scan_node(s, node->as.binary.rhs);
        break;
    case JOG_NODE_CAST:
        scan_node(s, node->as.cast.expr);
        break;
    case JOG_NODE_IS:
        scan_node(s, node->as.is.expr);
        break;
    case JOG_NODE_CALL:
        scan_node(s, node->as.call.callee);
        scan_list(s, node->as.call.args);
        scan_node(s, node->as.call.trailing);
        scan_list(s, node->as.call.contexts);
        scan_context_args(s, node);
        break;
    case JOG_NODE_ARG:
        scan_node(s, node->as.arg.value);
        break;
    case JOG_NODE_INDEX:
        scan_node(s, node->as.index.base);
        scan_node(s, node->as.index.index);
        break;
    case JOG_NODE_SLICE:
        scan_node(s, node->as.slice.base);
        scan_node(s, node->as.slice.lo);
        scan_node(s, node->as.slice.hi);
        break;
    case JOG_NODE_MEMBER:
        scan_node(s, node->as.member.base);
        break;
    case JOG_NODE_IF:
        scan_node(s, node->as.if_.cond);
        scan_node(s, node->as.if_.then_block);
        scan_node(s, node->as.if_.else_branch);
        break;
    case JOG_NODE_WHEN:
        scan_node(s, node->as.when.subject);
        scan_list(s, node->as.when.arms);
        break;
    case JOG_NODE_WHEN_ARM:
        scan_list(s, node->as.when_arm.conds);
        scan_node(s, node->as.when_arm.body);
        break;
    case JOG_NODE_LAMBDA:
        scan_node(s, node->as.lambda.body);
        break;
    case JOG_NODE_GO:
        scan_list(s, node->as.go.args);
        scan_list(s, node->as.go.contexts);
        scan_node(s, node->as.go.body);
        scan_context_args(s, node);
        break;
    case JOG_NODE_ARRAY_LIT:
        scan_list(s, node->as.array_lit.elems);
        break;
    case JOG_NODE_BUILTIN:
        scan_list(s, node->as.builtin.args);
        break;
    case JOG_NODE_PAT_EXPR:
        scan_node(s, node->as.pat_expr.expr);
        break;
    case JOG_NODE_PAT_FIELD:
        scan_node(s, node->as.pat_field.pattern);
        break;
    case JOG_NODE_PAT_VARIANT:
        scan_list(s, node->as.pat_variant.fields);
        break;
    default:
        break;
    }
}

static JirOperand bitcast_op(Lower *L, JirOperand src, JogType *dest, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !dest || !jog_lower_new_local(L, NULL, dest, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = src;
    return jog_lower_local_op(L, dst);
}

static JirOperand addr_of_local(Lower *L, uint32_t local, JogType *ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !ptr_ty || !jog_lower_new_local(L, NULL, ptr_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jog_lower_local_op(L, local);
    return jog_lower_local_op(L, dst);
}

static JirOperand field_ptr(Lower *L, JirOperand base, uint32_t index, JogType *field_ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !field_ptr_ty || !jog_lower_new_local(L, NULL, field_ptr_ty, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_FIELD_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = base;
    inst->aux = index;
    return jog_lower_local_op(L, dst);
}

static JirOperand load_ptr(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem;
    uint32_t dst;
    JirInst *inst;
    elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem || !jog_lower_new_local(L, NULL, elem, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static void store_ptr(Lower *L, JirOperand ptr, JirOperand value, JogPos pos) {
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

static JogType *tuple_of(Lower *L, JogType *void_ptr, size_t count, JogPos pos) {
    JogType **elems;
    JogType *tuple;
    size_t i;
    if (!void_ptr || count == 0) {
        return NULL;
    }
    elems = ct_arena_alloc(L->program->arena, count * sizeof(*elems), _Alignof(JogType *));
    if (!elems) {
        jog_lower_fail(L, pos, "out of memory");
        return NULL;
    }
    for (i = 0; i < count; i++) {
        elems[i] = void_ptr;
    }
    tuple = jog_type_tuple(L->program->types, elems, count);
    if (!tuple) {
        jog_lower_fail(L, pos, "out of memory");
    }
    return tuple;
}

/* *void loaded from env slot `index` of a tuple with `nfields` slots. */
static JirOperand load_env_slot(Lower *L, uint32_t env_local, size_t nfields, uint32_t index, JogType *void_ptr,
                                JogPos pos) {
    JogType *tuple;
    JogType *tuple_ptr;
    JogType *slot_ptr;
    JirOperand as_tuple;
    JirOperand slot;
    tuple = tuple_of(L, void_ptr, nfields, pos);
    tuple_ptr = tuple ? jog_type_ptr(L->program->types, tuple, false) : NULL;
    slot_ptr = void_ptr ? jog_type_ptr(L->program->types, void_ptr, false) : NULL;
    if (!tuple_ptr || !slot_ptr) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "out of memory");
        }
        return jir_none();
    }
    as_tuple = bitcast_op(L, jog_lower_local_op(L, env_local), tuple_ptr, pos);
    slot = field_ptr(L, as_tuple, index, slot_ptr, pos);
    return load_ptr(L, slot, pos);
}

static int cap_index(Lower *L, const JogSymbol *symbol, uint32_t *index) {
    return outer_slot(L, symbol, index);
}

static int is_cap(Lower *L, const JogSymbol *symbol) {
    uint32_t index = 0;
    return cap_index(L, symbol, &index);
}

static JogType *cap_type(Lower *L, const JogSymbol *symbol, JogNode *node) {
    JogType *ty = NULL;
    if (symbol && symbol->type) {
        ty = jog_lower_type(L, symbol->type);
    }
    if ((!ty || ty->kind == JOG_TY_ERROR) && node && node->type) {
        ty = jog_lower_type(L, node->type);
    }
    return ty;
}

/* Pointer to the captured local (*T). */
static JirOperand cap_addr(Lower *L, const JogSymbol *symbol, JogNode *node, JogPos pos) {
    uint32_t index = 0;
    JogType *void_ptr;
    JogType *ty;
    JogType *ptr_ty;
    JirLocal *env;
    JirOperand raw;
    if (!cap_index(L, symbol, &index)) {
        return jir_none();
    }
    env = jir_local_at(L->func, L->lambda_env);
    void_ptr = env ? env->type : NULL;
    ty = cap_type(L, symbol, node);
    ptr_ty = ty ? jog_type_ptr(L->program->types, ty, false) : NULL;
    if (!void_ptr || !ptr_ty) {
        jog_lower_fail(L, pos, "missing capture");
        return jir_none();
    }
    raw = load_env_slot(L, L->lambda_env, L->lambda_cap_count, index, void_ptr, pos);
    return bitcast_op(L, raw, ptr_ty, pos);
}

bool jog_lower_lambda_name(Lower *L, JogNode *node, JogSymbol *symbol, JirOperand *out) {
    JirOperand addr;
    JogPos pos;
    if (!out) {
        return false;
    }
    *out = jir_none();
    if (!is_cap(L, symbol)) {
        return false;
    }
    pos = node ? node->pos : (JogPos){0};
    addr = cap_addr(L, symbol, node, pos);
    if (L->failed) {
        return true;
    }
    *out = load_ptr(L, addr, pos);
    return true;
}

bool jog_lower_lambda_addr(Lower *L, const JogSymbol *symbol, JirOperand *out) {
    JirOperand addr;
    if (!out) {
        return false;
    }
    *out = jir_none();
    if (!is_cap(L, symbol)) {
        return false;
    }
    addr = cap_addr(L, symbol, NULL, (JogPos){0});
    if (L->failed) {
        return true;
    }
    *out = addr;
    return true;
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

static int map_binop(JogTokenKind op, JirBinOp *out, int *checked) {
    *checked = 0;
    switch (op) {
    case JOG_TOK_PLUS:
        *out = JIR_ADD;
        *checked = 1;
        return 1;
    case JOG_TOK_MINUS:
        *out = JIR_SUB;
        *checked = 1;
        return 1;
    case JOG_TOK_STAR:
        *out = JIR_MUL;
        *checked = 1;
        return 1;
    case JOG_TOK_SLASH:
        *out = JIR_DIV;
        *checked = 1;
        return 1;
    case JOG_TOK_PERCENT:
        *out = JIR_REM;
        *checked = 1;
        return 1;
    case JOG_TOK_AMP:
        *out = JIR_AND;
        return 1;
    case JOG_TOK_PIPE:
        *out = JIR_OR;
        return 1;
    case JOG_TOK_CARET:
        *out = JIR_XOR;
        return 1;
    case JOG_TOK_SHL:
        *out = JIR_SHL;
        *checked = 1;
        return 1;
    case JOG_TOK_SHR:
        *out = JIR_SHR;
        *checked = 1;
        return 1;
    default:
        return 0;
    }
}

static JirOperand emit_binop(Lower *L, JirBinOp op, int checked, JirOperand lhs, JirOperand rhs, JogType *ty, JogPos pos) {
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
    inst->checked = checked ? true : false;
    inst->a = lhs;
    inst->b = rhs;
    return jog_lower_local_op(L, dst);
}

bool jog_lower_lambda_store(Lower *L, JogNode *node) {
    JogNode *target;
    JogSymbol *symbol;
    JogTokenKind base;
    JirOperand addr;
    JirOperand value;
    JogType *ty;
    JirBinOp bin;
    int checked = 0;
    if (!L || !node || node->kind != JOG_NODE_ASSIGN || L->in_coro) {
        return false;
    }
    target = node->as.assign.target;
    if (!target || target->kind != JOG_NODE_IDENT || !target->symbol) {
        return false;
    }
    symbol = target->symbol;
    if (!is_cap(L, symbol)) {
        return false;
    }
    if (node->as.assign.op != JOG_TOK_ASSIGN) {
        base = compound_base(node->as.assign.op);
        if (base == JOG_TOK_EOF) {
            jog_lower_fail(L, node->pos, "operator '%s' is not lowered yet", jog_token_spelling(node->as.assign.op));
            return true;
        }
    } else {
        base = JOG_TOK_EOF;
    }
    addr = cap_addr(L, symbol, target, node->pos);
    if (L->failed) {
        return true;
    }
    ty = cap_type(L, symbol, target);
    if ((base == JOG_TOK_PLUS || base == JOG_TOK_MINUS) && ty && ty->kind == JOG_TY_PTR) {
        jog_lower_memory_ptr_update(L, addr, base, node->as.assign.value, ty, node->pos);
        return true;
    }
    if (base != JOG_TOK_EOF) {
        JirOperand old = load_ptr(L, addr, node->pos);
        JirOperand rhs = jog_lower_expr(L, node->as.assign.value);
        if (L->failed || L->terminated || !map_binop(base, &bin, &checked)) {
            if (!L->failed) {
                jog_lower_fail(L, node->pos, "operator is not lowered yet");
            }
            return true;
        }
        value = emit_binop(L, bin, checked, old, rhs, ty ? ty : rhs.type, node->pos);
        store_ptr(L, addr, value, node->pos);
        return true;
    }
    value = jog_lower_expr(L, node->as.assign.value);
    if (!L->terminated) {
        store_ptr(L, addr, value, node->pos);
    }
    return true;
}

JirOperand jog_lower_lambda_closure(Lower *L, JogType *fn_type, uint32_t func_index, JirOperand env, JogPos pos) {
    JirOperand args[2];
    JirInst *inst;
    uint32_t dst;
    if (L->failed || !fn_type) {
        return jir_none();
    }
    args[0] = jir_func_ref(L->module, func_index);
    args[1] = env;
    if (!jog_lower_new_local(L, NULL, fn_type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    if (!jir_set_args(L->module, inst, args, 2)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

bool jog_lower_lambda_prep(Lower *L, JogNode *node, JogLambdaPrep *prep) {
    LamScan scan;
    JogType *void_ty;
    JogType *void_ptr;
    JogType *tuple;
    JogType *tuple_ptr;
    JogType *slot_ptr;
    JirOperand tuple_addr;
    uint32_t tuple_local = 0;
    size_t i;
    JogPos pos;
    if (!prep) {
        return false;
    }
    memset(prep, 0, sizeof(*prep));
    prep->env = jir_none();
    if (!L || !node || L->failed) {
        return false;
    }
    pos = node->pos;
    memset(&scan, 0, sizeof(scan));
    scan.L = L;
    if (node->kind == JOG_NODE_LAMBDA) {
        scan_node(&scan, node->as.lambda.body);
    } else {
        scan_node(&scan, node);
    }
    prep->caps = scan.caps;
    prep->count = scan.count;
    if (scan.failed || L->failed) {
        return false;
    }
    if (scan.count == 0) {
        return true;
    }
    void_ty = universe_type(L, "void");
    void_ptr = void_ty ? jog_type_ptr(L->program->types, void_ty, false) : NULL;
    if (!void_ptr) {
        jog_lower_fail(L, pos, "missing void");
        return false;
    }
    prep->void_ptr = void_ptr;
    tuple = tuple_of(L, void_ptr, scan.count, pos);
    tuple_ptr = tuple ? jog_type_ptr(L->program->types, tuple, false) : NULL;
    slot_ptr = jog_type_ptr(L->program->types, void_ptr, false);
    if (!tuple || !tuple_ptr || !slot_ptr || !jog_lower_new_local(L, NULL, tuple, &tuple_local)) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "out of memory");
        }
        return false;
    }
    tuple_addr = addr_of_local(L, tuple_local, tuple_ptr, pos);
    prep->env = bitcast_op(L, tuple_addr, void_ptr, pos);
    for (i = 0; i < scan.count && !L->failed && !L->terminated; i++) {
        JirOperand raw;
        JirOperand slot;
        if (scan.caps[i].indirect) {
            raw = load_env_slot(L, L->lambda_env, L->lambda_cap_count, scan.caps[i].indirect_slot, void_ptr, pos);
        } else {
            JogType *ty = cap_type(L, scan.caps[i].symbol, NULL);
            JogType *ptr_ty = ty ? jog_type_ptr(L->program->types, ty, false) : NULL;
            JirOperand addr;
            if (!ptr_ty) {
                jog_lower_fail(L, pos, "missing capture");
                return false;
            }
            addr = addr_of_local(L, scan.caps[i].parent_local, ptr_ty, pos);
            raw = bitcast_op(L, addr, void_ptr, pos);
        }
        slot = field_ptr(L, tuple_addr, (uint32_t)i, slot_ptr, pos);
        store_ptr(L, slot, raw, pos);
    }
    return !L->failed;
}
