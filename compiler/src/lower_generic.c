#include "lower_generic.h"

#include "check_generic.h"
#include "lower_struct.h"

#include "str.h"

#include <stdio.h>
#include <string.h>

/* One JIR function per concrete type-argument list. The C name is
   jog_<pkg>__<name>__<arg> with an extra __ between arguments.
   Spellings: i32, bool, str, ptr_, cptr_, slice_, cslice_, arrN_, opt_,
   tup_. A generic struct argument keeps its arguments, as in Box_i32.
   A generic template is not emitted. A struct template is not emitted. */

bool jog_lower_type_is_open(const JogType *type) {
    return jog_generic_has_param(type);
}

bool jog_lower_is_alias_template(const JogNode *decl) {
    return decl && decl->kind == JOG_NODE_TYPE_DECL && decl->as.type_decl.type_params.count > 0;
}

JogType *jog_lower_type(Lower *L, JogType *type) {
    if (!L || !type || L->subst_n == 0 || !L->subst_from || !L->subst_to) {
        return type;
    }
    return jog_generic_subst(L->program->types, L->program->arena, type, L->subst_from, L->subst_to, L->subst_n);
}

static int same_args(const JogGenericInst *inst, JogType *const *args, size_t n) {
    size_t i;
    if (!inst || inst->arg_count != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!inst->args || inst->args[i] != args[i]) {
            return 0;
        }
    }
    return 1;
}

static JogGenericInst *find_inst(JogSymbol *sym, JogType *const *args, size_t n) {
    size_t i;
    if (!sym) {
        return NULL;
    }
    for (i = 0; i < sym->generic_inst_count; i++) {
        JogGenericInst *inst = sym->generic_insts ? sym->generic_insts[i] : NULL;
        if (same_args(inst, args, n)) {
            return inst;
        }
    }
    return NULL;
}

static const char *pkg_source(const Lower *L, const JogPackage *pkg) {
    if (pkg && pkg->path && pkg->path[0] != '\0') {
        return pkg->path;
    }
    if (pkg && pkg->name && pkg->name[0] != '\0') {
        return pkg->name;
    }
    if (L->program->module_name && L->program->module_name[0] != '\0') {
        return L->program->module_name;
    }
    return "main";
}

static int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void append_piece(CtSb *sb, const char *src) {
    size_t i;
    if (!src || src[0] == '\0') {
        ct_sb_append_cstr(sb, "main");
        return;
    }
    for (i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '.') {
            c = '_';
        } else if (!is_ident_char(c)) {
            c = '_';
        }
        ct_sb_putc(sb, c);
    }
}

static void mangle_type(Lower *L, CtSb *sb, const JogType *type) {
    size_t i;
    if (!type) {
        ct_sb_append_cstr(sb, "unknown");
        return;
    }
    switch (type->kind) {
    case JOG_TY_BOOL:
        ct_sb_append_cstr(sb, "bool");
        return;
    case JOG_TY_INT:
        if (type->as.integer.is_size) {
            ct_sb_append_cstr(sb, type->as.integer.is_signed ? "isize" : "usize");
        } else {
            ct_sb_appendf(sb, "%c%u", type->as.integer.is_signed ? 'i' : 'u', (unsigned)type->as.integer.bits);
        }
        return;
    case JOG_TY_FLOAT:
        ct_sb_appendf(sb, "f%u", (unsigned)type->as.flt.bits);
        return;
    case JOG_TY_UNIT:
        ct_sb_append_cstr(sb, "unit");
        return;
    case JOG_TY_BYTES:
        ct_sb_append_cstr(sb, type->is_const ? "cbytes" : "bytes");
        return;
    case JOG_TY_BYTES_N:
        ct_sb_appendf(sb, "bytes%u", (unsigned)type->as.bytes_n.n);
        return;
    case JOG_TY_PTR:
        ct_sb_append_cstr(sb, type->is_const ? "cptr_" : "ptr_");
        mangle_type(L, sb, type->as.elem.elem);
        return;
    case JOG_TY_SLICE:
        ct_sb_append_cstr(sb, type->is_const ? "cslice_" : "slice_");
        mangle_type(L, sb, type->as.elem.elem);
        return;
    case JOG_TY_ARRAY:
        ct_sb_appendf(sb, "arr%u_", (unsigned)type->as.array.len);
        mangle_type(L, sb, type->as.array.elem);
        return;
    case JOG_TY_OPTION:
        ct_sb_append_cstr(sb, "opt_");
        mangle_type(L, sb, type->as.elem.elem);
        return;
    case JOG_TY_TUPLE:
        ct_sb_append_cstr(sb, "tup");
        for (i = 0; i < type->as.tuple.count; i++) {
            ct_sb_putc(sb, '_');
            mangle_type(L, sb, type->as.tuple.elems ? type->as.tuple.elems[i] : NULL);
        }
        return;
    case JOG_TY_STRUCT:
    case JOG_TY_ENUM:
    case JOG_TY_INTERFACE:
    case JOG_TY_DISTINCT:
        if (type->as.nominal.package && type->as.nominal.package[0] != '\0') {
            append_piece(sb, type->as.nominal.package);
            ct_sb_putc(sb, '_');
        }
        append_piece(sb, type->as.nominal.name ? type->as.nominal.name : "Type");
        /* A generic struct used as a type argument has to keep its
           arguments, or Box<i32> and Box<bool> would share one C name. */
        for (i = 0; i < type->as.nominal.type_arg_count; i++) {
            ct_sb_putc(sb, '_');
            mangle_type(L, sb, type->as.nominal.type_args ? type->as.nominal.type_args[i] : NULL);
        }
        return;
    default:
        ct_sb_appendf(sb, "t%u", (unsigned)type->id);
        return;
    }
}

static const char *dup_sb(Lower *L, CtSb *sb) {
    const char *text = NULL;
    if (!sb->failed) {
        text = ct_arena_strdup(L->program->arena, ct_sb_cstr(sb));
    }
    ct_sb_free(sb);
    if (!text) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
    }
    return text;
}

static JogType *flatten_fn(Lower *L, JogType *fn) {
    size_t n;
    size_t i;
    JogType **params;
    if (!fn || fn->kind != JOG_TY_FN || fn->as.fn.context_count == 0) {
        return fn;
    }
    n = fn->as.fn.param_count + fn->as.fn.context_count;
    params = ct_arena_alloc(L->program->arena, n * sizeof(*params), _Alignof(JogType *));
    if (!params) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return NULL;
    }
    for (i = 0; i < fn->as.fn.param_count; i++) {
        params[i] = fn->as.fn.params ? fn->as.fn.params[i] : NULL;
    }
    for (i = 0; i < fn->as.fn.context_count; i++) {
        params[fn->as.fn.param_count + i] = fn->as.fn.contexts ? fn->as.fn.contexts[i] : NULL;
    }
    return jog_type_fn(L->program->types, params, n, fn->as.fn.ret, NULL, 0, fn->as.fn.variadic, fn->as.fn.escaping,
                       fn->as.fn.is_extern);
}

static const char *mangle_instance(Lower *L, JogNode *fn, JogType *const *args, size_t n) {
    CtSb sb;
    const JogPackage *pkg;
    const char *piece;
    size_t i;
    ct_sb_init(&sb);
    pkg = fn->symbol ? fn->symbol->package : NULL;
    ct_sb_append_cstr(&sb, "jog_");
    append_piece(&sb, pkg_source(L, pkg));
    ct_sb_append_cstr(&sb, "__");
    if (fn->as.fn.receiver && fn->as.fn.receiver->type) {
        JogType *recv = fn->as.fn.receiver->type;
        if (recv->kind == JOG_TY_PTR && recv->as.elem.elem) {
            recv = recv->as.elem.elem;
        }
        if (recv->kind == JOG_TY_STRUCT || recv->kind == JOG_TY_ENUM || recv->kind == JOG_TY_INTERFACE ||
            recv->kind == JOG_TY_DISTINCT) {
            piece = recv->as.nominal.name;
        } else {
            piece = NULL;
        }
        append_piece(&sb, piece);
        ct_sb_append_cstr(&sb, "__");
    }
    append_piece(&sb, fn->as.fn.name ? fn->as.fn.name : "fn");
    for (i = 0; i < n; i++) {
        ct_sb_append_cstr(&sb, "__");
        mangle_type(L, &sb, args ? args[i] : NULL);
    }
    return dup_sb(L, &sb);
}

static const char *jog_instance_name(Lower *L, JogNode *fn, JogType *const *args, size_t n) {
    CtSb sb;
    const JogPackage *pkg;
    size_t i;
    ct_sb_init(&sb);
    pkg = fn->symbol ? fn->symbol->package : NULL;
    ct_sb_append_cstr(&sb, pkg_source(L, pkg));
    ct_sb_putc(&sb, '.');
    if (fn->as.fn.receiver && fn->as.fn.receiver->type) {
        JogType *recv = fn->as.fn.receiver->type;
        if (recv->kind == JOG_TY_PTR && recv->as.elem.elem) {
            recv = recv->as.elem.elem;
        }
        if ((recv->kind == JOG_TY_STRUCT || recv->kind == JOG_TY_ENUM || recv->kind == JOG_TY_INTERFACE ||
             recv->kind == JOG_TY_DISTINCT) &&
            recv->as.nominal.name) {
            ct_sb_append_cstr(&sb, recv->as.nominal.name);
        } else {
            ct_sb_append_cstr(&sb, "Type");
        }
        ct_sb_putc(&sb, '.');
    }
    ct_sb_append_cstr(&sb, fn->as.fn.name ? fn->as.fn.name : "fn");
    ct_sb_putc(&sb, '<');
    for (i = 0; i < n; i++) {
        if (i > 0) {
            ct_sb_append_cstr(&sb, ", ");
        }
        jog_type_print(args ? args[i] : NULL, &sb);
    }
    ct_sb_putc(&sb, '>');
    return dup_sb(L, &sb);
}

static JirLinkage linkage_of(JogNode *fn) {
    if (fn->visibility == JOG_VIS_PUBLIC) {
        return JIR_LINK_PUBLIC;
    }
    return JIR_LINK_INTERNAL;
}

static int declare_inst(Lower *L, JogGenericInst *inst) {
    JogNode *fn;
    JogType *type;
    const char *name;
    const char *jog_name;
    const char **param_names;
    size_t nparam;
    size_t i;
    int32_t index;
    JirFunc *func;
    fn = inst->fn;
    type = flatten_fn(L, inst->fn_type);
    if (!type || L->failed) {
        return 0;
    }
    name = mangle_instance(L, fn, inst->args, inst->arg_count);
    jog_name = jog_instance_name(L, fn, inst->args, inst->arg_count);
    if (!name || !jog_name) {
        return 0;
    }
    nparam = type->as.fn.param_count;
    param_names = NULL;
    if (nparam > 0) {
        param_names = ct_arena_alloc(L->program->arena, nparam * sizeof(*param_names), _Alignof(const char *));
        if (!param_names) {
            jog_lower_fail(L, fn->pos, "out of memory");
            return 0;
        }
        memset(param_names, 0, nparam * sizeof(*param_names));
        for (i = 0; i < nparam; i++) {
            param_names[i] = jog_lower_param_name(fn, i);
        }
    }
    index = jir_func_add(L->module, name, jog_name, linkage_of(fn), type, param_names);
    if (index < 0) {
        jog_lower_fail(L, fn->pos, "out of memory");
        return 0;
    }
    func = jir_func(L->module, (uint32_t)index);
    if (func) {
        func->pos = fn->pos;
    }
    inst->jir_index = index;
    return 1;
}

static JogGenericInst *add_inst(Lower *L, JogSymbol *sym, JogNode *fn, JogType **args, size_t n, JogType *fn_type) {
    JogGenericInst *inst;
    JogGenericInst **grown;
    size_t cap;
    if (sym->generic_inst_count == sym->generic_inst_cap) {
        cap = sym->generic_inst_cap ? sym->generic_inst_cap * 2 : 4;
        grown = ct_arena_alloc(L->program->arena, cap * sizeof(*grown), _Alignof(JogGenericInst *));
        if (!grown) {
            jog_lower_fail(L, fn->pos, "out of memory");
            return NULL;
        }
        if (sym->generic_inst_count > 0 && sym->generic_insts) {
            memcpy(grown, sym->generic_insts, sym->generic_inst_count * sizeof(*grown));
        }
        sym->generic_insts = grown;
        sym->generic_inst_cap = cap;
    }
    inst = ct_arena_alloc(L->program->arena, sizeof(*inst), _Alignof(JogGenericInst));
    if (!inst) {
        jog_lower_fail(L, fn->pos, "out of memory");
        return NULL;
    }
    memset(inst, 0, sizeof(*inst));
    inst->args = args;
    inst->arg_count = n;
    inst->fn_type = fn_type;
    inst->fn = fn;
    inst->jir_index = -1;
    sym->generic_insts[sym->generic_inst_count++] = inst;
    if (!declare_inst(L, inst)) {
        return NULL;
    }
    if (!ct_da_push(&L->generic_pending, &inst)) {
        jog_lower_fail(L, fn->pos, "out of memory");
        return NULL;
    }
    return inst;
}

int32_t jog_lower_generic_callee(Lower *L, JogNode *call) {
    JogNode *callee;
    JogSymbol *sym;
    JogNode *fn;
    size_t n;
    size_t i;
    JogType **args;
    JogType *sig;
    JogGenericInst *inst;
    if (!L || !call || L->failed) {
        return -1;
    }
    callee = call->as.call.callee;
    sym = callee ? callee->symbol : call->symbol;
    fn = sym && sym->decl && sym->decl->kind == JOG_NODE_FN ? sym->decl : NULL;
    n = call->solved_type_arg_count;
    if (!sym || !fn || n == 0 || !call->solved_type_args) {
        jog_lower_fail(L, call->pos, "missing generic instance");
        return -1;
    }
    args = ct_arena_alloc(L->program->arena, n * sizeof(*args), _Alignof(JogType *));
    if (!args) {
        jog_lower_fail(L, call->pos, "out of memory");
        return -1;
    }
    for (i = 0; i < n; i++) {
        args[i] = jog_lower_type(L, call->solved_type_args[i]);
        if (!args[i] || jog_lower_type_is_open(args[i])) {
            jog_lower_fail(L, call->pos, "generic type was not instantiated");
            return -1;
        }
    }
    sig = callee && callee->type ? jog_lower_type(L, callee->type) : NULL;
    if (!sig || sig->kind != JOG_TY_FN || jog_lower_type_is_open(sig)) {
        jog_lower_fail(L, call->pos, "generic type was not instantiated");
        return -1;
    }
    inst = find_inst(sym, args, n);
    if (!inst) {
        inst = add_inst(L, sym, fn, args, n, sig);
        if (!inst) {
            return -1;
        }
    }
    return inst->jir_index;
}

bool jog_lower_generic_take(Lower *L, JogNode **fn, int32_t *jir_index) {
    size_t i;
    if (!L || !fn || !jir_index || L->failed) {
        return false;
    }
    for (i = 0; i < L->generic_pending.count; i++) {
        JogGenericInst **slot = ct_da_at(&L->generic_pending, i);
        JogGenericInst *inst = slot ? *slot : NULL;
        JogSymbol *sym;
        if (!inst || inst->body_done || !inst->fn || !inst->fn->symbol) {
            continue;
        }
        inst->body_done = true;
        sym = inst->fn->symbol;
        L->subst_from = sym->type_params;
        L->subst_to = inst->args;
        L->subst_n = inst->arg_count;
        *fn = inst->fn;
        *jir_index = inst->jir_index;
        return true;
    }
    return false;
}
