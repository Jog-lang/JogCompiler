#include "resolve_universe.h"

/* true and false share the bool builtin. Scalar names are not here;
   jog_bind_prelude reads them from core/scalar.jog. */
static const struct {
    const char *name;
    JogSymbolKind kind;
    JogBuiltin which;
} k_universe[] = {
    {"true", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"false", JOG_SYM_CONSTANT, JOG_BUILTIN_BOOL},
    {"null", JOG_SYM_CONSTANT, JOG_BUILTIN_NULL},
};

/* byte is the one-byte inline type. It is a universe name, not a keyword. */
static bool bind_byte(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope) {
    const char *name = ct_intern_cstr(intern, "byte");
    JogType *type = jog_type_bytes_n(types, 1);
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
    if (!bind_byte(arena, intern, types, scope)) {
        return NULL;
    }
    return scope;
}

int jog_bind_lang_items(CtArena *arena, CtIntern *intern, JogTypes *types, JogPackageScope *scope) {
    const char *void_name;
    JogSymbol *void_sym;
    if (!arena || !intern || !types || !scope) {
        return 0;
    }
    void_name = ct_intern_cstr(intern, "void");
    void_sym = void_name ? jog_package_scope_lookup(scope, void_name) : NULL;
    if (!void_sym || !void_sym->type || !bind_distinct_ptr(arena, intern, types, scope, "Scope", void_sym->type) ||
        !bind_distinct_ptr(arena, intern, types, scope, "Chan", void_sym->type) ||
        !bind_distinct_ptr(arena, intern, types, scope, "Task", void_sym->type) ||
        !bind_err_const(arena, intern, types, scope, "ErrCancelled") ||
        !bind_err_const(arena, intern, types, scope, "ErrClosed")) {
        return 0;
    }
    return 1;
}
