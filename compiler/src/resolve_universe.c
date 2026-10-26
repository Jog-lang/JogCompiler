#include "resolve_universe.h"

/* str is the const-bytes builtin. true and false share the bool builtin. */
static const struct {
    const char *name;
    JogSymbolKind kind;
    JogBuiltin which;
} k_universe[] = {
    {"i8", JOG_SYM_TYPE, JOG_BUILTIN_I8},
    {"i16", JOG_SYM_TYPE, JOG_BUILTIN_I16},
    {"i32", JOG_SYM_TYPE, JOG_BUILTIN_I32},
    {"i64", JOG_SYM_TYPE, JOG_BUILTIN_I64},
    {"u8", JOG_SYM_TYPE, JOG_BUILTIN_U8},
    {"u16", JOG_SYM_TYPE, JOG_BUILTIN_U16},
    {"u32", JOG_SYM_TYPE, JOG_BUILTIN_U32},
    {"u64", JOG_SYM_TYPE, JOG_BUILTIN_U64},
    {"isize", JOG_SYM_TYPE, JOG_BUILTIN_ISIZE},
    {"usize", JOG_SYM_TYPE, JOG_BUILTIN_USIZE},
    {"f32", JOG_SYM_TYPE, JOG_BUILTIN_F32},
    {"f64", JOG_SYM_TYPE, JOG_BUILTIN_F64},
    {"bool", JOG_SYM_TYPE, JOG_BUILTIN_BOOL},
    {"bytes", JOG_SYM_TYPE, JOG_BUILTIN_BYTES},
    {"str", JOG_SYM_TYPE, JOG_BUILTIN_CONST_BYTES},
    {"true", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"false", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"null", JOG_SYM_CONSTANT, JOG_BUILTIN_NULL},
};

/* error is not a JogBuiltin. It is the std interface named in error?,
   present before std is loaded. Methods arrive with interfaces. */
static bool bind_error(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope) {
    const char *name = ct_intern_cstr(intern, "error");
    JogType *type = jog_type_new_nominal(types, JOG_TY_INTERFACE, name, NULL, NULL);
    JogSymbol *symbol;
    if (!name || !type || !jog_type_layout(types, type)) {
        return false;
    }
    symbol = jog_symbol_new(arena, JOG_SYM_TYPE, name, NULL, type, NULL);
    if (!symbol) {
        return false;
    }
    return jog_package_scope_bind(scope, symbol);
}

/* void is not a JogBuiltin. It is an incomplete nominal type so *void can
   be named without becoming the unit type (). */
static bool bind_void(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope) {
    const char *name = ct_intern_cstr(intern, "void");
    JogType *type = jog_type_new_nominal(types, JOG_TY_STRUCT, name, NULL, NULL);
    JogSymbol *symbol;
    if (!name || !type) {
        return false;
    }
    symbol = jog_symbol_new(arena, JOG_SYM_TYPE, name, NULL, type, NULL);
    if (!symbol) {
        return false;
    }
    return jog_package_scope_bind(scope, symbol);
}

/* Scope, Chan, and Task are distinct *void. Chan and Task are markers;
   Chan<T> and Task<T> are built by the checker. ErrCancelled and ErrClosed
   are error values. */
static bool bind_distinct_ptr(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope,
                              const char *name, JogType *void_ty) {
    const char *interned = ct_intern_cstr(intern, name);
    JogType *base = jog_type_ptr(types, void_ty, false);
    JogType *type = jog_type_new_nominal(types, JOG_TY_DISTINCT, interned, NULL, NULL);
    JogSymbol *symbol;
    if (!interned || !base || !type || !jog_type_layout(types, base)) {
        return false;
    }
    type->as.nominal.base = base;
    if (!jog_type_layout(types, type)) {
        return false;
    }
    symbol = jog_symbol_new(arena, JOG_SYM_TYPE, interned, NULL, type, NULL);
    if (!symbol) {
        return false;
    }
    return jog_package_scope_bind(scope, symbol);
}

static bool bind_err_const(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope, const char *name) {
    const char *interned = ct_intern_cstr(intern, name);
    const char *err_name = ct_intern_cstr(intern, "error");
    JogSymbol *err = err_name ? jog_package_scope_lookup(scope, err_name) : NULL;
    JogSymbol *symbol;
    (void)types;
    if (!interned || !err || !err->type) {
        return false;
    }
    symbol = jog_symbol_new(arena, JOG_SYM_CONSTANT, interned, NULL, err->type, NULL);
    if (!symbol) {
        return false;
    }
    return jog_package_scope_bind(scope, symbol);
}

static bool bind_builtin(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope,
                         const char *name, JogSymbolKind kind, JogBuiltin which) {
    const char *interned = ct_intern_cstr(intern, name);
    JogType *type = jog_type_builtin(types, which);
    JogSymbol *symbol;
    if (!interned || !type) {
        return false;
    }
    symbol = jog_symbol_new(arena, kind, interned, NULL, type, NULL);
    if (!symbol) {
        return false;
    }
    return jog_package_scope_bind(scope, symbol);
}

JogPackageScope *jog_universe_scope(CtArena *arena, CtIntern *intern, JogTypes *types) {
    JogPackageScope *scope;
    size_t i;
    if (!arena || !intern || !types) {
        return NULL;
    }
    scope = jog_package_scope_new(arena, NULL);
    if (!scope) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_universe) / sizeof(k_universe[0]); i++) {
        if (!bind_builtin(arena, intern, types, scope, k_universe[i].name, k_universe[i].kind,
                          k_universe[i].which)) {
            return NULL;
        }
    }
    if (!bind_error(arena, intern, types, scope) || !bind_void(arena, intern, types, scope)) {
        return NULL;
    }
    {
        const char *void_name = ct_intern_cstr(intern, "void");
        JogSymbol *void_sym = void_name ? jog_package_scope_lookup(scope, void_name) : NULL;
        if (!void_sym || !void_sym->type ||
            !bind_distinct_ptr(arena, intern, types, scope, "Scope", void_sym->type) ||
            !bind_distinct_ptr(arena, intern, types, scope, "Chan", void_sym->type) ||
            !bind_distinct_ptr(arena, intern, types, scope, "Task", void_sym->type) ||
            !bind_err_const(arena, intern, types, scope, "ErrCancelled") ||
            !bind_err_const(arena, intern, types, scope, "ErrClosed")) {
            return NULL;
        }
    }
    return scope;
}
