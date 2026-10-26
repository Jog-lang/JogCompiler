#include "lower_context.h"

#include "lower_generic.h"
#include "lower_interface.h"

#include "resolve_package.h"

#include "da.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

/* A with block evaluates each value once, then the body reads those locals
   through the symbols the checker stored. Calls pass context_args, which
   are either those symbols or the explicit with expressions. A bare root
   fn main() materializes Allocator, ErrorAllocator, Scope, and Logger on
   first use. Those locals are not parameters of jog_entry_main. The test
   harness supplies std.testing.Testing from testing() when no context
   global has that type. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

void jog_lower_context_with(Lower *L, JogNode *node) {
    size_t n;
    size_t i;
    JirOperand *vals;
    if (!L || !node || L->failed) {
        return;
    }
    n = node->as.with.values.count;
    vals = NULL;
    if (n > 0) {
        vals = ct_arena_alloc(L->program->arena, n * sizeof(*vals), _Alignof(JirOperand));
        if (!vals) {
            jog_lower_fail(L, node->pos, "out of memory");
            return;
        }
        for (i = 0; i < n && !L->failed; i++) {
            vals[i] = jog_lower_expr(L, list_at(node->as.with.values, i));
        }
    }
    if (L->failed) {
        return;
    }
    for (i = 0; i < n && !L->failed; i++) {
        JogSymbol *symbol = NULL;
        JogType *type;
        uint32_t local;
        if (node->context_symbols && i < node->context_symbol_count) {
            symbol = node->context_symbols[i];
        }
        type = symbol ? symbol->type : NULL;
        if (!type && vals) {
            type = vals[i].type;
        }
        if (!symbol || !type || type->kind == JOG_TY_ERROR) {
            continue;
        }
        if (!jog_lower_new_local(L, symbol->name, type, &local)) {
            return;
        }
        jog_lower_copy(L, local, vals[i], node->pos);
        if (!jog_lower_bind_local(L, symbol, local)) {
            return;
        }
    }
    if (L->failed || !node->as.with.body) {
        return;
    }
    if (node->as.with.body->kind == JOG_NODE_BLOCK) {
        jog_lower_stmt(L, node->as.with.body);
        return;
    }
    jog_lower_stmt(L, node->as.with.body);
}

bool jog_lower_context_args(Lower *L, JogNode *call, JirOperand *args, size_t base) {
    JogNode *callee;
    JogType *fn;
    size_t n;
    size_t i;
    if (!L || !call || L->failed) {
        return false;
    }
    n = call->context_arg_count;
    callee = call->as.call.callee;
    fn = callee ? callee->type : NULL;
    if ((!fn || fn->kind != JOG_TY_FN) && call->symbol && call->symbol->type && call->symbol->type->kind == JOG_TY_FN) {
        fn = call->symbol->type;
    }
    if (fn && fn->kind == JOG_TY_FN && fn->as.fn.context_count != n) {
        jog_lower_fail(L, call->pos, "missing context argument");
        return false;
    }
    if (n == 0) {
        return true;
    }
    if (!args || !call->context_args) {
        jog_lower_fail(L, call->pos, "missing context argument");
        return false;
    }
    for (i = 0; i < n; i++) {
        JogType *need = NULL;
        JirOperand op;
        if (fn && fn->kind == JOG_TY_FN && fn->as.fn.contexts && i < fn->as.fn.context_count) {
            need = jog_lower_type(L, fn->as.fn.contexts[i]);
        }
        op = jog_lower_expr(L, call->context_args[i]);
        if (L->failed) {
            return false;
        }
        if (need && need->kind == JOG_TY_INTERFACE && op.type && op.type->kind != JOG_TY_INTERFACE &&
            op.kind != JIR_OPND_NONE) {
            op = jog_lower_iface_convert(L, op, need, call->pos);
            if (L->failed) {
                return false;
            }
        }
        args[base + i] = op;
    }
    return true;
}

static void type_text(const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (!buf || cap == 0) {
        return;
    }
    buf[0] = '\0';
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    if (!sb.failed && text) {
        snprintf(buf, cap, "%s", text);
    } else {
        snprintf(buf, cap, "?");
    }
    ct_sb_free(&sb);
}

static JirOperand load_context(Lower *L, JogSymbol *symbol, JogType *type, JogPos pos) {
    JirOperand addr;
    uint32_t dst;
    JirInst *inst;
    addr = jog_lower_global_addr(L, symbol);
    if (addr.kind != JIR_OPND_GLOBAL) {
        jog_lower_fail(L, pos, "missing context");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = addr;
    return jog_lower_local_op(L, dst);
}

static JogNode *pkg_fn(Lower *L, const char *path, const char *name, JogPos pos);
static JirOperand call_decl(Lower *L, JogNode *decl, JogPos pos);

static int is_testing_type(const JogType *type) {
    const char *name;
    const char *pkg;
    if (!type || type->kind != JOG_TY_INTERFACE) {
        return 0;
    }
    name = type->as.nominal.name;
    pkg = type->as.nominal.package;
    return name && pkg && strcmp(name, "Testing") == 0 && strcmp(pkg, "std.testing") == 0;
}

bool jog_lower_context_supply(Lower *L, JogNode *fn, JirOperand **args_out, size_t *count_out) {
    JogType *type;
    size_t n;
    size_t i;
    JirOperand *args;
    const JogPackage *from;
    if (args_out) {
        *args_out = NULL;
    }
    if (count_out) {
        *count_out = 0;
    }
    if (!L || !fn || !args_out || !count_out || L->failed) {
        return false;
    }
    if (fn->kind != JOG_NODE_FN || !fn->type || fn->type->kind != JOG_TY_FN) {
        return !L->failed;
    }
    type = fn->type;
    n = type->as.fn.context_count;
    if (n == 0) {
        return true;
    }
    args = ct_arena_alloc(L->program->arena, n * sizeof(*args), _Alignof(JirOperand));
    if (!args) {
        jog_lower_fail(L, fn->pos, "out of memory");
        return false;
    }
    memset(args, 0, n * sizeof(*args));
    from = fn->symbol ? fn->symbol->package : NULL;
    for (i = 0; i < n && !L->failed; i++) {
        JogType *want = type->as.fn.contexts ? type->as.fn.contexts[i] : NULL;
        JogSymbol *found = NULL;
        int matches = 0;
        size_t g;
        char text[160];
        if (!want || want->kind == JOG_TY_ERROR) {
            continue;
        }
        for (g = 0; g < L->globals.count; g++) {
            GlobalBind *bind = ct_da_at(&L->globals, g);
            if (!bind || !bind->symbol || bind->symbol->kind != JOG_SYM_CONTEXT) {
                continue;
            }
            if (bind->symbol->type != want) {
                continue;
            }
            if (!jog_symbol_visible_from(bind->symbol, from, fn->pos.file)) {
                continue;
            }
            if (matches == 0) {
                found = bind->symbol;
            }
            matches++;
        }
        type_text(want, text, sizeof(text));
        if (matches == 0) {
            if (is_testing_type(want)) {
                JogNode *decl = pkg_fn(L, "std.testing", "testing", fn->pos);
                args[i] = call_decl(L, decl, fn->pos);
                if (L->failed) {
                    return false;
                }
                continue;
            }
            jog_lower_fail(L, fn->pos, "missing context %s", text);
            return false;
        }
        if (matches > 1) {
            jog_lower_fail(L, fn->pos, "ambiguous context %s", text);
            return false;
        }
        args[i] = load_context(L, found, want, fn->pos);
    }
    if (L->failed) {
        return false;
    }
    *args_out = args;
    *count_out = n;
    return true;
}

enum {
    DEF_ALLOC = 1,
    DEF_ERR,
    DEF_SCOPE,
    DEF_LOG
};

static const JogSymbol *opening;

static int on_main_defaults(const JogSymbol *symbol) {
    const JogNode *fn;
    size_t i;
    if (!symbol || symbol->kind != JOG_SYM_CONTEXT || !symbol->decl || symbol->decl->kind != JOG_NODE_FN) {
        return 0;
    }
    fn = symbol->decl;
    if (!fn->context_symbols) {
        return 0;
    }
    for (i = 0; i < fn->context_symbol_count; i++) {
        if (fn->context_symbols[i] == symbol) {
            return 1;
        }
    }
    return 0;
}

static int default_kind(const JogSymbol *symbol) {
    const JogType *type;
    const char *name;
    const char *pkg;
    if (!symbol || !symbol->type) {
        return 0;
    }
    type = symbol->type;
    if (type->kind != JOG_TY_DISTINCT && type->kind != JOG_TY_INTERFACE) {
        return 0;
    }
    name = type->as.nominal.name;
    pkg = type->as.nominal.package;
    if (!name) {
        return 0;
    }
    if (type->kind == JOG_TY_DISTINCT && type->as.nominal.decl == NULL && strcmp(name, "Scope") == 0) {
        return DEF_SCOPE;
    }
    if (pkg && strcmp(pkg, "std.mem") == 0) {
        if (type->kind == JOG_TY_INTERFACE && strcmp(name, "Allocator") == 0) {
            return DEF_ALLOC;
        }
        if (type->kind == JOG_TY_DISTINCT && strcmp(name, "ErrorAllocator") == 0) {
            return DEF_ERR;
        }
    }
    if (pkg && strcmp(pkg, "std.io") == 0 && type->kind == JOG_TY_INTERFACE && strcmp(name, "Logger") == 0) {
        return DEF_LOG;
    }
    return 0;
}

static int immediate_base(const JogType *distinct, const JogType *base) {
    return distinct && base && distinct->kind == JOG_TY_DISTINCT && distinct->as.nominal.base == base;
}

static JirOperand bitcast_to(Lower *L, JirOperand value, JogType *type, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (!L || L->failed || L->terminated) {
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

static int32_t find_named_fn(const Lower *L, const char *name) {
    size_t i;
    if (!L || !L->module || !name) {
        return -1;
    }
    for (i = 0; i < L->module->funcs.count; i++) {
        JirFunc *fn = jir_func(L->module, (uint32_t)i);
        if (fn && fn->name && strcmp(fn->name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static JirOperand call_index(Lower *L, int32_t index, JogType *ret, JogPos pos) {
    uint32_t dst;
    JirInst *inst;
    if (index < 0 || !ret || ret->kind == JOG_TY_UNIT || ret->kind == JOG_TY_ERROR) {
        jog_lower_fail(L, pos, "missing runtime default");
        return jir_none();
    }
    if (!jog_lower_new_local(L, NULL, ret, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_CALL, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jir_func_ref(L->module, (uint32_t)index);
    if (!jir_set_args(L->module, inst, NULL, 0)) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    return jog_lower_local_op(L, dst);
}

static JogNode *pkg_fn(Lower *L, const char *path, const char *name, JogPos pos) {
    const char *interned;
    size_t i;
    if (!L->program || !L->program->intern) {
        jog_lower_fail(L, pos, "missing runtime default '%s'", name);
        return NULL;
    }
    interned = ct_intern_cstr(L->program->intern, name);
    if (!interned) {
        jog_lower_fail(L, pos, "out of memory");
        return NULL;
    }
    for (i = 0; i < L->program->package_count; i++) {
        JogPackage *pkg = L->program->packages[i];
        JogSymbol *sym;
        if (!pkg || !pkg->path || strcmp(pkg->path, path) != 0 || !pkg->scope) {
            continue;
        }
        sym = jog_package_scope_lookup(pkg->scope, interned);
        if (!sym || !sym->decl || sym->decl->kind != JOG_NODE_FN) {
            jog_lower_fail(L, pos, "missing runtime default '%s'", name);
            return NULL;
        }
        return sym->decl;
    }
    jog_lower_fail(L, pos, "missing runtime default '%s'", name);
    return NULL;
}

static JirOperand call_decl(Lower *L, JogNode *decl, JogPos pos) {
    int32_t index;
    JirFunc *func;
    JogType *ret;
    if (!decl || L->failed) {
        return jir_none();
    }
    index = jog_lower_find_fn(L, decl);
    if (index < 0) {
        jog_lower_fail(L, pos, "missing runtime default");
        return jir_none();
    }
    func = jir_func(L->module, (uint32_t)index);
    ret = func && func->type && func->type->kind == JOG_TY_FN ? func->type->as.fn.ret : NULL;
    return call_index(L, index, ret, pos);
}

static JirOperand lower_scope_root(Lower *L, JogType *scope, JogPos pos) {
    JogType *ret;
    JogType *fn_ty;
    int32_t index;
    JirOperand raw;
    if (!scope || scope->kind != JOG_TY_DISTINCT || !scope->as.nominal.base) {
        jog_lower_fail(L, pos, "missing runtime default Scope");
        return jir_none();
    }
    ret = scope->as.nominal.base;
    index = find_named_fn(L, "jogrt_scope_root");
    if (index < 0) {
        fn_ty = jog_type_fn(L->program->types, NULL, 0, ret, NULL, 0, false, false, true);
        if (!fn_ty) {
            jog_lower_fail(L, pos, "out of memory");
            return jir_none();
        }
        index = jir_func_add(L->module, "jogrt_scope_root", "jogrt_scope_root", JIR_LINK_EXTERN, fn_ty, NULL);
        if (index < 0) {
            jog_lower_fail(L, pos, "out of memory");
            return jir_none();
        }
    }
    raw = call_index(L, index, ret, pos);
    if (L->failed) {
        return jir_none();
    }
    return bitcast_to(L, raw, scope, pos);
}

static JirOperand materialize(Lower *L, int kind, JogType *type, JogPos pos) {
    JogNode *decl;
    JirOperand value;
    if (kind == DEF_SCOPE) {
        return lower_scope_root(L, type, pos);
    }
    if (kind == DEF_ALLOC) {
        decl = pkg_fn(L, "std.mem", "heap", pos);
        return call_decl(L, decl, pos);
    }
    if (kind == DEF_ERR) {
        decl = pkg_fn(L, "std.mem", "error_heap", pos);
        value = call_decl(L, decl, pos);
        if (L->failed) {
            return jir_none();
        }
        return bitcast_to(L, value, type, pos);
    }
    if (kind == DEF_LOG) {
        decl = pkg_fn(L, "std.io", "logger", pos);
        return call_decl(L, decl, pos);
    }
    jog_lower_fail(L, pos, "runtime default has no value");
    return jir_none();
}

bool jog_lower_context_default(Lower *L, JogNode *node, JogSymbol *symbol, JirOperand *out) {
    JogPos pos;
    const JogSymbol *saved;
    JirOperand value;
    int kind;
    uint32_t existing;
    if (out) {
        *out = jir_none();
    }
    if (!on_main_defaults(symbol)) {
        return false;
    }
    pos = node && node->pos.file ? node->pos : (symbol->decl ? symbol->decl->pos : (JogPos){0});
    if (!L || L->failed) {
        return true;
    }
    if (jog_lower_find_local(L, symbol, &existing)) {
        if (out) {
            *out = jog_lower_local_op(L, existing);
        }
        return true;
    }
    if (L->in_comptime) {
        jog_lower_fail(L, pos, "compile-time expression reads a runtime value");
        return true;
    }
    if (opening == symbol) {
        jog_lower_fail(L, pos, "runtime default depends on itself");
        return true;
    }
    kind = default_kind(symbol);
    if (kind == 0) {
        jog_lower_fail(L, pos, "runtime default has no value");
        return true;
    }
    saved = opening;
    opening = symbol;
    value = materialize(L, kind, symbol->type, pos);
    opening = saved;
    if (L->failed || value.kind != JIR_OPND_LOCAL) {
        if (!L->failed) {
            jog_lower_fail(L, pos, "runtime default is not a value");
        }
        return true;
    }
    if (!jog_lower_bind_local(L, symbol, value.as.local)) {
        return true;
    }
    if (out) {
        *out = value;
    }
    return true;
}

bool jog_lower_context_cast(Lower *L, JogNode *node, JirOperand *out) {
    JogType *dest;
    JogType *got;
    JirOperand src;
    if (out) {
        *out = jir_none();
    }
    if (!node || node->kind != JOG_NODE_CAST || node->as.cast.op != JOG_TOK_KW_AS) {
        return false;
    }
    dest = node->type;
    got = node->as.cast.expr ? node->as.cast.expr->type : NULL;
    if (!immediate_base(got, dest) && !immediate_base(dest, got)) {
        return false;
    }
    if (!L || L->failed) {
        return true;
    }
    src = jog_lower_expr(L, node->as.cast.expr);
    if (L->failed) {
        return true;
    }
    src = bitcast_to(L, src, dest, node->pos);
    if (out) {
        *out = src;
    }
    return true;
}

bool jog_lower_testing_finish(Lower *L, JogPos pos) {
    const char *interned;
    size_t i;
    int found_pkg = 0;
    JogNode *decl = NULL;
    int32_t index;
    JirFunc *func;
    JirInst *inst;
    JogType *ret;
    if (!L || L->failed) {
        return false;
    }
    if (!L->program || !L->program->intern || !L->program->packages) {
        return true;
    }
    interned = ct_intern_cstr(L->program->intern, "finish");
    if (!interned) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    for (i = 0; i < L->program->package_count; i++) {
        JogPackage *pkg = L->program->packages[i];
        JogSymbol *sym;
        if (!pkg || !pkg->path || strcmp(pkg->path, "std.testing") != 0) {
            continue;
        }
        found_pkg = 1;
        sym = pkg->scope ? jog_package_scope_lookup(pkg->scope, interned) : NULL;
        if (!sym || !sym->decl || sym->decl->kind != JOG_NODE_FN) {
            jog_lower_fail(L, pos, "missing test result");
            return false;
        }
        decl = sym->decl;
        break;
    }
    if (!found_pkg) {
        return true;
    }
    index = jog_lower_find_fn(L, decl);
    if (index < 0) {
        jog_lower_fail(L, pos, "missing test result");
        return false;
    }
    func = jir_func(L->module, (uint32_t)index);
    ret = func && func->type && func->type->kind == JOG_TY_FN ? func->type->as.fn.ret : NULL;
    if (!func || !func->type || func->type->kind != JOG_TY_FN || func->type->as.fn.param_count != 0 ||
        func->type->as.fn.context_count != 0 || !ret || ret->kind != JOG_TY_UNIT) {
        jog_lower_fail(L, pos, "test result has the wrong type");
        return false;
    }
    inst = jog_lower_emit(L, JIR_CALL, -1, pos);
    if (!inst) {
        return false;
    }
    inst->a = jir_func_ref(L->module, (uint32_t)index);
    if (!jir_set_args(L->module, inst, NULL, 0)) {
        jog_lower_fail(L, pos, "out of memory");
        return false;
    }
    return !L->failed;
}
