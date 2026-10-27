#include "resolve_package.h"

#include "os_suffix.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Package and file scopes for one program. Types and bodies are left
   for later checker steps. Duplicate names are reported and skipped;
   the rest of the program is still bound. */

typedef struct Resolve {
    JogProgram *program;
    const JogPackageScope *universe;
    bool failed;
} Resolve;

static CtPos as_pos(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static JogPos no_pos(void) {
    JogPos pos;
    memset(&pos, 0, sizeof(pos));
    return pos;
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void fail(Resolve *r, JogPos pos, const char *fmt, ...) {
    char buf[512];
    va_list args;
    int wrote;
    r->failed = true;
    va_start(args, fmt);
    wrote = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (wrote < 0) {
        buf[0] = '\0';
    }
    ct_diag_emit(r->program->diag, CT_SEVERITY_ERROR, as_pos(pos), "%s", buf);
}

static void oom(Resolve *r) {
    CtPos pos = {0};
    r->failed = true;
    ct_diag_emit(r->program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static bool package_visible(JogVisibility vis) {
    return vis == JOG_VIS_PUBLIC || vis == JOG_VIS_PACKAGE;
}

static bool same_file(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static void duplicate(Resolve *r, JogPos pos, const char *name, const JogSymbol *prev) {
    fail(r, pos, "duplicate declaration of '%s'", name);
    if (prev && prev->decl) {
        ct_diag_emit(r->program->diag, CT_SEVERITY_NOTE, as_pos(prev->decl->pos), "previous declaration is here");
    }
}

static JogSymbol *file_prev(const JogPackageScope *scope, size_t file_index, const char *name) {
    JogSymbol *prev;
    if (!scope->file_scopes || file_index >= scope->file_scope_count || !scope->file_scopes[file_index]) {
        return NULL;
    }
    prev = jog_scope_map_lookup(scope->file_scopes[file_index]->bindings, name);
    if (prev) {
        return prev;
    }
    return jog_package_scope_lookup(scope, name);
}

static void bind_named(Resolve *r, JogPackage *pkg, size_t file_index, JogNode *node, const char *name,
                       JogSymbolKind kind, JogVisibility vis) {
    JogPackageScope *scope = pkg->scope;
    JogSymbol *symbol;
    JogSymbol *prev = NULL;
    bool in_package = package_visible(vis);
    if (is_discard(name)) {
        return;
    }
    symbol = jog_symbol_new(r->program->arena, kind, name, node, NULL, pkg);
    if (!symbol) {
        oom(r);
        return;
    }
    if (kind == JOG_SYM_METHOD) {
        if (!node->symbol) {
            node->symbol = symbol;
        }
        return;
    }
    if (in_package) {
        prev = jog_package_scope_lookup(scope, name);
    } else {
        prev = file_prev(scope, file_index, name);
    }
    if (prev) {
        duplicate(r, node->pos, name, prev);
        return;
    }
    if (in_package) {
        if (!jog_package_scope_bind(scope, symbol)) {
            oom(r);
            return;
        }
    } else if (!scope->file_scopes || file_index >= scope->file_scope_count || !scope->file_scopes[file_index] ||
               !jog_block_scope_bind(scope->file_scopes[file_index], symbol)) {
        oom(r);
        return;
    }
    if (!node->symbol) {
        node->symbol = symbol;
    }
}

static const char *join_path(Resolve *r, JogNames path, bool *out_of_memory) {
    size_t len = 0;
    size_t at = 0;
    size_t i;
    char *buf;
    const char *interned;
    *out_of_memory = false;
    if (path.count == 0 || !path.items) {
        return NULL;
    }
    if (path.count == 1) {
        return path.items[0];
    }
    for (i = 0; i < path.count; i++) {
        if (!path.items[i]) {
            return NULL;
        }
        len += strlen(path.items[i]) + 1;
    }
    buf = ct_arena_alloc(r->program->arena, len, 1);
    if (!buf) {
        *out_of_memory = true;
        return NULL;
    }
    for (i = 0; i < path.count; i++) {
        size_t n = strlen(path.items[i]);
        memcpy(buf + at, path.items[i], n);
        at += n;
        if (i + 1 < path.count) {
            buf[at++] = '.';
        }
    }
    buf[at] = '\0';
    interned = ct_intern(r->program->intern, buf, at);
    if (!interned) {
        *out_of_memory = true;
        return NULL;
    }
    return interned;
}

static bool is_prefixed(const char *canonical, const char *prefix, const char *path) {
    size_t n = strlen(prefix);
    return strncmp(canonical, prefix, n) == 0 && canonical[n] == '.' && strcmp(canonical + n + 1, path) == 0;
}

/* An import binds by canonical path. The written path is canonical in the
   root module; in a dependency its own packages are <module>.<path>. The
   loader recorded the target in pkg->imports either way. */
static JogPackage *find_package(const JogProgram *program, const JogPackage *from, const char *path) {
    size_t i;
    bool in_dep;
    if (!path) {
        return NULL;
    }
    in_dep = from && from->module_name && !from->is_std && program->module_name &&
             strcmp(from->module_name, program->module_name) != 0;
    if (from) {
        for (i = 0; i < from->import_count; i++) {
            JogPackage *pkg = from->imports[i];
            if (pkg && pkg->path && strcmp(pkg->path, path) == 0) {
                return pkg;
            }
        }
        for (i = 0; in_dep && i < from->import_count; i++) {
            JogPackage *pkg = from->imports[i];
            if (pkg && pkg->path && is_prefixed(pkg->path, from->module_name, path)) {
                return pkg;
            }
        }
    }
    if (in_dep) {
        return NULL;
    }
    for (i = 0; i < program->package_count; i++) {
        JogPackage *pkg = program->packages[i];
        if (pkg && pkg->path && strcmp(pkg->path, path) == 0) {
            return pkg;
        }
    }
    return NULL;
}

static void bind_import(Resolve *r, JogPackage *pkg, size_t file_index, JogNode *node) {
    bool failed_oom = false;
    const char *path = join_path(r, node->as.import.path, &failed_oom);
    const char *local;
    JogPackage *target;
    JogSymbol *prev;
    JogSymbol *symbol;
    JogPackageScope *scope = pkg->scope;
    if (failed_oom) {
        oom(r);
        return;
    }
    if (!path || path[0] == '\0') {
        fail(r, node->pos, "empty import path");
        return;
    }
    local = node->as.import.alias;
    if (!local || local[0] == '\0') {
        JogNames names = node->as.import.path;
        local = (names.count > 0 && names.items) ? names.items[names.count - 1] : NULL;
    }
    if (is_discard(local)) {
        fail(r, node->pos, "import has no name");
        return;
    }
    target = find_package(r->program, pkg, path);
    if (!target) {
        fail(r, node->pos, "cannot find package '%s'", path);
        return;
    }
    if (target == pkg) {
        fail(r, node->pos, "a package cannot import itself");
        return;
    }
    prev = file_prev(scope, file_index, local);
    if (prev) {
        duplicate(r, node->pos, local, prev);
        return;
    }
    symbol = jog_symbol_new(r->program->arena, JOG_SYM_PACKAGE, local, node, NULL, target);
    if (!symbol || !scope->file_scopes || file_index >= scope->file_scope_count || !scope->file_scopes[file_index] ||
        !jog_block_scope_bind(scope->file_scopes[file_index], symbol)) {
        oom(r);
        return;
    }
    node->symbol = symbol;
}

static void bind_decl(Resolve *r, JogPackage *pkg, size_t file_index, JogNode *node, bool package_pass);

static void bind_extern(Resolve *r, JogPackage *pkg, size_t file_index, JogNode *block, bool package_pass) {
    size_t i;
    if (package_visible(block->visibility) != package_pass) {
        return;
    }
    for (i = 0; i < block->as.extern_block.fns.count; i++) {
        JogNode *fn = list_at(block->as.extern_block.fns, i);
        if (!fn) {
            continue;
        }
        if (fn->kind != JOG_NODE_FN) {
            fail(r, fn->pos, "expected a function");
            continue;
        }
        /* The modifier is written on the block. The function node is what
           later visibility checks read. */
        fn->visibility = block->visibility;
        bind_decl(r, pkg, file_index, fn, package_pass);
    }
}

static const char *single_name(const JogNode *node) {
    switch (node->kind) {
    case JOG_NODE_FN:
        return node->as.fn.name;
    case JOG_NODE_TYPE_DECL:
        return node->as.type_decl.name;
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        return node->as.aggregate.name;
    default:
        return NULL;
    }
}

static void bind_decl(Resolve *r, JogPackage *pkg, size_t file_index, JogNode *node, bool package_pass) {
    size_t i;
    if (!node) {
        return;
    }
    switch (node->kind) {
    case JOG_NODE_IMPORT:
        if (!package_pass) {
            bind_import(r, pkg, file_index, node);
        }
        return;
    case JOG_NODE_EXTERN_BLOCK:
        bind_extern(r, pkg, file_index, node, package_pass);
        return;
    case JOG_NODE_FN:
        /* A free expect and its actual share one symbol, bound after both
           passes. The actual keeps the body; the expect keeps the visibility. */
        if ((node->as.fn.is_expect || node->as.fn.is_actual) && !node->as.fn.receiver) {
            return;
        }
        if (package_visible(node->visibility) != package_pass) {
            return;
        }
        bind_named(r, pkg, file_index, node, node->as.fn.name,
                   node->as.fn.receiver ? JOG_SYM_METHOD : JOG_SYM_FUNCTION, node->visibility);
        return;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
        if (package_visible(node->visibility) != package_pass) {
            return;
        }
        for (i = 0; i < node->as.binding.names.count; i++) {
            const char *name = (node->as.binding.names.items && i < node->as.binding.names.count)
                                   ? node->as.binding.names.items[i]
                                   : NULL;
            JogSymbolKind kind = JOG_SYM_VARIABLE;
            if (node->kind == JOG_NODE_VAL) {
                kind = JOG_SYM_CONSTANT;
            } else if (node->kind == JOG_NODE_CONTEXT) {
                kind = JOG_SYM_CONTEXT;
            }
            bind_named(r, pkg, file_index, node, name, kind, node->visibility);
        }
        return;
    case JOG_NODE_TYPE_DECL:
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        if (package_visible(node->visibility) != package_pass) {
            return;
        }
        bind_named(r, pkg, file_index, node, single_name(node), JOG_SYM_TYPE, node->visibility);
        return;
    default:
        if (package_pass) {
            fail(r, node->pos, "not a top-level declaration");
        }
        return;
    }
}

static void bind_file(Resolve *r, JogPackage *pkg, size_t file_index, bool package_pass) {
    JogSourceFile *source = pkg->files[file_index];
    JogNode *file;
    size_t i;
    if (!source || !source->ast) {
        return;
    }
    file = source->ast;
    if (file->kind != JOG_NODE_FILE) {
        if (package_pass) {
            fail(r, file->pos, "expected a file");
        }
        return;
    }
    if (!package_pass) {
        for (i = 0; i < file->as.file.imports.count; i++) {
            JogNode *import = list_at(file->as.file.imports, i);
            if (!import) {
                continue;
            }
            if (import->kind != JOG_NODE_IMPORT) {
                fail(r, import->pos, "expected an import");
                continue;
            }
            bind_import(r, pkg, file_index, import);
        }
    }
    for (i = 0; i < file->as.file.decls.count; i++) {
        bind_decl(r, pkg, file_index, list_at(file->as.file.decls, i), package_pass);
    }
}

static int source_matches_target(const JogSourceFile *source, const char *target) {
    const char *tag = NULL;
    size_t tag_len = 0;
    if (!source || !source->file.path) {
        return 0;
    }
    if (!jog_file_os_tag(jog_path_base(source->file.path), &tag, &tag_len, NULL)) {
        return 0;
    }
    return jog_os_tag_matches(tag, tag_len, target);
}

/* NULL when this target has no actual, or more than one. */
static JogNode *one_actual(JogPackage *pkg, const char *target, const char *name) {
    JogNode *found = NULL;
    size_t f;
    size_t d;
    if (!pkg->files || !name) {
        return NULL;
    }
    for (f = 0; f < pkg->file_count; f++) {
        JogSourceFile *source = pkg->files[f];
        JogNode *file;
        if (!source_matches_target(source, target) || !source->ast || source->ast->kind != JOG_NODE_FILE) {
            continue;
        }
        file = source->ast;
        for (d = 0; d < file->as.file.decls.count; d++) {
            JogNode *decl = list_at(file->as.file.decls, d);
            if (!decl || decl->kind != JOG_NODE_FN || !decl->as.fn.is_actual || decl->as.fn.receiver) {
                continue;
            }
            if (!decl->as.fn.name || strcmp(decl->as.fn.name, name) != 0) {
                continue;
            }
            if (found) {
                return NULL;
            }
            found = decl;
        }
    }
    return found;
}

static void bind_platform(Resolve *r, JogPackage *pkg) {
    const char *target = jog_target_os(r->program->options.ptr_size);
    size_t f;
    size_t d;
    if (!pkg->files) {
        return;
    }
    for (f = 0; f < pkg->file_count; f++) {
        JogSourceFile *source = pkg->files[f];
        JogNode *file;
        if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE || !source->file.path) {
            continue;
        }
        if (jog_file_os_tag(jog_path_base(source->file.path), NULL, NULL, NULL)) {
            continue;
        }
        file = source->ast;
        for (d = 0; d < file->as.file.decls.count; d++) {
            JogNode *expect = list_at(file->as.file.decls, d);
            JogNode *actual;
            if (!expect || expect->kind != JOG_NODE_FN || !expect->as.fn.is_expect || expect->as.fn.receiver) {
                continue;
            }
            if (!expect->as.fn.name || expect->as.fn.name[0] == '\0') {
                continue;
            }
            actual = one_actual(pkg, target, expect->as.fn.name);
            if (!actual) {
                continue;
            }
            actual->visibility = expect->visibility;
            bind_named(r, pkg, f, actual, actual->as.fn.name, JOG_SYM_FUNCTION, expect->visibility);
        }
    }
}

static void build_package(Resolve *r, JogPackage *pkg) {
    JogPackageScope *scope;
    size_t i;
    if (!pkg) {
        fail(r, no_pos(), "missing package");
        return;
    }
    scope = jog_package_scope_new(r->program->arena, pkg);
    if (!scope) {
        oom(r);
        return;
    }
    scope->universe = r->universe;
    pkg->scope = scope;
    if (pkg->file_count == 0) {
        return;
    }
    if (!pkg->files) {
        fail(r, no_pos(), "missing source files");
        return;
    }
    scope->file_scopes =
        ct_arena_alloc(r->program->arena, pkg->file_count * sizeof(JogBlockScope *), _Alignof(JogBlockScope *));
    if (!scope->file_scopes) {
        oom(r);
        return;
    }
    scope->file_scope_count = pkg->file_count;
    for (i = 0; i < pkg->file_count; i++) {
        scope->file_scopes[i] = jog_block_scope_new(r->program->arena, NULL, scope);
        if (!scope->file_scopes[i]) {
            oom(r);
            return;
        }
    }
    for (i = 0; i < pkg->file_count; i++) {
        bind_file(r, pkg, i, true);
    }
    for (i = 0; i < pkg->file_count; i++) {
        bind_file(r, pkg, i, false);
    }
    bind_platform(r, pkg);
}

bool jog_resolve_packages(JogProgram *program, const JogPackageScope *universe) {
    Resolve r;
    size_t i;
    if (!program || !program->arena || !program->diag || !program->intern) {
        return false;
    }
    if (program->package_count > 0 && !program->packages) {
        r.program = program;
        r.universe = universe;
        r.failed = false;
        fail(&r, no_pos(), "missing packages");
        return false;
    }
    r.program = program;
    r.universe = universe;
    r.failed = false;
    for (i = 0; i < program->package_count; i++) {
        build_package(&r, program->packages[i]);
    }
    return !r.failed;
}

JogSymbol *jog_file_lookup(const JogPackageScope *package_scope, size_t file_index, const char *name) {
    JogSymbol *found;
    if (!package_scope || !name || !package_scope->file_scopes || file_index >= package_scope->file_scope_count) {
        return NULL;
    }
    found = jog_block_scope_lookup(package_scope->file_scopes[file_index], name);
    if (found) {
        return found;
    }
    if (!package_scope->universe) {
        return NULL;
    }
    return jog_package_scope_lookup(package_scope->universe, name);
}

bool jog_symbol_visible_from(const JogSymbol *symbol, const JogPackage *from_package, const char *from_file) {
    JogVisibility vis;
    if (!symbol) {
        return false;
    }
    if (!symbol->decl) {
        return true;
    }
    if (symbol->kind == JOG_SYM_PACKAGE) {
        return same_file(symbol->decl->pos.file, from_file);
    }
    vis = symbol->decl->visibility;
    if (vis == JOG_VIS_PUBLIC) {
        return true;
    }
    if (symbol->package != from_package) {
        return false;
    }
    if (vis == JOG_VIS_PACKAGE) {
        return true;
    }
    return same_file(symbol->decl->pos.file, from_file);
}
