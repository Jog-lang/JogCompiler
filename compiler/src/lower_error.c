#include "lower_error.h"

#include "check_body.h"
#include "lower_generic.h"
#include "lower_memory.h"
#include "lower_struct.h"

#include "intern.h"

#include <string.h>

/* Cleanup blocks are duplicated at each exit. A return copies its value
   first, then runs cleanups, so a defer sees the variables but not a
   later assignment to the returned temporary. Panic does not enter this
   path. A return or break inside a cleanup body does not run the rest. */

typedef struct Cleanup {
    JogNode *node;
    bool err;
} Cleanup;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static JogType *underlying(JogType *type) {
    while (type && type->kind == JOG_TY_DISTINCT) {
        type = type->as.nominal.base;
    }
    return type;
}

static bool is_error_interface(const JogType *type) {
    const JogType *u = underlying((JogType *)type);
    return u && u->kind == JOG_TY_INTERFACE && u->as.nominal.name && strcmp(u->as.nominal.name, "error") == 0;
}

static bool is_error_option(const JogType *type) {
    return type && type->kind == JOG_TY_OPTION && is_error_interface(type->as.elem.elem);
}

static JogType *last_result(JogType *ret) {
    if (!ret) {
        return NULL;
    }
    if (ret->kind == JOG_TY_TUPLE && ret->as.tuple.count > 0 && ret->as.tuple.elems) {
        return ret->as.tuple.elems[ret->as.tuple.count - 1];
    }
    return ret;
}

static JogType *fn_ret(Lower *L) {
    JogType *ret;
    JogType *sub;
    if (!L->ast_fn || !L->ast_fn->type || L->ast_fn->type->kind != JOG_TY_FN) {
        return NULL;
    }
    ret = L->ast_fn->type->as.fn.ret;
    sub = jog_lower_type(L, ret);
    return sub ? sub : ret;
}

static void use_block(Lower *L, uint32_t block) {
    JirBlock *b = jir_block_at(L->func, block);
    L->block = block;
    L->terminated = !b || b->term.kind != JIR_TERM_NONE;
}

static bool new_block(Lower *L, const char *label, uint32_t *out) {
    size_t before;
    if (L->failed || !L->func) {
        return false;
    }
    before = L->func->blocks.count;
    *out = jir_block_add(L->func, label);
    if (L->func->blocks.count == before) {
        jog_lower_fail(L, L->func->pos, "out of memory");
        return false;
    }
    return true;
}

static void seal(Lower *L, JirTerm term) {
    if (L->failed || L->terminated) {
        return;
    }
    jir_terminate(L->func, L->block, term);
    L->terminated = true;
}

static void emit_br(Lower *L, uint32_t target, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_BR;
    term.target = target;
    term.pos = pos;
    seal(L, term);
}

static void emit_cond(Lower *L, JirOperand cond, uint32_t yes, uint32_t no, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_COND_BR;
    term.value = cond;
    term.target = yes;
    term.else_target = no;
    term.pos = pos;
    seal(L, term);
}

static void emit_ret(Lower *L, JirOperand value, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_RET;
    term.value = value;
    term.pos = pos;
    seal(L, term);
}

static void emit_unreachable(Lower *L, JogPos pos) {
    JirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = JIR_TERM_UNREACHABLE;
    term.pos = pos;
    seal(L, term);
}

static bool hold(Lower *L, JirOperand src, JogType *type, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JogType *ty = type ? type : src.type;
    if (!ty || ty->kind == JOG_TY_ERROR) {
        jog_lower_fail(L, pos, "lowering is missing a type");
        return false;
    }
    if (!jog_lower_new_local(L, NULL, ty, &dst)) {
        return false;
    }
    jog_lower_copy(L, dst, src, pos);
    *out = jog_lower_local_op(L, dst);
    return !L->failed;
}

static bool extract_at(Lower *L, JirOperand src, uint32_t index, JogType *field, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    JirOperand local;
    if (!field) {
        jog_lower_fail(L, pos, "lowering is missing a type");
        return false;
    }
    local = jog_lower_as_local(L, src, pos);
    if (L->failed) {
        return false;
    }
    if (!jog_lower_new_local(L, NULL, field, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->a = local;
    inst->aux = index;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool aggregate(Lower *L, JogType *type, const JirOperand *args, size_t count, JogPos pos, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_AGGREGATE, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    if (count > 0 && !jir_set_args(L->module, inst, args, count)) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool emit_cmp(Lower *L, JirBinOp op, JirOperand a, JirOperand b, JogPos pos, JirOperand *out) {
    JogType *boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    uint32_t dst;
    JirInst *inst;
    if (!boolean) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    if (!jog_lower_new_local(L, NULL, boolean, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_BINARY, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->aux = (uint32_t)op;
    inst->a = a;
    inst->b = b;
    *out = jog_lower_local_op(L, dst);
    return true;
}

static bool emit_not(Lower *L, JirOperand src, JogPos pos, JirOperand *out) {
    JogType *boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    uint32_t dst;
    JirInst *inst;
    if (!boolean) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    if (!jog_lower_new_local(L, NULL, boolean, &dst)) {
        return false;
    }
    inst = jog_lower_emit(L, JIR_UNARY, (int32_t)dst, pos);
    if (!inst) {
        return false;
    }
    inst->aux = (uint32_t)JIR_NOT;
    inst->a = src;
    *out = jog_lower_local_op(L, dst);
    return true;
}

bool jog_lower_error_open(Lower *L) {
    return L && ct_da_init(&L->cleanups, sizeof(Cleanup));
}

void jog_lower_error_close(Lower *L) {
    if (L) {
        ct_da_free(&L->cleanups);
    }
}

void jog_lower_error_reset(Lower *L) {
    if (!L) {
        return;
    }
    L->cleanups.count = 0;
    L->in_cleanup = false;
}

void jog_lower_error_note(Lower *L, JogNode *node) {
    Cleanup item;
    if (!L || L->failed || !node) {
        return;
    }
    item.node = node;
    item.err = node->kind == JOG_NODE_ERRDEFER;
    if (!ct_da_push(&L->cleanups, &item)) {
        jog_lower_fail(L, node->pos, "out of memory");
    }
}

static Cleanup *snapshot(Lower *L, size_t base, size_t *out_n) {
    size_t n;
    Cleanup *copy;
    Cleanup *first;
    *out_n = 0;
    if (!L || L->cleanups.count < base) {
        return NULL;
    }
    n = L->cleanups.count - base;
    if (n == 0) {
        return NULL;
    }
    copy = ct_arena_alloc(L->program->arena, n * sizeof(Cleanup), _Alignof(Cleanup));
    first = ct_da_at(&L->cleanups, base);
    if (!copy || !first) {
        jog_lower_fail(L, L->func ? L->func->pos : (JogPos){0}, "out of memory");
        return NULL;
    }
    memcpy(copy, first, n * sizeof(Cleanup));
    *out_n = n;
    return copy;
}

/* bodies are lowered last-in first out. resume switches to `done`. */
static void chain(Lower *L, JogNode **bodies, size_t n, uint32_t done, bool resume, JogPos pos) {
    uint32_t *blocks;
    size_t i;
    if (L->failed) {
        return;
    }
    if (n == 0) {
        if (!L->terminated) {
            emit_br(L, done, pos);
        }
        if (resume) {
            use_block(L, done);
        }
        return;
    }
    blocks = ct_arena_alloc(L->program->arena, n * sizeof(uint32_t), _Alignof(uint32_t));
    if (!blocks) {
        jog_lower_fail(L, pos, "out of memory");
        return;
    }
    for (i = 0; i < n; i++) {
        if (!new_block(L, "cleanup", &blocks[i])) {
            return;
        }
    }
    if (!L->terminated) {
        emit_br(L, blocks[0], pos);
    }
    for (i = 0; i < n && !L->failed; i++) {
        bool saved;
        use_block(L, blocks[i]);
        saved = L->in_cleanup;
        L->in_cleanup = true;
        if (bodies[i]) {
            jog_lower_stmt(L, bodies[i]);
        }
        L->in_cleanup = saved;
        if (!L->terminated && !L->failed) {
            emit_br(L, i + 1 < n ? blocks[i + 1] : done, pos);
        }
    }
    if (resume && !L->failed) {
        use_block(L, done);
    }
}

static size_t collect_bodies(Cleanup *items, size_t n, bool include_err, JogNode **out) {
    size_t filled = 0;
    size_t i;
    for (i = n; i-- > 0;) {
        if (!items[i].node) {
            continue;
        }
        if (items[i].err && !include_err) {
            continue;
        }
        out[filled++] = items[i].node->as.defer.body;
    }
    return filled;
}

void jog_lower_error_block_end(Lower *L, size_t base, JogPos pos) {
    size_t n = 0;
    size_t kept;
    Cleanup *items;
    JogNode **bodies;
    uint32_t cont;
    if (!L || L->failed) {
        return;
    }
    if (L->terminated) {
        if (L->cleanups.count > base) {
            L->cleanups.count = base;
        }
        return;
    }
    items = snapshot(L, base, &n);
    if (L->failed) {
        return;
    }
    if (n == 0) {
        return;
    }
    bodies = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    if (!bodies) {
        jog_lower_fail(L, pos, "out of memory");
        return;
    }
    kept = collect_bodies(items, n, false, bodies);
    L->cleanups.count = base;
    if (kept == 0) {
        return;
    }
    if (!new_block(L, "cleanup.next", &cont)) {
        return;
    }
    chain(L, bodies, kept, cont, true, pos);
}

void jog_lower_error_exit(Lower *L, size_t base, uint32_t target, JogPos pos) {
    size_t n = 0;
    size_t kept;
    Cleanup *items;
    JogNode **bodies;
    if (!L || L->failed || L->terminated) {
        return;
    }
    if (L->in_cleanup) {
        emit_br(L, target, pos);
        return;
    }
    items = snapshot(L, base, &n);
    if (L->failed) {
        return;
    }
    if (n == 0) {
        emit_br(L, target, pos);
        return;
    }
    bodies = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    if (!bodies) {
        jog_lower_fail(L, pos, "out of memory");
        return;
    }
    kept = collect_bodies(items, n, false, bodies);
    if (kept == 0) {
        emit_br(L, target, pos);
        return;
    }
    chain(L, bodies, kept, target, false, pos);
}

static bool error_present(Lower *L, JirOperand value, JogType *ret, JogPos pos, JirOperand *out) {
    JogType *last = last_result(ret);
    JirOperand opt = value;
    if (!last || !is_error_option(last)) {
        return false;
    }
    if (ret && ret->kind == JOG_TY_TUPLE) {
        if (!extract_at(L, value, (uint32_t)(ret->as.tuple.count - 1), last, pos, &opt)) {
            return false;
        }
    }
    if (jog_type_option_is_niche(last)) {
        return emit_cmp(L, JIR_NE, opt, jir_null(last), pos, out);
    }
    return extract_at(L, opt, 0, jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL), pos, out);
}

static void ret_here(Lower *L, JirOperand value, JogPos pos) {
    if (!L->failed && !L->terminated) {
        emit_ret(L, value, pos);
    }
}

static void return_plain(Lower *L, JirOperand value, Cleanup *items, size_t n, JogPos pos) {
    JogNode **bodies;
    size_t kept;
    uint32_t ret_b;
    if (n == 0) {
        ret_here(L, value, pos);
        return;
    }
    bodies = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    if (!bodies) {
        jog_lower_fail(L, pos, "out of memory");
        return;
    }
    kept = collect_bodies(items, n, false, bodies);
    if (kept == 0) {
        ret_here(L, value, pos);
        return;
    }
    if (!new_block(L, "ret", &ret_b)) {
        return;
    }
    chain(L, bodies, kept, ret_b, false, pos);
    use_block(L, ret_b);
    ret_here(L, value, pos);
}

static void return_err(Lower *L, JirOperand value, JogType *ret, Cleanup *items, size_t n, JogPos pos) {
    JogNode **all_bodies;
    JogNode **ok_bodies;
    size_t all_n;
    size_t ok_n;
    uint32_t err_b;
    uint32_t ok_b;
    uint32_t ret_b;
    JirOperand present;
    if (!error_present(L, value, ret, pos, &present) || L->failed || L->terminated) {
        if (!L->failed && !L->terminated) {
            return_plain(L, value, items, n, pos);
        }
        return;
    }
    all_bodies = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    ok_bodies = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    if (!all_bodies || !ok_bodies) {
        jog_lower_fail(L, pos, "out of memory");
        return;
    }
    all_n = collect_bodies(items, n, true, all_bodies);
    ok_n = collect_bodies(items, n, false, ok_bodies);
    if (!new_block(L, "cleanup.err", &err_b) || !new_block(L, "cleanup.ok", &ok_b) || !new_block(L, "ret", &ret_b)) {
        return;
    }
    emit_cond(L, present, err_b, ok_b, pos);
    use_block(L, err_b);
    chain(L, all_bodies, all_n, ret_b, false, pos);
    use_block(L, ok_b);
    chain(L, ok_bodies, ok_n, ret_b, false, pos);
    use_block(L, ret_b);
    ret_here(L, value, pos);
}

static JogType *universe_named(Lower *L, const char *name, JogPos pos) {
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
        jog_lower_fail(L, pos, "out of memory");
        return NULL;
    }
    symbol = jog_package_scope_lookup(pkg->universe, interned);
    return symbol ? symbol->type : NULL;
}

static JogType *void_ptr_type(Lower *L, JogPos pos) {
    JogType *void_ty = universe_named(L, "void", pos);
    JogType *ptr;
    if (L->failed) {
        return NULL;
    }
    if (!void_ty) {
        jog_lower_fail(L, pos, "missing void");
        return NULL;
    }
    ptr = jog_type_ptr(L->program->types, void_ty, false);
    if (!ptr) {
        jog_lower_fail(L, pos, "out of memory");
    }
    return ptr;
}

static JirOperand bitcast_to(Lower *L, JirOperand value, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || L->terminated) {
        return jir_none();
    }
    if (value.type == type) {
        return value;
    }
    if (!type || !jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = value;
    return jog_lower_local_op(L, dst);
}

/* Both interface words are the sentinel address, matching ErrCancelled. */
static bool iface_words(Lower *L, JogType *iface, JirOperand ptr, JogPos pos, JirOperand *out) {
    JogType *data_ty;
    JirOperand data;
    JirOperand args[2];
    data_ty = void_ptr_type(L, pos);
    if (!data_ty || L->failed) {
        return false;
    }
    data = bitcast_to(L, ptr, data_ty, pos);
    if (L->failed || data.kind == JIR_OPND_NONE) {
        return false;
    }
    args[0] = data;
    args[1] = data;
    return aggregate(L, iface, args, 2, pos, out);
}

JirOperand jog_lower_error_box(Lower *L, JirOperand src, JogType *slot, JogPos pos) {
    JogType *iface;
    JirOperand boxed;
    JirOperand args[2];
    JogType *boolean;
    if (!L || L->failed || src.kind == JIR_OPND_NONE || !slot || !src.type) {
        return src;
    }
    slot = jog_lower_type(L, slot);
    if (L->failed || !slot) {
        return jir_none();
    }
    if (src.type == slot || (is_error_option(src.type) && is_error_option(slot)) ||
        (is_error_interface(src.type) && is_error_interface(slot))) {
        return src;
    }
    if (!is_error_option(slot) && !is_error_interface(slot)) {
        return src;
    }
    iface = is_error_option(slot) ? underlying(slot->as.elem.elem) : slot;
    if (!is_error_interface(iface)) {
        return src;
    }
    if (is_error_interface(src.type)) {
        boxed = src;
    } else if (src.type->kind == JOG_TY_PTR) {
        if (!iface_words(L, iface, src, pos, &boxed)) {
            return jir_none();
        }
    } else {
        return src;
    }
    if (!is_error_option(slot) || L->failed) {
        return boxed;
    }
    boolean = jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL);
    if (!boolean) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    args[0] = jir_int(boolean, 1);
    args[1] = boxed;
    if (!aggregate(L, slot, args, 2, pos, &boxed)) {
        return jir_none();
    }
    return boxed;
}

static bool operand_data(Lower *L, JirOperand value, JogType *type, JogPos pos, JirOperand *out) {
    JogType *vp;
    JirOperand iface;
    JirOperand ptr;
    type = type ? jog_lower_type(L, type) : value.type;
    if (L->failed || !type) {
        return false;
    }
    if (is_error_option(type)) {
        JogType *elem = underlying(type->as.elem.elem);
        if (!extract_at(L, value, 1, elem, pos, &iface)) {
            return false;
        }
        value = iface;
        type = elem;
    }
    if (is_error_interface(type)) {
        vp = void_ptr_type(L, pos);
        if (!vp || !extract_at(L, value, 0, vp, pos, &ptr)) {
            return false;
        }
        *out = ptr;
        return !L->failed;
    }
    if (type->kind == JOG_TY_PTR) {
        vp = void_ptr_type(L, pos);
        if (!vp) {
            return false;
        }
        ptr = bitcast_to(L, value, vp, pos);
        if (L->failed || ptr.kind == JIR_OPND_NONE) {
            return false;
        }
        *out = ptr;
        return true;
    }
    return false;
}

static bool is_sentinel_operand(const JogType *type) {
    return is_error_interface(type) || (type && type->kind == JOG_TY_PTR);
}

static bool sentinel_pair(const JogType *left, const JogType *right) {
    if (is_error_option(left) && !is_error_option(right) && is_sentinel_operand(right)) {
        return true;
    }
    if (is_error_option(right) && !is_error_option(left) && is_sentinel_operand(left)) {
        return true;
    }
    return false;
}

static bool build_return(Lower *L, JogNode *node, JogType *ret, JirOperand *out) {
    size_t count = node->as.ret.values.count;
    size_t i;
    JirOperand *args;
    JirOperand one;
    if (count == 0) {
        *out = jir_none();
        return true;
    }
    if (count == 1) {
        one = jog_lower_expr(L, list_at(node->as.ret.values, 0));
        if (L->failed || L->terminated) {
            return false;
        }
        if (!ret || ret->kind == JOG_TY_UNIT) {
            *out = jir_none();
            return true;
        }
        one = jog_lower_error_box(L, one, ret, node->pos);
        if (L->failed) {
            return false;
        }
        *out = one;
        return true;
    }
    /* The checker only lets a matching tuple through. A substituted return
       type is concrete here. Anything else is still a guard, not a form the
       notes describe. */
    if (!ret || ret->kind != JOG_TY_TUPLE || ret->as.tuple.count != count) {
        jog_lower_fail(L, node->pos, "multiple return values are not lowered yet");
        return false;
    }
    args = ct_arena_alloc(L->program->arena, count * sizeof(JirOperand), _Alignof(JirOperand));
    if (!args) {
        jog_lower_fail(L, node->pos, "out of memory");
        return false;
    }
    for (i = 0; i < count; i++) {
        JogType *slot = ret->as.tuple.elems ? ret->as.tuple.elems[i] : NULL;
        args[i] = jog_lower_expr(L, list_at(node->as.ret.values, i));
        if (L->failed || L->terminated) {
            return false;
        }
        args[i] = jog_lower_error_box(L, args[i], slot, node->pos);
        if (L->failed) {
            return false;
        }
    }
    return aggregate(L, ret, args, count, node->pos, out);
}

void jog_lower_error_return(Lower *L, JogNode *node) {
    JogType *ret;
    JirOperand value;
    size_t n = 0;
    Cleanup *items;
    bool any_err = false;
    size_t i;
    if (!L || !node || L->failed || L->terminated) {
        return;
    }
    ret = fn_ret(L);
    if (!build_return(L, node, ret, &value) || L->failed || L->terminated) {
        return;
    }
    if (L->in_cleanup) {
        ret_here(L, value, node->pos);
        return;
    }
    items = snapshot(L, 0, &n);
    if (L->failed) {
        return;
    }
    /* Copy before any cleanup. A defer may assign the variable that was returned,
       and another exit of this block still needs the same cleanup stack. */
    if (n > 0 && value.kind != JIR_OPND_NONE) {
        JirOperand held;
        if (!hold(L, value, value.type, node->pos, &held)) {
            return;
        }
        value = held;
    }
    for (i = 0; i < n; i++) {
        if (items[i].err) {
            any_err = true;
        }
    }
    if (any_err && is_error_option(last_result(ret))) {
        return_err(L, value, ret, items, n, node->pos);
        return;
    }
    return_plain(L, value, items, n, node->pos);
}

static const char *bind_name(const JogNode *node, size_t index) {
    if (!node->as.binding.names.items || index >= node->as.binding.names.count) {
        return NULL;
    }
    return node->as.binding.names.items[index];
}

static JogSymbol *bind_symbol(const JogNode *node, size_t index) {
    if (!node->bound_symbols || index >= node->bound_symbol_count) {
        return NULL;
    }
    return node->bound_symbols[index];
}

void jog_lower_error_binding(Lower *L, JogNode *node) {
    JogType *tuple;
    JirOperand value;
    size_t count;
    size_t i;
    bool have;
    if (!L || !node || L->failed || L->terminated) {
        return;
    }
    tuple = node->type;
    if (!tuple || tuple->kind != JOG_TY_TUPLE) {
        jog_lower_fail(L, node->pos, "destructuring is missing a tuple type");
        return;
    }
    count = node->as.binding.names.count;
    have = node->as.binding.init != NULL;
    value = jir_none();
    if (have) {
        JirOperand init = jog_lower_expr(L, node->as.binding.init);
        if (L->failed || L->terminated) {
            return;
        }
        if (!hold(L, init, tuple, node->pos, &value)) {
            return;
        }
    }
    for (i = 0; i < count && !L->failed && !L->terminated; i++) {
        const char *name = bind_name(node, i);
        JogSymbol *symbol = bind_symbol(node, i);
        JogType *elem = tuple->as.tuple.elems ? tuple->as.tuple.elems[i] : NULL;
        uint32_t local;
        JirOperand field;
        if (!elem && symbol) {
            elem = symbol->type;
        }
        if (is_discard(name)) {
            continue;
        }
        if (!jog_lower_new_local(L, name, elem, &local)) {
            return;
        }
        if (symbol) {
            LocalBind bind;
            bind.symbol = symbol;
            bind.local = local;
            if (!ct_da_push(&L->locals, &bind)) {
                jog_lower_fail(L, node->pos, "out of memory");
                return;
            }
        }
        if (have) {
            if (!extract_at(L, value, (uint32_t)i, elem, node->pos, &field)) {
                return;
            }
            jog_lower_copy(L, local, field, node->pos);
        } else if (jog_check_type_has_zero(elem)) {
            jog_lower_zero(L, local, node->pos);
        }
    }
}

static bool discard_target(const JogNode *node) {
    return node && node->kind == JOG_NODE_IDENT && is_discard(node->as.ident.name);
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

static JirOperand emit_addr_local(Lower *L, uint32_t local, JogType *type, JogPos pos) {
    JogType *ptr;
    uint32_t dst;
    JirInst *inst;
    ptr = type ? jog_type_ptr(L->program->types, type, false) : NULL;
    if (!ptr || !jog_lower_new_local(L, NULL, ptr, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jog_lower_local_op(L, local);
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_field_ptr(Lower *L, JirOperand base, uint32_t index, JogType *field, bool is_const, JogPos pos) {
    JogType *ptr;
    uint32_t dst;
    JirInst *inst;
    ptr = field ? jog_type_ptr(L->program->types, field, is_const) : NULL;
    if (!ptr || !jog_lower_new_local(L, NULL, ptr, &dst)) {
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

static JirOperand emit_load_ptr(Lower *L, JirOperand ptr, JogPos pos) {
    JogType *elem;
    uint32_t dst;
    JirInst *inst;
    elem = ptr.type && ptr.type->kind == JOG_TY_PTR ? ptr.type->as.elem.elem : NULL;
    if (!elem || !jog_lower_new_local(L, NULL, elem, &dst)) {
        jog_lower_fail(L, pos, "cannot load this pointer");
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = ptr;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_bitcast(Lower *L, JirOperand src, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (L->failed || !type) {
        return jir_none();
    }
    if (src.type == type) {
        return src;
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = src;
    return jog_lower_local_op(L, dst);
}

static JirOperand emit_elem_addr(Lower *L, JirOperand ptr, JirOperand index, JogType *ptr_ty, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (ptr.type != ptr_ty) {
        ptr = emit_bitcast(L, ptr, ptr_ty, pos);
    }
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

static JirOperand place_addr(Lower *L, JogNode *node);

/* Address of an indexed element. Pointers are not bounds-checked. */
static JirOperand index_addr(Lower *L, JogNode *node) {
    JogNode *base_node;
    JogType *base;
    JogType *elem;
    JogType *ptr_ty;
    JogType *usize;
    JirOperand index;
    JirOperand raw;
    JirOperand len;
    bool bounded = false;
    if (!node || L->failed) {
        return jir_none();
    }
    base_node = node->as.index.base;
    base = base_node && base_node->type ? jog_lower_type(L, base_node->type) : NULL;
    usize = jog_type_builtin(L->program->types, JOG_BUILTIN_USIZE);
    if (!base || !usize || !base_node) {
        jog_lower_fail(L, node->pos, "cannot index this value");
        return jir_none();
    }
    index = jog_lower_expr(L, node->as.index.index);
    if (L->failed) {
        return jir_none();
    }
    if (base->kind == JOG_TY_PTR) {
        elem = base->as.elem.elem;
        ptr_ty = elem ? jog_type_ptr(L->program->types, elem, base->is_const) : NULL;
        raw = jog_lower_expr(L, base_node);
        if (!ptr_ty || L->failed) {
            return jir_none();
        }
        return emit_elem_addr(L, raw, index, ptr_ty, node->pos);
    }
    if (base->kind == JOG_TY_SLICE) {
        elem = base->as.elem.elem;
        ptr_ty = elem ? jog_type_ptr(L->program->types, elem, base->is_const) : NULL;
        raw = jog_lower_expr(L, base_node);
        len = jir_none();
        if (ptr_ty) {
            JirOperand header = raw;
            if (!extract_at(L, header, 0, ptr_ty, node->pos, &raw) ||
                !extract_at(L, header, 1, usize, node->pos, &len)) {
                return jir_none();
            }
        }
        bounded = true;
    } else if (base->kind == JOG_TY_ARRAY) {
        elem = base->as.array.elem;
        ptr_ty = elem ? jog_type_ptr(L->program->types, elem, false) : NULL;
        raw = place_addr(L, base_node);
        len = jir_int(usize, base->as.array.len);
        bounded = true;
    } else {
        jog_lower_fail(L, node->pos, "destructuring target is not lowered yet");
        return jir_none();
    }
    if (L->failed || !ptr_ty) {
        return jir_none();
    }
    if (bounded) {
        emit_bounds(L, index, len, node->pos);
    }
    return emit_elem_addr(L, raw, index, ptr_ty, node->pos);
}

static JirOperand place_addr(Lower *L, JogNode *node) {
    JogType *type;
    uint32_t local;
    JirOperand global;
    if (!node || L->failed) {
        return jir_none();
    }
    type = node->type ? jog_lower_type(L, node->type) : NULL;
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        if (jog_lower_find_local(L, node->symbol, &local)) {
            return emit_addr_local(L, local, type, node->pos);
        }
        global = jog_lower_global_addr(L, node->symbol);
        if (global.kind != JIR_OPND_NONE) {
            return global;
        }
        jog_lower_fail(L, node->pos, "missing local");
        return jir_none();
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        return jog_lower_expr(L, node->as.unary.operand);
    }
    if (node->kind == JOG_NODE_MEMBER && !node->as.member.name) {
        return jog_lower_tuple_field_ptr(L, node);
    }
    if (node->kind == JOG_NODE_MEMBER && node->as.member.name) {
        return jog_lower_field_ptr(L, node);
    }
    if (node->kind == JOG_NODE_INDEX) {
        return index_addr(L, node);
    }
    jog_lower_fail(L, node->pos, "destructuring target is not lowered yet");
    return jir_none();
}

/* Pointer to member's tuple field. A pointer base is already that address. */
JirOperand jog_lower_tuple_field_ptr(Lower *L, JogNode *member) {
    JogNode *base;
    JogType *base_ty;
    JogType *tuple;
    JogType *field;
    JirOperand ptr;
    bool is_const = false;
    uint32_t index;
    if (!member || L->failed) {
        return jir_none();
    }
    base = member->as.member.base;
    base_ty = base && base->type ? jog_lower_type(L, base->type) : NULL;
    index = member->as.member.tuple_index;
    if (!base || !base_ty) {
        jog_lower_fail(L, member->pos, "tuple field is not lowered yet");
        return jir_none();
    }
    if (base_ty->kind == JOG_TY_PTR && base_ty->as.elem.elem && base_ty->as.elem.elem->kind == JOG_TY_TUPLE) {
        tuple = base_ty->as.elem.elem;
        is_const = base_ty->is_const;
        ptr = jog_lower_expr(L, base);
    } else if (base_ty->kind == JOG_TY_TUPLE) {
        tuple = base_ty;
        ptr = place_addr(L, base);
    } else {
        jog_lower_fail(L, member->pos, "tuple field is not lowered yet");
        return jir_none();
    }
    if (L->failed || !tuple || !tuple->as.tuple.elems || (size_t)index >= tuple->as.tuple.count) {
        if (!L->failed) {
            jog_lower_fail(L, member->pos, "tuple field is not lowered yet");
        }
        return jir_none();
    }
    field = tuple->as.tuple.elems[index];
    return emit_field_ptr(L, ptr, index, field, is_const, member->pos);
}

/* Store value through an lvalue that is not a plain local. */
static bool store_operand(Lower *L, JogNode *target, JirOperand value, JogPos pos) {
    JogSymbol *sym;
    JogNode ident;
    JogNode assign;
    if (value.kind != JIR_OPND_LOCAL) {
        JirOperand held;
        if (!hold(L, value, value.type, pos, &held)) {
            return false;
        }
        value = held;
    }
    if (target && target->kind == JOG_NODE_MEMBER && !target->as.member.name) {
        JirOperand ptr = jog_lower_tuple_field_ptr(L, target);
        if (L->failed) {
            return false;
        }
        emit_store(L, ptr, value, pos);
        return !L->failed;
    }
    sym = ct_arena_alloc(L->program->arena, sizeof(JogSymbol), _Alignof(JogSymbol));
    if (!sym) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    memset(sym, 0, sizeof(*sym));
    sym->kind = JOG_SYM_VARIABLE;
    sym->type = value.type;
    if (!jog_lower_bind_local(L, sym, value.as.local)) {
        return false;
    }
    memset(&ident, 0, sizeof(ident));
    memset(&assign, 0, sizeof(assign));
    ident.kind = JOG_NODE_IDENT;
    ident.pos = pos;
    ident.type = value.type;
    ident.symbol = sym;
    assign.kind = JOG_NODE_ASSIGN;
    assign.pos = pos;
    assign.as.assign.op = JOG_TOK_ASSIGN;
    assign.as.assign.target = target;
    assign.as.assign.value = &ident;
    if (target && (target->kind == JOG_NODE_INDEX ||
                   (target->kind == JOG_NODE_UNARY && target->as.unary.op == JOG_TOK_STAR))) {
        if (!jog_lower_memory_assign(L, &assign)) {
            jog_lower_fail(L, pos, "destructuring target is not lowered yet");
        }
        return !L->failed;
    }
    if (target && target->kind == JOG_NODE_MEMBER && target->as.member.name) {
        jog_lower_field_assign(L, &assign);
        return !L->failed;
    }
    jog_lower_fail(L, pos, "destructuring target is not lowered yet");
    return false;
}

static void store_one(Lower *L, JogNode *target, JirOperand value) {
    uint32_t local;
    JirOperand global;
    if (!target || L->failed || L->terminated) {
        return;
    }
    if (target->kind == JOG_NODE_IDENT && target->symbol) {
        if (jog_lower_find_local(L, target->symbol, &local)) {
            jog_lower_copy(L, local, value, target->pos);
            return;
        }
        global = jog_lower_global_addr(L, target->symbol);
        if (global.kind != JIR_OPND_NONE) {
            emit_store(L, global, value, target->pos);
            return;
        }
        jog_lower_fail(L, target->pos, "missing local");
        return;
    }
    (void)store_operand(L, target, value, target->pos);
}

static void store_list(Lower *L, JogList targets, JirOperand tuple, JogType *tuple_ty) {
    size_t count;
    size_t i;
    if (L->failed || L->terminated) {
        return;
    }
    if (!tuple_ty || tuple_ty->kind != JOG_TY_TUPLE) {
        jog_lower_fail(L, targets.count ? (list_at(targets, 0) ? list_at(targets, 0)->pos : (JogPos){0}) : (JogPos){0},
                       "destructuring is missing a tuple type");
        return;
    }
    count = targets.count;
    for (i = 0; i < count && !L->failed && !L->terminated; i++) {
        JogNode *target = list_at(targets, i);
        JogType *elem = tuple_ty->as.tuple.elems ? tuple_ty->as.tuple.elems[i] : NULL;
        JirOperand field;
        if (!target || discard_target(target)) {
            continue;
        }
        if (!extract_at(L, tuple, (uint32_t)i, elem, target->pos, &field)) {
            return;
        }
        if (target->kind == JOG_NODE_TUPLE) {
            store_list(L, target->as.tuple.elems, field, elem);
            continue;
        }
        store_one(L, target, field);
    }
}

void jog_lower_error_destructure(Lower *L, JogNode *node) {
    JogType *tuple;
    JirOperand value;
    JirOperand init;
    if (!L || !node || L->failed || L->terminated) {
        return;
    }
    init = jog_lower_expr(L, node->as.destructure.value);
    if (L->failed || L->terminated) {
        return;
    }
    tuple = node->as.destructure.value ? node->as.destructure.value->type : NULL;
    tuple = jog_lower_type(L, tuple);
    if (!tuple || tuple->kind != JOG_TY_TUPLE) {
        jog_lower_fail(L, node->pos, "destructuring is missing a tuple type");
        return;
    }
    if (!hold(L, init, tuple, node->pos, &value)) {
        return;
    }
    store_list(L, node->as.destructure.targets, value, tuple);
}

JirOperand jog_lower_error_tuple(Lower *L, JogNode *node) {
    size_t count;
    size_t i;
    JirOperand *args;
    JirOperand out;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    count = node->as.tuple.elems.count;
    if (!node->type || node->type->kind != JOG_TY_TUPLE) {
        jog_lower_fail(L, node->pos, "tuple is missing a type");
        return jir_none();
    }
    args = NULL;
    if (count > 0) {
        args = ct_arena_alloc(L->program->arena, count * sizeof(JirOperand), _Alignof(JirOperand));
        if (!args) {
            jog_lower_fail(L, node->pos, "out of memory");
            return jir_none();
        }
    }
    for (i = 0; i < count; i++) {
        args[i] = jog_lower_expr(L, list_at(node->as.tuple.elems, i));
        if (L->failed || L->terminated) {
            return jir_none();
        }
    }
    if (!aggregate(L, node->type, args, count, node->pos, &out)) {
        return jir_none();
    }
    return out;
}

JirOperand jog_lower_error_field(Lower *L, JogNode *node) {
    JirOperand base;
    JirOperand out;
    JogType *type;
    JogType *field;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    type = node->as.member.base && node->as.member.base->type ? jog_lower_type(L, node->as.member.base->type) : NULL;
    if (type && type->kind == JOG_TY_PTR && type->as.elem.elem && type->as.elem.elem->kind == JOG_TY_TUPLE) {
        JirOperand ptr = jog_lower_tuple_field_ptr(L, node);
        if (L->failed) {
            return jir_none();
        }
        return emit_load_ptr(L, ptr, node->pos);
    }
    base = jog_lower_expr(L, node->as.member.base);
    if (L->failed || L->terminated) {
        return jir_none();
    }
    if (!type || type->kind != JOG_TY_TUPLE || !type->as.tuple.elems ||
        (size_t)node->as.member.tuple_index >= type->as.tuple.count) {
        jog_lower_fail(L, node->pos, "tuple field is not lowered yet");
        return jir_none();
    }
    field = type->as.tuple.elems[node->as.member.tuple_index];
    if (!extract_at(L, base, node->as.member.tuple_index, field, node->pos, &out)) {
        return jir_none();
    }
    return out;
}

JirOperand jog_lower_error_null(Lower *L, JogNode *node) {
    JogType *type;
    uint32_t dst;
    if (!L || !node || L->failed) {
        return jir_none();
    }
    type = node->type ? jog_lower_type(L, node->type) : NULL;
    if (type && type->kind == JOG_TY_OPTION && !jog_type_option_is_niche(type)) {
        if (!jog_lower_new_local(L, NULL, type, &dst)) {
            return jir_none();
        }
        jog_lower_zero(L, dst, node->pos);
        return jog_lower_local_op(L, dst);
    }
    return jir_null(type);
}

bool jog_lower_error_compare(Lower *L, JogNode *node, JirOperand lhs, JirOperand rhs, JirOperand *out) {
    JogNode *left;
    JogNode *right;
    JogType *lt;
    JogType *rt;
    JogTokenKind op;
    bool left_null;
    bool right_null;
    JogType *opt;
    JirOperand value;
    JirOperand has;
    if (!L || !node || !out || L->failed) {
        return false;
    }
    op = node->as.binary.op;
    if (op != JOG_TOK_EQ && op != JOG_TOK_NEQ) {
        return false;
    }
    left = node->as.binary.lhs;
    right = node->as.binary.rhs;
    lt = left ? left->type : NULL;
    rt = right ? right->type : NULL;
    left_null = lt && lt->kind == JOG_TY_NULL;
    right_null = rt && rt->kind == JOG_TY_NULL;
    if (left_null && rt && rt->kind == JOG_TY_OPTION) {
        opt = rt;
        value = rhs;
    } else if (right_null && lt && lt->kind == JOG_TY_OPTION) {
        opt = lt;
        value = lhs;
    } else if (lt && rt && lt->kind == JOG_TY_OPTION && lt == rt && jog_type_option_is_niche(lt)) {
        return emit_cmp(L, op == JOG_TOK_EQ ? JIR_EQ : JIR_NE, lhs, rhs, node->pos, out);
    } else if (sentinel_pair(lt, rt)) {
        JirOperand lp;
        JirOperand rp;
        if (!operand_data(L, lhs, lt, node->pos, &lp) || !operand_data(L, rhs, rt, node->pos, &rp)) {
            return true;
        }
        return emit_cmp(L, op == JOG_TOK_EQ ? JIR_EQ : JIR_NE, lp, rp, node->pos, out);
    } else {
        return false;
    }
    if (jog_type_option_is_niche(opt)) {
        return emit_cmp(L, op == JOG_TOK_EQ ? JIR_EQ : JIR_NE, value, jir_null(opt), node->pos, out);
    }
    if (!extract_at(L, value, 0, jog_type_builtin(L->program->types, JOG_BUILTIN_BOOL), node->pos, &has)) {
        return true;
    }
    if (op == JOG_TOK_NEQ) {
        *out = has;
        return true;
    }
    return emit_not(L, has, node->pos, out);
}

static bool is_builtin_panic(const JogNode *node) {
    const JogNode *callee;
    if (!node || node->kind != JOG_NODE_CALL) {
        return false;
    }
    callee = node->as.call.callee;
    return callee && callee->kind == JOG_NODE_IDENT && callee->as.ident.name &&
           strcmp(callee->as.ident.name, "panic") == 0 && !callee->symbol;
}

bool jog_lower_error_panic(Lower *L, JogNode *node) {
    JogNode *arg;
    JogNode *value;
    JirOperand message;
    JirInst *inst;
    if (!is_builtin_panic(node)) {
        return false;
    }
    if (!L || L->failed) {
        return true;
    }
    arg = list_at(node->as.call.args, 0);
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : NULL;
    message = jog_lower_expr(L, value);
    if (L->failed || L->terminated) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_PANIC, -1, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = message;
    emit_unreachable(L, node->pos);
    return true;
}
