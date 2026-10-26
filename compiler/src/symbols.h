#ifndef JOG_SYMBOLS_H
#define JOG_SYMBOLS_H

#include "ast.h"
#include "program.h"
#include "types.h"

#include "arena.h"
#include "map.h"

#include <stdbool.h>
#include <string.h>

/* Bindings the checker creates and lowering reads. JogNode.symbol points
   here. kind, decl, type, and package are the fields lowering needs; name
   is the interned key used in scope maps. There is no symbols.c: every
   function is inline so lowering can include this header on its own.

   A package scope is one CtMap from interned name to JogSymbol *. Block
   scopes are a parent chain of the same maps. Lookup walks the blocks
   from the inside out, then the package scope. The checker stores the
   JogPackageScope * in JogPackage.scope (void * in program.h).

   file_scopes is one block scope per source file, in package->files
   order, for imports and file-private top-level names. Those blocks
   fall through to bindings. universe is the builtin scope after that;
   jog_package_scope_lookup does not walk it. Both are NULL until the
   package resolver fills them. */

typedef enum JogSymbolKind {
    JOG_SYM_PACKAGE = 0,
    JOG_SYM_TYPE,
    JOG_SYM_FUNCTION,
    JOG_SYM_VARIABLE,
    JOG_SYM_CONSTANT,
    JOG_SYM_FIELD,
    JOG_SYM_METHOD,
    JOG_SYM_VARIANT,
    JOG_SYM_PARAMETER,
    JOG_SYM_CONTEXT,
    JOG_SYM_COUNT
} JogSymbolKind;

/* One monomorphized copy of a generic function. args are the solved type
   arguments (they may still name an outer type parameter until lowering
   substitutes them). jir_index is -1 until the C name is declared. */
typedef struct JogGenericInst {
    JogType **args;
    size_t arg_count;
    JogType *fn_type;
    JogNode *fn;
    int32_t jir_index;
    bool body_done;
} JogGenericInst;

struct JogSymbol {
    JogSymbolKind kind;
    const char *name;    /* interned; NULL until the symbol is named */
    JogNode *decl;       /* declaring node; NULL when there is none */
    JogType *type;       /* NULL until the checker knows it */
    JogPackage *package; /* owning package; NULL when the symbol has none */
    /* Generic function, struct, enum, or alias. type_params are
       JOG_TY_TYPE_PARAM, in order. generic_insts are the function instances
       lowering emits. nominal_insts are monomorphized struct and enum types,
       including open ones whose arguments still name a type parameter. A
       generic alias stores its unsubstituted target in type; that target is
       not a value and is not a nominal instance. Unused lists stay NULL. */
    JogType **type_params;
    size_t type_param_count;
    JogGenericInst **generic_insts;
    size_t generic_inst_count;
    size_t generic_inst_cap;
    JogType **nominal_insts;
    size_t nominal_inst_count;
    size_t nominal_inst_cap;
    /* An imported package name that a file actually used. The checker
       reports an import whose symbol stays false. Other kinds ignore it. */
    bool referenced;
};

typedef struct JogBlockScope JogBlockScope;

typedef struct JogPackageScope {
    JogPackage *package;
    CtMap *bindings; /* interned name -> JogSymbol * */
    JogBlockScope **file_scopes; /* parallel to package->files; NULL if none */
    size_t file_scope_count;
    const struct JogPackageScope *universe; /* not owned; jog_package_scope_lookup ignores it */
} JogPackageScope;

typedef struct JogBlockScope {
    CtMap *bindings;              /* interned name -> JogSymbol * */
    struct JogBlockScope *parent; /* NULL at the outermost block */
    JogPackageScope *package;     /* fallthrough after the block chain */
} JogBlockScope;

/* "package", "type", ... "?" when kind is out of range. */
static inline const char *jog_symbol_kind_name(JogSymbolKind kind) {
    switch (kind) {
    case JOG_SYM_PACKAGE:
        return "package";
    case JOG_SYM_TYPE:
        return "type";
    case JOG_SYM_FUNCTION:
        return "function";
    case JOG_SYM_VARIABLE:
        return "variable";
    case JOG_SYM_CONSTANT:
        return "constant";
    case JOG_SYM_FIELD:
        return "field";
    case JOG_SYM_METHOD:
        return "method";
    case JOG_SYM_VARIANT:
        return "variant";
    case JOG_SYM_PARAMETER:
        return "parameter";
    case JOG_SYM_CONTEXT:
        return "context";
    case JOG_SYM_COUNT:
        break;
    }
    return "?";
}

/* NULL if arena is NULL or the arena is out of memory. name, decl, type,
   and package may be NULL. */
static inline JogSymbol *jog_symbol_new(CtArena *arena, JogSymbolKind kind, const char *name,
                                        JogNode *decl, JogType *type, JogPackage *package) {
    JogSymbol *symbol;
    if (!arena) {
        return NULL;
    }
    symbol = ct_arena_alloc(arena, sizeof(*symbol), _Alignof(JogSymbol));
    if (!symbol) {
        return NULL;
    }
    memset(symbol, 0, sizeof(*symbol));
    symbol->kind = kind;
    symbol->name = name;
    symbol->decl = decl;
    symbol->type = type;
    symbol->package = package;
    return symbol;
}

static inline JogSymbol *jog_scope_map_lookup(const CtMap *bindings, const char *name) {
    void *value = NULL;
    if (!bindings || !name || !ct_map_get(bindings, name, &value)) {
        return NULL;
    }
    return value;
}

/* Binds symbol under symbol->name. False if any argument is NULL, the
   symbol has no name, the name is already bound in this map, or the arena
   is out of memory. Does not replace an existing binding. */
static inline bool jog_scope_map_bind(CtMap *bindings, JogSymbol *symbol) {
    if (!bindings || !symbol || !symbol->name) {
        return false;
    }
    if (jog_scope_map_lookup(bindings, symbol->name)) {
        return false;
    }
    return ct_map_put(bindings, symbol->name, symbol);
}

/* NULL if arena is NULL or the arena is out of memory. package may be NULL. */
static inline JogPackageScope *jog_package_scope_new(CtArena *arena, JogPackage *package) {
    JogPackageScope *scope;
    if (!arena) {
        return NULL;
    }
    scope = ct_arena_alloc(arena, sizeof(*scope), _Alignof(JogPackageScope));
    if (!scope) {
        return NULL;
    }
    memset(scope, 0, sizeof(*scope));
    scope->package = package;
    scope->bindings = ct_map_create(arena);
    if (!scope->bindings) {
        return NULL;
    }
    return scope;
}

/* NULL if arena is NULL or the arena is out of memory. parent NULL starts
   a chain that falls through to package. A nested block takes its package
   scope from parent, and the package argument is ignored. */
static inline JogBlockScope *jog_block_scope_new(CtArena *arena, JogBlockScope *parent,
                                                 JogPackageScope *package) {
    JogBlockScope *scope;
    if (!arena) {
        return NULL;
    }
    scope = ct_arena_alloc(arena, sizeof(*scope), _Alignof(JogBlockScope));
    if (!scope) {
        return NULL;
    }
    memset(scope, 0, sizeof(*scope));
    scope->bindings = ct_map_create(arena);
    if (!scope->bindings) {
        return NULL;
    }
    scope->parent = parent;
    scope->package = parent ? parent->package : package;
    return scope;
}

static inline bool jog_package_scope_bind(JogPackageScope *scope, JogSymbol *symbol) {
    if (!scope) {
        return false;
    }
    return jog_scope_map_bind(scope->bindings, symbol);
}

static inline bool jog_block_scope_bind(JogBlockScope *scope, JogSymbol *symbol) {
    if (!scope) {
        return false;
    }
    return jog_scope_map_bind(scope->bindings, symbol);
}

/* NULL if scope or name is NULL, or the name is not bound. */
static inline JogSymbol *jog_package_scope_lookup(const JogPackageScope *scope, const char *name) {
    if (!scope) {
        return NULL;
    }
    return jog_scope_map_lookup(scope->bindings, name);
}

/* The innermost binding wins. After the block chain, lookup continues in
   the package scope. NULL if scope or name is NULL, or the name is absent. */
static inline JogSymbol *jog_block_scope_lookup(const JogBlockScope *scope, const char *name) {
    const JogBlockScope *block = scope;
    if (!scope || !name) {
        return NULL;
    }
    while (block) {
        JogSymbol *found = jog_scope_map_lookup(block->bindings, name);
        if (found) {
            return found;
        }
        if (!block->parent) {
            return jog_package_scope_lookup(block->package, name);
        }
        block = block->parent;
    }
    return NULL;
}

#endif
