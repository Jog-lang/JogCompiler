#include "lower_package.h"

#include "lower_context.h"

#include <stdint.h>
#include <string.h>

/* Imported packages get their own init functions. The module init calls
   those functions in loader order, which is each package after the packages
   it imports, and then the root package's own init. A program whose runtime
   initializers are all in the root package keeps the single inlined init
   function in lower.c, so that dump does not change.
   jog test adds one internal function that calls every @test, then
   std.testing.finish when that package is loaded, and becomes the
   program entry. */

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
        if (!is_ident_char(c)) {
            c = '_';
        }
        ct_sb_putc(sb, c);
    }
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

static const char *dup_sb(Lower *L, CtSb *sb) {
    const char *text = NULL;
    if (sb && !sb->failed) {
        text = ct_arena_strdup(L->program->arena, ct_sb_cstr(sb));
    }
    ct_sb_free(sb);
    if (!text) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
    }
    return text;
}

static const char *make_c_name(Lower *L, const char *source, const char *suffix) {
    CtSb sb;
    ct_sb_init(&sb);
    ct_sb_append_cstr(&sb, "jog_");
    append_piece(&sb, source);
    ct_sb_append_cstr(&sb, suffix);
    return dup_sb(L, &sb);
}

static const char *make_jog_name(Lower *L, const char *source, const char *suffix) {
    CtSb sb;
    ct_sb_init(&sb);
    if (!source || source[0] == '\0') {
        ct_sb_append_cstr(&sb, "main");
    } else {
        ct_sb_append_cstr(&sb, source);
    }
    ct_sb_append_cstr(&sb, suffix);
    return dup_sb(L, &sb);
}

static int root_global(const Lower *L, const GlobalBind *bind) {
    JogPackage *owner = bind && bind->symbol ? bind->symbol->package : NULL;
    return !owner || owner == L->program->root;
}

static int needs_split(Lower *L) {
    size_t i;
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (bind && bind->runtime && !root_global(L, bind)) {
            return 1;
        }
    }
    return 0;
}

static int in_package(const Lower *L, const GlobalBind *bind, const JogPackage *pkg) {
    JogPackage *owner;
    if (!bind || !bind->runtime) {
        return 0;
    }
    owner = bind->symbol ? bind->symbol->package : NULL;
    if (pkg == L->program->root) {
        return !owner || owner == pkg;
    }
    return owner == pkg;
}

static int deps_done(const uint8_t *deps, const uint8_t *done, size_t n, size_t i) {
    size_t j;
    for (j = 0; j < n; j++) {
        if (deps[i * n + j] && !done[j]) {
            return 0;
        }
    }
    return 1;
}

static GlobalBind *bind_at_rt(Lower *L, const int32_t *rts, int32_t rt) {
    if (rt < 0) {
        return NULL;
    }
    return ct_da_at(&L->globals, (size_t)rts[rt]);
}

static int schedule_pkg(Lower *L, const int32_t *mine, size_t n, const uint8_t *deps, uint8_t *done, size_t count,
                        int32_t *order) {
    size_t cursor;
    size_t since;
    size_t step;
    if (n == 0) {
        return 1;
    }
    cursor = n - 1;
    since = 0;
    step = 0;
    while (step < n) {
        size_t at = (cursor + 1) % n;
        int32_t rt = mine[at];
        GlobalBind *stuck;
        cursor = at;
        if (rt < 0 || (size_t)rt >= count) {
            jog_lower_fail(L, (JogPos){0}, "global initializer cycle");
            return 0;
        }
        if (done[rt]) {
            since++;
            if (since > n) {
                jog_lower_fail(L, (JogPos){0}, "global initializer cycle");
                return 0;
            }
            continue;
        }
        if (!deps_done(deps, done, count, (size_t)rt)) {
            since++;
            if (since > n) {
                stuck = NULL;
                if (L->globals.count > 0) {
                    size_t g;
                    for (g = 0; g < L->globals.count; g++) {
                        GlobalBind *bind = ct_da_at(&L->globals, g);
                        if (bind && bind->rt == rt) {
                            stuck = bind;
                            break;
                        }
                    }
                }
                jog_lower_fail(L, stuck && stuck->decl ? stuck->decl->pos : (JogPos){0}, "global initializer cycle");
                return 0;
            }
            continue;
        }
        done[rt] = 1;
        order[step] = rt;
        step++;
        since = 0;
    }
    return 1;
}

static int open_unit(Lower *L, const char *c_name, const char *jog_name, JogPos pos, int32_t *index_out) {
    JogType *unit;
    JogType *fn_type;
    int32_t index;
    JirFunc *func;
    size_t before;
    uint32_t entry;
    if (!c_name || !jog_name) {
        return 0;
    }
    unit = jog_type_builtin(L->program->types, JOG_BUILTIN_UNIT);
    fn_type = unit ? jog_type_fn(L->program->types, NULL, 0, unit, NULL, 0, false, false, false) : NULL;
    if (!fn_type) {
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    index = jir_func_add(L->module, c_name, jog_name, JIR_LINK_INTERNAL, fn_type, NULL);
    func = index >= 0 ? jir_func(L->module, (uint32_t)index) : NULL;
    if (!func) {
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    func->pos = pos;
    L->func = func;
    L->ast_fn = NULL;
    L->locals.count = 0;
    L->loops.count = 0;
    L->cleanups.count = 0;
    L->overlay = NULL;
    L->overlay_count = 0;
    L->terminated = false;
    L->unchecked = false;
    L->in_cleanup = false;
    before = func->blocks.count;
    entry = jir_block_add(func, "entry");
    if (func->blocks.count == before) {
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    L->block = entry;
    L->terminated = false;
    *index_out = index;
    return 1;
}

static void close_unit(Lower *L, JogPos pos) {
    JirTerm term;
    if (L->failed) {
        return;
    }
    memset(&term, 0, sizeof term);
    term.kind = JIR_TERM_RET;
    term.value = jir_none();
    term.pos = pos;
    jir_terminate(L->func, L->block, term);
    L->terminated = true;
}

static int emit_call_func(Lower *L, int32_t index, const JirOperand *args, size_t arg_count, JogPos pos) {
    JirInst *inst;
    inst = jog_lower_emit(L, JIR_CALL, -1, pos);
    if (!inst) {
        return 0;
    }
    inst->a = jir_func_ref(L->module, (uint32_t)index);
    if (!jir_set_args(L->module, inst, args, arg_count)) {
        jog_lower_fail(L, pos, "out of memory");
        return 0;
    }
    return !L->failed;
}

static int emit_global_store(Lower *L, GlobalBind *bind) {
    JogNode *init;
    JirOperand value;
    JirInst *inst;
    JogPos pos;
    if (!bind || !bind->decl || bind->index < 0) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return 0;
    }
    init = bind->decl->as.binding.init;
    pos = init ? init->pos : bind->decl->pos;
    value = jog_lower_expr(L, init);
    if (L->failed) {
        return 0;
    }
    if (L->terminated) {
        jog_lower_fail(L, pos, "initializer does not produce a value");
        return 0;
    }
    inst = jog_lower_emit(L, JIR_STORE, -1, pos);
    if (!inst) {
        return 0;
    }
    inst->a = jir_global_ref(L->module, (uint32_t)bind->index);
    inst->b = value;
    return !L->failed;
}

static int ptr_like(const JogType *type) {
    JogType *under;
    JogType *elem;
    if (!type) {
        return 0;
    }
    under = jog_type_underlying((JogType *)type);
    if (!under) {
        return 0;
    }
    if (under->kind == JOG_TY_PTR) {
        return 1;
    }
    if (!jog_type_option_is_niche(under)) {
        return 0;
    }
    elem = jog_type_underlying(under->as.elem.elem);
    return elem && elem->kind == JOG_TY_PTR;
}

static int is_test_attr(const JogNode *attr) {
    return attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
           strcmp(attr->as.attribute.name, "test") == 0;
}

static int fn_is_test(const JogNode *fn) {
    size_t i;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return 0;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        JogNode *attr = fn->attrs.items ? fn->attrs.items[i] : NULL;
        if (is_test_attr(attr)) {
            return 1;
        }
    }
    return 0;
}

bool jog_lower_package_inits(Lower *L) {
    size_t count = 0;
    size_t i;
    size_t p;
    size_t filled;
    int32_t *rts = NULL;
    uint8_t *deps = NULL;
    uint8_t *done = NULL;
    int32_t *mine = NULL;
    int32_t *order = NULL;
    int32_t *funcs = NULL;
    const char *disp_c;
    const char *disp_jog;
    int32_t disp = -1;
    JogPos disp_pos = {0};
    int any = 0;
    if (!L || L->failed) {
        return true;
    }
    if (!needs_split(L)) {
        return false;
    }
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (!bind) {
            continue;
        }
        if (bind->runtime) {
            bind->rt = (int32_t)count;
            count++;
        } else {
            bind->rt = -1;
        }
    }
    if (count == 0 || count > SIZE_MAX / count || count > SIZE_MAX / sizeof(int32_t)) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return true;
    }
    rts = ct_arena_alloc(L->program->arena, count * sizeof(int32_t), _Alignof(int32_t));
    deps = ct_arena_alloc(L->program->arena, count * count, 1);
    done = ct_arena_alloc(L->program->arena, count, 1);
    mine = ct_arena_alloc(L->program->arena, count * sizeof(int32_t), _Alignof(int32_t));
    order = ct_arena_alloc(L->program->arena, count * sizeof(int32_t), _Alignof(int32_t));
    funcs = ct_arena_alloc(L->program->arena, (L->program->package_count + 1) * sizeof(int32_t), _Alignof(int32_t));
    if (!rts || !deps || !done || !mine || !order || !funcs) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return true;
    }
    memset(deps, 0, count * count);
    memset(done, 0, count);
    for (i = 0; i < L->program->package_count + 1; i++) {
        funcs[i] = -1;
    }
    count = 0;
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        if (!bind || !bind->runtime) {
            continue;
        }
        rts[count] = (int32_t)i;
        count++;
    }
    for (i = 0; i < count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, (size_t)rts[i]);
        if (bind && bind->decl && bind->decl->as.binding.init) {
            jog_lower_walk_init_deps(L, bind->decl->as.binding.init, bind->rt, deps + (size_t)bind->rt * count);
        }
    }
    if (L->failed) {
        return true;
    }
    filled = 0;
    for (p = 0; p < L->program->package_count && !L->failed; p++) {
        JogPackage *pkg = L->program->packages ? L->program->packages[p] : NULL;
        size_t n = 0;
        size_t k;
        const char *source;
        const char *c_name;
        const char *jog_name;
        const char *suffix_c;
        const char *suffix_jog;
        JogPos pos = {0};
        int32_t index = -1;
        if (!pkg) {
            continue;
        }
        for (k = 0; k < count; k++) {
            GlobalBind *bind = ct_da_at(&L->globals, (size_t)rts[k]);
            if (in_package(L, bind, pkg)) {
                mine[n++] = bind->rt;
            }
        }
        if (n == 0) {
            continue;
        }
        if (!schedule_pkg(L, mine, n, deps, done, count, order + filled)) {
            return true;
        }
        pos = (JogPos){0};
        {
            GlobalBind *first = bind_at_rt(L, rts, order[filled]);
            if (first && first->decl) {
                pos = first->decl->pos;
            }
        }
        source = pkg_source(L, pkg);
        if (pkg == L->program->root) {
            suffix_c = "__0rootinit";
            suffix_jog = ".rootinit";
        } else {
            suffix_c = "__0pkginit";
            suffix_jog = ".pkginit";
        }
        c_name = make_c_name(L, source, suffix_c);
        jog_name = make_jog_name(L, source, suffix_jog);
        if (!open_unit(L, c_name, jog_name, pos, &index)) {
            return true;
        }
        for (k = 0; k < n && !L->failed; k++) {
            if (!emit_global_store(L, bind_at_rt(L, rts, order[filled + k]))) {
                return true;
            }
        }
        close_unit(L, pos);
        funcs[p] = index;
        filled += n;
        any = 1;
        if (disp_pos.file == NULL) {
            disp_pos = pos;
        }
    }
    for (i = 0; i < count && !L->failed; i++) {
        if (!done[i]) {
            GlobalBind *stuck = bind_at_rt(L, rts, (int32_t)i);
            jog_lower_fail(L, stuck && stuck->decl ? stuck->decl->pos : (JogPos){0}, "global initializer cycle");
            return true;
        }
    }
    if (L->failed || !any) {
        return true;
    }
    disp_c = make_c_name(L, pkg_source(L, L->program->root), "__0init");
    disp_jog = make_jog_name(L, pkg_source(L, L->program->root), ".init");
    if (!open_unit(L, disp_c, disp_jog, disp_pos, &disp)) {
        return true;
    }
    for (p = 0; p < L->program->package_count && !L->failed; p++) {
        if (funcs[p] >= 0 && !emit_call_func(L, funcs[p], NULL, 0, disp_pos)) {
            return true;
        }
    }
    close_unit(L, disp_pos);
    if (!L->failed) {
        L->module->init_func = disp;
    }
    return true;
}

bool jog_lower_package_tests(Lower *L) {
    size_t p;
    size_t f;
    size_t d;
    size_t n = 0;
    size_t at = 0;
    int32_t *ids = NULL;
    JogNode **decls = NULL;
    JogPos pos = {0};
    const char *c_name;
    const char *jog_name;
    int32_t index = -1;
    if (!L || L->failed || !L->program) {
        return false;
    }
    for (p = 0; p < L->program->package_count; p++) {
        JogPackage *pkg = L->program->packages ? L->program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count; d++) {
                JogNode *decl = root->as.file.decls.items ? root->as.file.decls.items[d] : NULL;
                if (fn_is_test(decl)) {
                    n++;
                }
            }
        }
    }
    if (n == 0) {
        return true;
    }
    ids = ct_arena_alloc(L->program->arena, n * sizeof(int32_t), _Alignof(int32_t));
    decls = ct_arena_alloc(L->program->arena, n * sizeof(JogNode *), _Alignof(JogNode *));
    if (!ids || !decls) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return false;
    }
    for (p = 0; p < L->program->package_count; p++) {
        JogPackage *pkg = L->program->packages ? L->program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count; d++) {
                JogNode *decl = root->as.file.decls.items ? root->as.file.decls.items[d] : NULL;
                int32_t fn;
                if (!fn_is_test(decl)) {
                    continue;
                }
                fn = jog_lower_find_fn(L, decl);
                if (fn < 0) {
                    jog_lower_fail(L, decl->pos, "test function was not lowered");
                    return false;
                }
                if (pos.file == NULL) {
                    pos = decl->pos;
                }
                decls[at] = decl;
                ids[at] = fn;
                at++;
            }
        }
    }
    c_name = make_c_name(L, pkg_source(L, L->program->root), "__0test");
    jog_name = make_jog_name(L, pkg_source(L, L->program->root), ".test");
    if (!open_unit(L, c_name, jog_name, pos, &index)) {
        return false;
    }
    for (at = 0; at < n && !L->failed; at++) {
        JirOperand *args = NULL;
        size_t argc = 0;
        JogPos call_pos = decls[at] ? decls[at]->pos : pos;
        if (!jog_lower_context_supply(L, decls[at], &args, &argc)) {
            return false;
        }
        if (!emit_call_func(L, ids[at], args, argc, call_pos)) {
            return false;
        }
    }
    if (L->failed || !jog_lower_testing_finish(L, pos)) {
        return false;
    }
    close_unit(L, pos);
    if (!L->failed) {
        L->module->main_func = index;
    }
    return !L->failed;
}

bool jog_lower_package_cast(Lower *L, JogNode *node, JirOperand src, JirOperand *out) {
    uint32_t dst;
    JirInst *inst;
    if (!L || !node || !out || !node->type || !ptr_like(src.type) || !ptr_like(node->type)) {
        return false;
    }
    if (!jog_lower_new_local(L, NULL, node->type, &dst)) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_BITCAST, (int32_t)dst, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = src;
    *out = jog_lower_local_op(L, dst);
    return true;
}

bool jog_lower_package_view(Lower *L, JogNode *node, JirOperand *out) {
    JogNode *base;
    JogType *type;
    JirOperand value;
    uint32_t dst;
    JirInst *inst;
    uint32_t aux;
    if (!L || !node || !out || node->kind != JOG_NODE_MEMBER || !node->as.member.name || !node->type) {
        return false;
    }
    if (strcmp(node->as.member.name, "ptr") == 0) {
        aux = 0;
    } else if (strcmp(node->as.member.name, "len") == 0) {
        aux = 1;
    } else {
        return false;
    }
    base = node->as.member.base;
    if (!base || !base->type) {
        return false;
    }
    type = jog_type_underlying(base->type);
    if (!type || (type->kind != JOG_TY_BYTES && type->kind != JOG_TY_SLICE)) {
        return false;
    }
    value = jog_lower_expr(L, base);
    if (L->failed) {
        return true;
    }
    if (L->terminated) {
        jog_lower_fail(L, node->pos, "expression does not produce a value");
        return true;
    }
    if (!jog_lower_new_local(L, NULL, node->type, &dst)) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_EXTRACT, (int32_t)dst, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = value;
    inst->aux = aux;
    *out = jog_lower_local_op(L, dst);
    return true;
}

bool jog_lower_package_view_assign(Lower *L, JogNode *node) {
    JogNode *target;
    JogNode *base;
    JogType *type;
    JogType *field_ptr;
    JirOperand addr;
    JirOperand value;
    JirOperand slot;
    uint32_t dst;
    JirInst *inst;
    uint32_t aux;
    uint32_t local;
    if (!L || !node || node->kind != JOG_NODE_ASSIGN || L->failed) {
        return false;
    }
    target = node->as.assign.target;
    if (!target || target->kind != JOG_NODE_MEMBER || !target->as.member.name || !target->type) {
        return false;
    }
    if (strcmp(target->as.member.name, "ptr") == 0) {
        aux = 0;
    } else if (strcmp(target->as.member.name, "len") == 0) {
        aux = 1;
    } else {
        return false;
    }
    base = target->as.member.base;
    if (!base || !base->type) {
        return false;
    }
    type = jog_type_underlying(base->type);
    if (!type || (type->kind != JOG_TY_BYTES && type->kind != JOG_TY_SLICE)) {
        return false;
    }
    if (node->as.assign.op != JOG_TOK_ASSIGN) {
        jog_lower_fail(L, node->pos, "cannot assign to field '%s'", target->as.member.name);
        return true;
    }
    value = jog_lower_expr(L, node->as.assign.value);
    if (L->failed) {
        return true;
    }
    if (base->kind == JOG_NODE_IDENT && base->symbol && jog_lower_find_local(L, base->symbol, &local)) {
        JogType *ptr_ty = jog_type_ptr(L->program->types, base->type, false);
        if (!ptr_ty || !jog_lower_new_local(L, NULL, ptr_ty, &dst)) {
            return true;
        }
        inst = jog_lower_emit(L, JIR_ADDR, (int32_t)dst, node->pos);
        if (!inst) {
            return true;
        }
        inst->a = jog_lower_local_op(L, local);
        addr = jog_lower_local_op(L, dst);
    } else if (base->kind == JOG_NODE_IDENT && base->symbol) {
        addr = jog_lower_global_addr(L, base->symbol);
        if (addr.kind == JIR_OPND_NONE) {
            jog_lower_fail(L, node->pos, "cannot assign to field '%s'", target->as.member.name);
            return true;
        }
    } else {
        jog_lower_fail(L, node->pos, "cannot assign to field '%s'", target->as.member.name);
        return true;
    }
    field_ptr = jog_type_ptr(L->program->types, target->type, false);
    if (!field_ptr || !jog_lower_new_local(L, NULL, field_ptr, &dst)) {
        return true;
    }
    inst = jog_lower_emit(L, JIR_FIELD_ADDR, (int32_t)dst, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = addr;
    inst->aux = aux;
    slot = jog_lower_local_op(L, dst);
    inst = jog_lower_emit(L, JIR_STORE, -1, node->pos);
    if (!inst) {
        return true;
    }
    inst->a = slot;
    inst->b = value;
    return true;
}
