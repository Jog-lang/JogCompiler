#include "check_context.h"

#include "check_interface.h"
#include "resolve_package.h"

#include "coro.h"
#include "da.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

/* Contexts match by exact type, innermost level first. A with (...) at a
   call is its own level. Each context binding is its own level. One with
   block is one level, and so is the function's with clause. Top-level
   context bindings are the outermost level, searched after the function's
   parameters. A bare root fn main() with no with clause gets one more
   outermost level: Allocator, ErrorAllocator, Scope, and Logger, each only
   when that type is already loaded. Those are not parameters of main.
   A private function that omits with is inferred: the least set of context
   types its body still needs after subtracting what each call already
   provides. A recursive group is that least fixed point. */

static void install_main_defaults(Check *ck, CheckJob *job);

typedef struct CtxSrc {
    JogType *type;
    JogNode *expr;
} CtxSrc;

typedef struct CtxLevel {
    CtxSrc *srcs;
    size_t count;
    struct CtxLevel *outer;
} CtxLevel;

typedef struct Need {
    JogType *type;
    const char *chain;
} Need;

typedef struct Edge {
    CheckJob *callee;
    JogType **provided;
    size_t provided_count;
} Edge;

typedef struct InferState {
    CtDa needs;
    CtDa edges;
} InferState;

typedef struct Work {
    CheckJob *job;
    Need *needs;
    size_t count;
    size_t cap;
} Work;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static int is_error(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static int has_attr(const JogNode *fn, const char *name) {
    size_t i;
    if (!fn || !name) {
        return 0;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        JogNode *attr = list_at(fn->attrs, i);
        if (attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name && strcmp(attr->as.attribute.name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

bool jog_check_context_should_infer(const CheckJob *job) {
    const JogNode *fn;
    if (!job || !job->decl || job->decl->kind != JOG_NODE_FN) {
        return false;
    }
    fn = job->decl;
    if (fn->visibility == JOG_VIS_PUBLIC || fn->as.fn.is_extern || !fn->as.fn.body) {
        return false;
    }
    if (fn->as.fn.contexts.count > 0 || fn->as.fn.type_params.count > 0) {
        return false;
    }
    /* fn List<T>.get is generic even though T is not in type_params. */
    if (fn->as.fn.receiver) {
        const JogNode *recv = fn->as.fn.receiver;
        while (recv && (recv->kind == JOG_NODE_TYPE_PTR || recv->kind == JOG_NODE_TYPE_SLICE ||
                        recv->kind == JOG_NODE_TYPE_OPTION)) {
            recv = recv->as.type_elem.elem;
        }
        if (recv && recv->kind == JOG_NODE_TYPE_NAME && recv->as.type_name.args.count > 0) {
            return false;
        }
    }
    if (has_attr(fn, "export") || has_attr(fn, "test")) {
        return false;
    }
    return true;
}

static CheckJob *job_for_symbol(const Check *ck, const JogSymbol *symbol) {
    size_t i;
    size_t s;
    if (!ck || !symbol) {
        return NULL;
    }
    for (i = 0; i < ck->job_count; i++) {
        CheckJob *job = &ck->jobs[i];
        for (s = 0; s < job->symbol_count; s++) {
            if (job->symbols && job->symbols[s] == symbol) {
                return job;
            }
        }
    }
    return NULL;
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

static const char *fn_name(const JogNode *fn) {
    if (fn && fn->kind == JOG_NODE_FN && fn->as.fn.name && fn->as.fn.name[0] != '\0') {
        return fn->as.fn.name;
    }
    return "?";
}

static const char *join_chain(Check *ck, const char *name, const char *inner) {
    char buf[256];
    const char *out;
    if (!name || name[0] == '\0') {
        name = "?";
    }
    if (!inner || inner[0] == '\0' || strstr(inner, name) != NULL) {
        out = ct_intern_cstr(ck->program->intern, name);
    } else {
        snprintf(buf, sizeof(buf), "%s -> %s", name, inner);
        out = ct_intern_cstr(ck->program->intern, buf);
    }
    if (!out) {
        jog_check_oom(ck);
    }
    return out;
}

static InferState *state_of(Check *ck, CheckJob *job) {
    InferState *st;
    if (!ck || !job) {
        return NULL;
    }
    if (job->infer_state) {
        return job->infer_state;
    }
    st = ct_arena_alloc(ck->program->arena, sizeof(*st), _Alignof(InferState));
    if (!st) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(st, 0, sizeof(*st));
    if (!ct_da_init(&st->needs, sizeof(Need)) || !ct_da_init(&st->edges, sizeof(Edge))) {
        jog_check_oom(ck);
        ct_da_free(&st->needs);
        ct_da_free(&st->edges);
        return NULL;
    }
    job->infer_state = st;
    return st;
}

static void add_need(Check *ck, InferState *st, JogType *type, const char *chain) {
    size_t i;
    Need need;
    if (!st || !type || is_error(type)) {
        return;
    }
    for (i = 0; i < st->needs.count; i++) {
        Need *have = ct_da_at(&st->needs, i);
        if (have && have->type == type) {
            return;
        }
    }
    need.type = type;
    need.chain = chain ? chain : "";
    if (!ct_da_push(&st->needs, &need)) {
        jog_check_oom(ck);
    }
}

static const char *chain_on_job(const CheckJob *job, const JogType *type) {
    size_t i;
    if (!job || !type) {
        return NULL;
    }
    for (i = 0; i < job->inferred_count; i++) {
        if (job->inferred_contexts && job->inferred_contexts[i] == type) {
            return job->infer_chains ? job->infer_chains[i] : NULL;
        }
    }
    return NULL;
}

static CtxLevel *stack_of(const CheckJob *job) {
    return job ? (CtxLevel *)job->ctx_stack : NULL;
}

void jog_check_context_begin(CheckJob *job) {
    if (!job) {
        return;
    }
    job->ctx_stack = NULL;
    install_main_defaults(job->check, job);
}

void *jog_check_context_mark(const CheckJob *job) {
    return job ? job->ctx_stack : NULL;
}

void jog_check_context_reset(CheckJob *job, void *mark) {
    if (job) {
        job->ctx_stack = mark;
    }
}

static int push_level(Check *ck, CheckJob *job, CtxSrc *srcs, size_t count) {
    CtxLevel *level;
    if (!ck || !job) {
        return 0;
    }
    level = ct_arena_alloc(ck->program->arena, sizeof(*level), _Alignof(CtxLevel));
    if (!level) {
        jog_check_oom(ck);
        return -1;
    }
    level->srcs = srcs;
    level->count = count;
    level->outer = stack_of(job);
    job->ctx_stack = level;
    return 1;
}

static JogNode *make_ident(Check *ck, JogPos pos, JogSymbol *symbol, JogType *type) {
    JogNode *node = jog_node_new(ck->program->arena, JOG_NODE_IDENT, pos);
    if (!node) {
        jog_check_oom(ck);
        return NULL;
    }
    node->as.ident.name = symbol ? symbol->name : NULL;
    node->symbol = symbol;
    node->type = type;
    return node;
}

static int bare_root_main(const Check *ck, const CheckJob *job) {
    const JogNode *fn;
    if (!ck || !job || !ck->program || job->infer_collecting || ck->stop) {
        return 0;
    }
    if (job->package != ck->program->root) {
        return 0;
    }
    fn = job->decl;
    if (!fn || fn->kind != JOG_NODE_FN || fn->as.fn.receiver || fn->as.fn.is_extern || !fn->as.fn.body) {
        return 0;
    }
    if (!fn->as.fn.name || strcmp(fn->as.fn.name, "main") != 0) {
        return 0;
    }
    if (fn->as.fn.contexts.count != 0 || has_attr(fn, "test")) {
        return 0;
    }
    return 1;
}

static int push_known(Check *ck, CheckJob *job, JogSymbol **symbols, size_t count) {
    CtxSrc *srcs;
    size_t n = 0;
    size_t i;
    JogPos pos;
    if (!ck || !job || ck->stop || count == 0 || !symbols) {
        return ck && ck->stop ? 0 : 1;
    }
    srcs = ct_arena_alloc(ck->program->arena, count * sizeof(*srcs), _Alignof(CtxSrc));
    if (!srcs) {
        jog_check_oom(ck);
        return 0;
    }
    pos = job->decl ? job->decl->pos : (JogPos){0};
    for (i = 0; i < count; i++) {
        JogSymbol *symbol = symbols[i];
        if (!symbol || !symbol->type || is_error(symbol->type)) {
            continue;
        }
        srcs[n].type = symbol->type;
        srcs[n].expr = make_ident(ck, pos, symbol, symbol->type);
        if (ck->stop) {
            return 0;
        }
        n++;
    }
    if (n == 0) {
        return 1;
    }
    return push_level(ck, job, srcs, n) > 0;
}

static JogType *pkg_nominal(Check *ck, CheckJob *job, const char *path, const char *name, JogTypeKind kind) {
    JogPackage *pkg = NULL;
    const char *interned;
    JogSymbol *sym;
    size_t i;
    if (!ck || !ck->program || ck->stop || !path || !name) {
        return NULL;
    }
    for (i = 0; i < ck->program->package_count; i++) {
        JogPackage *cand = ck->program->packages[i];
        if (cand && cand->path && strcmp(cand->path, path) == 0) {
            pkg = cand;
            break;
        }
    }
    if (!pkg || !pkg->scope || !ck->program->intern) {
        return NULL;
    }
    interned = ct_intern_cstr(ck->program->intern, name);
    if (!interned) {
        jog_check_oom(ck);
        return NULL;
    }
    sym = jog_package_scope_lookup(pkg->scope, interned);
    if (!sym) {
        return NULL;
    }
    if (!sym->type && !jog_check_wait(ck, job, sym)) {
        return NULL;
    }
    if (!sym->type || sym->type->kind != kind || is_error(sym->type)) {
        return NULL;
    }
    return sym->type;
}

static JogType *universe_scope_type(Check *ck, const CheckJob *job) {
    const JogPackageScope *pkg;
    const char *interned;
    JogSymbol *sym;
    JogType *type;
    if (!ck || !ck->program || !ck->program->intern || !job || !job->package || !job->package->scope) {
        return NULL;
    }
    pkg = job->package->scope;
    if (!pkg->universe) {
        return NULL;
    }
    interned = ct_intern_cstr(ck->program->intern, "Scope");
    if (!interned) {
        jog_check_oom(ck);
        return NULL;
    }
    sym = jog_package_scope_lookup(pkg->universe, interned);
    type = sym ? sym->type : NULL;
    if (!type || type->kind != JOG_TY_DISTINCT || type->as.nominal.decl != NULL) {
        return NULL;
    }
    if (!type->as.nominal.name || strcmp(type->as.nominal.name, "Scope") != 0) {
        return NULL;
    }
    return type;
}

static void install_main_defaults(Check *ck, CheckJob *job) {
    JogNode *fn;
    JogType *alloc;
    JogType *err_alloc;
    JogType *scope;
    JogType *logger;
    JogType *types[4];
    JogSymbol **symbols;
    size_t n = 0;
    size_t i;
    if (!bare_root_main(ck, job)) {
        return;
    }
    fn = job->decl;
    if (fn->context_symbol_count > 0 && fn->context_symbols) {
        (void)push_known(ck, job, fn->context_symbols, fn->context_symbol_count);
        return;
    }
    alloc = pkg_nominal(ck, job, "std.mem", "Allocator", JOG_TY_INTERFACE);
    err_alloc = pkg_nominal(ck, job, "std.mem", "ErrorAllocator", JOG_TY_DISTINCT);
    if (err_alloc && (!alloc || err_alloc->as.nominal.base != alloc)) {
        err_alloc = NULL;
    }
    scope = universe_scope_type(ck, job);
    logger = pkg_nominal(ck, job, "std.io", "Logger", JOG_TY_INTERFACE);
    if (ck->stop) {
        return;
    }
    if (alloc) {
        types[n++] = alloc;
    }
    if (err_alloc) {
        types[n++] = err_alloc;
    }
    if (scope) {
        types[n++] = scope;
    }
    if (logger) {
        types[n++] = logger;
    }
    if (n == 0) {
        return;
    }
    symbols = ct_arena_alloc(ck->program->arena, n * sizeof(*symbols), _Alignof(JogSymbol *));
    if (!symbols) {
        jog_check_oom(ck);
        return;
    }
    memset(symbols, 0, n * sizeof(*symbols));
    for (i = 0; i < n; i++) {
        const char *name = types[i]->as.nominal.name;
        symbols[i] = jog_symbol_new(ck->program->arena, JOG_SYM_CONTEXT, name, fn, types[i], job->package);
        if (!symbols[i]) {
            jog_check_oom(ck);
            return;
        }
    }
    fn->context_symbols = symbols;
    fn->context_symbol_count = n;
    (void)push_known(ck, job, symbols, n);
}

void jog_check_context_push_params(Check *ck, CheckJob *job, JogSymbol **symbols, JogType **types, size_t count) {
    CtxSrc *srcs;
    size_t n = 0;
    size_t i;
    if (!ck || !job || ck->stop || count == 0) {
        return;
    }
    srcs = ct_arena_alloc(ck->program->arena, count * sizeof(*srcs), _Alignof(CtxSrc));
    if (!srcs) {
        jog_check_oom(ck);
        return;
    }
    for (i = 0; i < count; i++) {
        JogType *type = types ? types[i] : NULL;
        if (!symbols || !symbols[i] || !type || is_error(type)) {
            continue;
        }
        srcs[n].type = type;
        srcs[n].expr = make_ident(ck, job->decl ? job->decl->pos : (JogPos){0}, symbols[i], type);
        if (ck->stop) {
            return;
        }
        n++;
    }
    if (n == 0) {
        return;
    }
    (void)push_level(ck, job, srcs, n);
}

static int push_symbol(Check *ck, CheckJob *job, JogSymbol *symbol, JogType *type, JogPos pos) {
    CtxSrc *src;
    JogNode *ident;
    if (!symbol || !type || is_error(type)) {
        return 0;
    }
    src = ct_arena_alloc(ck->program->arena, sizeof(*src), _Alignof(CtxSrc));
    ident = make_ident(ck, pos, symbol, type);
    if (!src || !ident) {
        jog_check_oom(ck);
        return -1;
    }
    src->type = type;
    src->expr = ident;
    return push_level(ck, job, src, 1);
}

int jog_check_context_push_symbol(Check *ck, CheckJob *job, JogSymbol *symbol, JogType *type, JogPos pos) {
    return push_symbol(ck, job, symbol, type, pos);
}

int jog_check_context_binding(Check *ck, CheckJob *job, JogNode *stmt) {
    JogType *annotated = NULL;
    JogType *got = NULL;
    JogType *type;
    JogSymbol *symbol;
    const char *name;
    JogBlockScope *scope;
    if (!ck || !job || !stmt || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    name = stmt->as.binding.names.items && stmt->as.binding.names.count > 0 ? stmt->as.binding.names.items[0] : NULL;
    if (!stmt->as.binding.init) {
        jog_check_fail(ck, stmt->pos, "context requires an initializer");
        return 0;
    }
    if (stmt->as.binding.type) {
        annotated = jog_check_resolve_type(ck, job, stmt->as.binding.type);
        if (!annotated || ck->stop) {
            return ck->stop ? -1 : 0;
        }
    }
    scope = jog_check_body_scope(job);
    got = jog_check_expr(ck, job, scope, stmt->as.binding.init, annotated && !is_error(annotated) ? annotated : NULL);
    if (ck->stop) {
        return -1;
    }
    type = annotated ? annotated : got;
    if (!type) {
        type = jog_check_error_type(ck);
    }
    stmt->type = type;
    symbol = jog_check_body_bind(job, stmt, JOG_SYM_CONTEXT, name, type);
    stmt->symbol = symbol;
    if (!symbol) {
        return ck->stop ? -1 : 0;
    }
    jog_check_body_assigned(job, symbol);
    return push_symbol(ck, job, symbol, type, stmt->pos);
}

int jog_check_context_push_with(Check *ck, CheckJob *job, JogNode *stmt) {
    size_t n;
    size_t i;
    size_t kept = 0;
    JogSymbol **symbols;
    CtxSrc *srcs;
    JogBlockScope *scope;
    if (!ck || !job || !stmt || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    n = stmt->as.with.values.count;
    scope = jog_check_body_scope(job);
    symbols = NULL;
    srcs = NULL;
    if (n > 0) {
        symbols = ct_arena_alloc(ck->program->arena, n * sizeof(*symbols), _Alignof(JogSymbol *));
        srcs = ct_arena_alloc(ck->program->arena, n * sizeof(*srcs), _Alignof(CtxSrc));
        if (!symbols || !srcs) {
            jog_check_oom(ck);
            return -1;
        }
        memset(symbols, 0, n * sizeof(*symbols));
    }
    for (i = 0; i < n && !ck->stop; i++) {
        JogNode *value = list_at(stmt->as.with.values, i);
        JogType *type = value ? jog_check_expr(ck, job, scope, value, NULL) : NULL;
        JogSymbol *symbol;
        if (ck->stop) {
            return -1;
        }
        if (!type || is_error(type)) {
            continue;
        }
        symbol = jog_symbol_new(ck->program->arena, JOG_SYM_CONTEXT, NULL, value, type, job->package);
        if (!symbol) {
            jog_check_oom(ck);
            return -1;
        }
        symbols[i] = symbol;
        srcs[kept].type = type;
        srcs[kept].expr = make_ident(ck, value->pos, symbol, type);
        if (ck->stop) {
            return -1;
        }
        kept++;
    }
    stmt->context_symbols = symbols;
    stmt->context_symbol_count = n;
    stmt->type = jog_check_unit_type(ck);
    if (kept == 0) {
        return 0;
    }
    if (push_level(ck, job, srcs, kept) < 0) {
        return -1;
    }
    return 1;
}

void jog_check_context_pop(CheckJob *job) {
    CtxLevel *level = stack_of(job);
    if (!job || !level) {
        return;
    }
    job->ctx_stack = level->outer;
}

static int types_match(Check *ck, JogType *have, const JogType *want) {
    if (!have || !want) {
        return 0;
    }
    if (have == want) {
        return 1;
    }
    return jog_iface_converts(ck, have, (JogType *)want);
}

bool jog_check_context_cast(Check *ck, JogNode *node, JogType *got, JogType *dest) {
    if (!ck || !node || !got || !dest || is_error(got) || is_error(dest)) {
        return false;
    }
    if (got->kind == JOG_TY_DISTINCT && got->as.nominal.base == dest) {
        node->type = dest;
        return true;
    }
    if (dest->kind == JOG_TY_DISTINCT && dest->as.nominal.base == got) {
        node->type = dest;
        return true;
    }
    return false;
}

static int match_level(Check *ck, const CtxSrc *srcs, size_t count, const JogType *type, size_t *first) {
    size_t i;
    int n = 0;
    if (first) {
        *first = 0;
    }
    for (i = 0; i < count; i++) {
        if (srcs && types_match(ck, srcs[i].type, type)) {
            if (n == 0 && first) {
                *first = i;
            }
            n++;
        }
    }
    return n;
}

static int stack_has(const CheckJob *job, const JogType *type) {
    CtxLevel *level = stack_of(job);
    while (level) {
        if (match_level(job->check, level->srcs, level->count, type, NULL) > 0) {
            return 1;
        }
        level = level->outer;
    }
    return 0;
}

static JogSymbol *callee_symbol(const JogNode *call) {
    JogNode *callee;
    if (!call) {
        return NULL;
    }
    callee = call->as.call.callee;
    if (callee && callee->symbol) {
        return callee->symbol;
    }
    return call->symbol;
}

static JogType *callee_fn_type(const JogNode *call) {
    JogNode *callee;
    JogType *type;
    JogSymbol *symbol;
    if (!call) {
        return NULL;
    }
    callee = call->as.call.callee;
    type = callee ? callee->type : NULL;
    if (type && type->kind == JOG_TY_FN) {
        return type;
    }
    symbol = callee_symbol(call);
    type = symbol ? symbol->type : NULL;
    if (type && type->kind == JOG_TY_FN) {
        return type;
    }
    return NULL;
}

static JogNode *callee_fn(const JogNode *call) {
    JogSymbol *symbol = callee_symbol(call);
    if (symbol && symbol->decl && symbol->decl->kind == JOG_NODE_FN) {
        return symbol->decl;
    }
    return NULL;
}

static int callee_pending(const CheckJob *other) {
    return other && other->infer_contexts && !other->done && !other->infer_solved;
}

static JogType **snapshot_types(Check *ck, CheckJob *job, const CtxSrc *extra, size_t extra_n, size_t *out_n) {
    size_t n = extra_n;
    size_t at = 0;
    CtxLevel *level;
    JogType **types;
    size_t i;
    level = stack_of(job);
    while (level) {
        n += level->count;
        level = level->outer;
    }
    *out_n = 0;
    if (n == 0) {
        return NULL;
    }
    types = ct_arena_alloc(ck->program->arena, n * sizeof(*types), _Alignof(JogType *));
    if (!types) {
        jog_check_oom(ck);
        return NULL;
    }
    for (i = 0; i < extra_n; i++) {
        if (extra && extra[i].type && !is_error(extra[i].type)) {
            types[at++] = extra[i].type;
        }
    }
    level = stack_of(job);
    while (level) {
        for (i = 0; i < level->count; i++) {
            if (level->srcs && level->srcs[i].type && !is_error(level->srcs[i].type)) {
                types[at++] = level->srcs[i].type;
            }
        }
        level = level->outer;
    }
    *out_n = at;
    return types;
}

static int type_in(Check *ck, JogType *const *types, size_t count, const JogType *type) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (types && (types[i] == type || jog_iface_converts(ck, types[i], (JogType *)type))) {
            return 1;
        }
    }
    return 0;
}

static int type_explicit(Check *ck, CheckJob *job, JogNode *call, CtxSrc *out, size_t cap, size_t *n_out) {
    size_t i;
    size_t n = 0;
    JogBlockScope *scope = jog_check_body_scope(job);
    *n_out = 0;
    for (i = 0; i < call->as.call.contexts.count && !ck->stop; i++) {
        JogNode *expr = list_at(call->as.call.contexts, i);
        JogType *type;
        if (!expr) {
            continue;
        }
        type = jog_check_expr(ck, job, scope, expr, NULL);
        if (ck->stop) {
            return -1;
        }
        if (!type || is_error(type) || n >= cap) {
            continue;
        }
        out[n].type = type;
        out[n].expr = expr;
        n++;
    }
    *n_out = n;
    return ck->stop ? -1 : 0;
}

static int search_top(Check *ck, CheckJob *job, const JogType *want, JogNode **expr, int *ambiguous) {
    size_t i;
    int matches = 0;
    JogNode *found = NULL;
    *ambiguous = 0;
    *expr = NULL;
    if (!ck->jobs) {
        return 0;
    }
    for (i = 0; i < ck->job_count && !ck->stop; i++) {
        CheckJob *other = &ck->jobs[i];
        JogSymbol *symbol;
        JogType *type;
        if (!other->decl || other->decl->kind != JOG_NODE_CONTEXT || other == job) {
            continue;
        }
        symbol = other->symbol;
        if (!symbol || symbol->kind != JOG_SYM_CONTEXT) {
            continue;
        }
        if (!jog_symbol_visible_from(symbol, job->package, job->decl ? job->decl->pos.file : NULL)) {
            continue;
        }
        if (!symbol->type) {
            CheckJob *owner = job_for_symbol(ck, symbol);
            if (job->infer_collecting && owner && !owner->done) {
                /* The annotated type is published before the initializer runs.
                   Parking until that type appears must not wait for the
                   initializer itself: it may call back into this function. */
                while (!symbol->type && !owner->done && !ck->stop) {
                    job->infer_type_wait = true;
                    job->parked = true;
                    job->waiting_for = owner;
                    coroutine_yield();
                    ck->current_job = job;
                    job->parked = false;
                    job->waiting_for = NULL;
                    job->infer_type_wait = false;
                }
            } else if (!jog_check_wait(ck, job, symbol)) {
                return -1;
            }
        }
        if (ck->stop) {
            return -1;
        }
        type = symbol->type;
        if (!type || !types_match(ck, type, want)) {
            continue;
        }
        if (matches == 0) {
            found = make_ident(ck, job->decl ? job->decl->pos : (JogPos){0}, symbol, type);
            if (ck->stop) {
                return -1;
            }
        }
        matches++;
    }
    if (matches > 1) {
        *ambiguous = 1;
        return 1;
    }
    if (matches == 1) {
        *expr = found;
        return 1;
    }
    return 0;
}

static const char *via_chain(Check *ck, CheckJob *callee, const JogType *type) {
    const char *name = callee && callee->decl ? fn_name(callee->decl) : "?";
    const char *inner = chain_on_job(callee, type);
    return join_chain(ck, name, inner);
}

static void record_need(Check *ck, CheckJob *job, CheckJob *callee, JogType *type) {
    InferState *st = state_of(ck, job);
    const char *chain;
    if (!st || ck->stop) {
        return;
    }
    chain = via_chain(ck, callee, type);
    add_need(ck, st, type, chain);
}

static void record_edge(Check *ck, CheckJob *job, CheckJob *callee, JogType **provided, size_t provided_count) {
    InferState *st = state_of(ck, job);
    Edge edge;
    if (!st || !callee || ck->stop) {
        return;
    }
    edge.callee = callee;
    edge.provided = provided;
    edge.provided_count = provided_count;
    if (!ct_da_push(&st->edges, &edge)) {
        jog_check_oom(ck);
    }
}

/* Distinct types are matched by their own name. jog_type_underlying peels
   them, which would hide Scope (distinct *void) from context lookup. */
static bool context_type_named(JogType *type, const char *name) {
    const char *have;
    if (!type || !name) {
        return false;
    }
    if (type->kind == JOG_TY_PTR && type->as.elem.elem) {
        type = type->as.elem.elem;
    }
    if (type->kind == JOG_TY_DISTINCT && type->as.nominal.name && strcmp(type->as.nominal.name, name) == 0) {
        return true;
    }
    type = jog_type_underlying(type);
    if (!type || (type->kind != JOG_TY_INTERFACE && type->kind != JOG_TY_STRUCT && type->kind != JOG_TY_ENUM &&
                  type->kind != JOG_TY_DISTINCT)) {
        return false;
    }
    have = type->as.nominal.name;
    return have && strcmp(have, name) == 0;
}

JogNode *jog_check_context_find(const CheckJob *job, const char *name) {
    CtxLevel *level;
    if (!job || !name) {
        return NULL;
    }
    for (level = stack_of(job); level; level = level->outer) {
        size_t i;
        for (i = 0; i < level->count; i++) {
            JogType *type = level->srcs ? level->srcs[i].type : NULL;
            if (context_type_named(type, name)) {
                return level->srcs[i].expr;
            }
        }
    }
    return NULL;
}

bool jog_check_context_has_named(const CheckJob *job, const char *name) {
    CtxLevel *level;
    if (!job || !name) {
        return false;
    }
    for (level = stack_of(job); level; level = level->outer) {
        size_t i;
        for (i = 0; i < level->count; i++) {
            JogType *type = level->srcs ? level->srcs[i].type : NULL;
            if (context_type_named(type, name)) {
                return true;
            }
        }
    }
    return false;
}

int jog_check_context_resolve(Check *ck, CheckJob *job, JogNode *call) {
    JogType *fn;
    JogSymbol *symbol;
    CheckJob *callee_job;
    JogNode *decl;
    CtxSrc explicit_buf[32];
    CtxSrc *explicit_srcs;
    size_t explicit_n = 0;
    size_t need_n;
    size_t i;
    int collecting;
    JogNode **args;
    char text[160];
    if (!ck || !job || !call || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    collecting = job->infer_collecting;
    symbol = callee_symbol(call);
    callee_job = job_for_symbol(ck, symbol);
    decl = callee_fn(call);
    fn = callee_fn_type(call);
    if (call->as.call.contexts.count > 32) {
        explicit_srcs = ct_arena_alloc(ck->program->arena, call->as.call.contexts.count * sizeof(*explicit_srcs), _Alignof(CtxSrc));
        if (!explicit_srcs) {
            jog_check_oom(ck);
            return -1;
        }
    } else {
        explicit_srcs = explicit_buf;
    }
    if (type_explicit(ck, job, call, explicit_srcs, call->as.call.contexts.count > 32 ? call->as.call.contexts.count : 32,
                      &explicit_n) < 0) {
        return -1;
    }
    if (collecting && callee_pending(callee_job)) {
        size_t provided_n = 0;
        JogType **provided = snapshot_types(ck, job, explicit_srcs, explicit_n, &provided_n);
        if (ck->stop) {
            return -1;
        }
        record_edge(ck, job, callee_job, provided, provided_n);
        return ck->stop ? -1 : 0;
    }
    if (!fn) {
        return 0;
    }
    need_n = fn->as.fn.context_count;
    if (need_n == 0) {
        if (!collecting && explicit_n > 0) {
            jog_check_fail(ck, call->pos, "this function takes no contexts");
        }
        return 0;
    }
    args = NULL;
    if (!collecting) {
        args = ct_arena_alloc(ck->program->arena, need_n * sizeof(*args), _Alignof(JogNode *));
        if (!args) {
            jog_check_oom(ck);
            return -1;
        }
        memset(args, 0, need_n * sizeof(*args));
    }
    for (i = 0; i < need_n && !ck->stop; i++) {
        JogType *want = fn->as.fn.contexts ? fn->as.fn.contexts[i] : NULL;
        CtxLevel *level;
        int found = 0;
        size_t at = 0;
        int matches;
        JogNode *chosen = NULL;
        if (!want || is_error(want)) {
            continue;
        }
        matches = match_level(ck, explicit_srcs, explicit_n, want, &at);
        if (matches > 1) {
            found = 1;
            if (!collecting) {
                type_text(want, text, sizeof(text));
                jog_check_fail(ck, call->pos, "ambiguous context %s", text);
            }
        } else if (matches == 1) {
            found = 1;
            chosen = explicit_srcs[at].expr;
        }
        if (!found) {
            level = stack_of(job);
            while (level && !found) {
                matches = match_level(ck, level->srcs, level->count, want, &at);
                if (matches > 1) {
                    found = 1;
                    if (!collecting) {
                        type_text(want, text, sizeof(text));
                        jog_check_fail(ck, call->pos, "ambiguous context %s", text);
                    }
                } else if (matches == 1) {
                    found = 1;
                    chosen = level->srcs[at].expr;
                }
                level = level->outer;
            }
        }
        if (!found) {
            int ambiguous = 0;
            int top = search_top(ck, job, want, &chosen, &ambiguous);
            if (top < 0) {
                return -1;
            }
            if (ambiguous) {
                found = 1;
                chosen = NULL;
                if (!collecting) {
                    type_text(want, text, sizeof(text));
                    jog_check_fail(ck, call->pos, "ambiguous context %s", text);
                }
            } else if (top > 0) {
                found = 1;
            }
        }
        if (!found) {
            if (collecting) {
                if (!stack_has(job, want)) {
                    record_need(ck, job, callee_job, want);
                }
            } else {
                const char *inner;
                type_text(want, text, sizeof(text));
                inner = callee_job ? chain_on_job(callee_job, want) : NULL;
                if (callee_job && callee_job->infer_contexts && inner && inner[0] != '\0') {
                    jog_check_fail(ck, call->pos, "missing context %s (%s -> %s)", text, fn_name(decl), inner);
                } else {
                    jog_check_fail(ck, call->pos, "missing context %s", text);
                }
            }
            continue;
        }
        if (!collecting && chosen) {
            args[i] = chosen;
        }
    }
    if (!collecting && explicit_n > 0) {
        for (i = 0; i < explicit_n && !ck->stop; i++) {
            int used = 0;
            size_t k;
            for (k = 0; k < need_n; k++) {
                JogType *want = fn->as.fn.contexts ? fn->as.fn.contexts[k] : NULL;
                if (want && types_match(ck, explicit_srcs[i].type, want)) {
                    used = 1;
                    break;
                }
            }
            if (!used) {
                jog_check_fail(ck, explicit_srcs[i].expr ? explicit_srcs[i].expr->pos : call->pos,
                               "context value matches no parameter");
            }
        }
    }
    if (ck->stop) {
        return -1;
    }
    if (!collecting) {
        call->context_args = args;
        call->context_arg_count = need_n;
    }
    return 0;
}

static int name_taken(const JogNode *fn, const char *name) {
    size_t i;
    if (!name) {
        return 0;
    }
    if (fn->as.fn.receiver && strcmp(name, "self") == 0) {
        return 1;
    }
    for (i = 0; i < fn->as.fn.params.count; i++) {
        JogNode *param = list_at(fn->as.fn.params, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *fresh_name(Check *ck, const JogNode *fn, size_t index) {
    char buf[64];
    const char *name;
    snprintf(buf, sizeof(buf), "ctx%zu", index);
    name = ct_intern_cstr(ck->program->intern, buf);
    if (name && name_taken(fn, name)) {
        snprintf(buf, sizeof(buf), "ctx%zu_", index);
        name = ct_intern_cstr(ck->program->intern, buf);
    }
    if (!name) {
        jog_check_oom(ck);
    }
    return name;
}

static int install_params(Check *ck, CheckJob *job) {
    JogNode *fn;
    size_t n;
    size_t i;
    JogNode **items;
    if (!job || !job->decl) {
        return 0;
    }
    fn = job->decl;
    n = job->inferred_count;
    if (n == 0) {
        return 1;
    }
    items = ct_arena_alloc(ck->program->arena, n * sizeof(*items), _Alignof(JogNode *));
    if (!items) {
        jog_check_oom(ck);
        return 0;
    }
    for (i = 0; i < n; i++) {
        JogNode *param = jog_node_new(ck->program->arena, JOG_NODE_PARAM, fn->pos);
        if (!param) {
            jog_check_oom(ck);
            return 0;
        }
        param->as.param.name = fresh_name(ck, fn, i);
        param->type = job->inferred_contexts[i];
        if (ck->stop) {
            return 0;
        }
        items[i] = param;
    }
    fn->as.fn.contexts.items = items;
    fn->as.fn.contexts.count = n;
    return !ck->stop;
}

static void copy_needs_to_job(Check *ck, CheckJob *job, const Need *needs, size_t count) {
    size_t i;
    job->inferred_count = count;
    job->inferred_contexts = NULL;
    job->infer_chains = NULL;
    if (count == 0) {
        return;
    }
    job->inferred_contexts = ct_arena_alloc(ck->program->arena, count * sizeof(JogType *), _Alignof(JogType *));
    job->infer_chains = ct_arena_alloc(ck->program->arena, count * sizeof(char *), _Alignof(char *));
    if (!job->inferred_contexts || !job->infer_chains) {
        jog_check_oom(ck);
        return;
    }
    for (i = 0; i < count; i++) {
        job->inferred_contexts[i] = needs[i].type;
        job->infer_chains[i] = needs[i].chain ? needs[i].chain : "";
    }
}

static void materialize(Check *ck, CheckJob *job) {
    InferState *st = job->infer_state;
    Need *needs;
    size_t count;
    size_t cap;
    size_t i;
    size_t e;
    if (job->infer_solved) {
        return;
    }
    count = st ? st->needs.count : 0;
    cap = count + 4;
    needs = NULL;
    if (cap > 0) {
        needs = ct_arena_alloc(ck->program->arena, cap * sizeof(*needs), _Alignof(Need));
        if (!needs) {
            jog_check_oom(ck);
            return;
        }
    }
    for (i = 0; i < count; i++) {
        Need *have = ct_da_at(&st->needs, i);
        needs[i] = have ? *have : (Need){0};
    }
    if (st) {
        for (e = 0; e < st->edges.count && !ck->stop; e++) {
            Edge *edge = ct_da_at(&st->edges, e);
            JogType **contexts = NULL;
            size_t nctx = 0;
            CheckJob *callee;
            if (!edge || !edge->callee || !edge->callee->done) {
                continue;
            }
            callee = edge->callee;
            if (callee->symbol && callee->symbol->type && callee->symbol->type->kind == JOG_TY_FN) {
                contexts = callee->symbol->type->as.fn.contexts;
                nctx = callee->symbol->type->as.fn.context_count;
            }
            for (i = 0; i < nctx; i++) {
                JogType *type = contexts ? contexts[i] : NULL;
                size_t k;
                int have = 0;
                if (!type || is_error(type) || type_in(ck, edge->provided, edge->provided_count, type)) {
                    continue;
                }
                for (k = 0; k < count; k++) {
                    if (needs[k].type == type) {
                        have = 1;
                        break;
                    }
                }
                if (have) {
                    continue;
                }
                if (count == cap) {
                    Need *grown;
                    size_t next = cap * 2 + 4;
                    grown = ct_arena_alloc(ck->program->arena, next * sizeof(*grown), _Alignof(Need));
                    if (!grown) {
                        jog_check_oom(ck);
                        return;
                    }
                    memcpy(grown, needs, count * sizeof(*grown));
                    needs = grown;
                    cap = next;
                }
                needs[count].type = type;
                needs[count].chain = via_chain(ck, callee, type);
                count++;
            }
        }
    }
    copy_needs_to_job(ck, job, needs, count);
}

static int edges_done(const CheckJob *job) {
    InferState *st = job ? job->infer_state : NULL;
    size_t i;
    if (!st) {
        return 1;
    }
    for (i = 0; i < st->edges.count; i++) {
        Edge *edge = ct_da_at(&st->edges, i);
        if (!edge || !edge->callee || !edge->callee->done) {
            return 0;
        }
    }
    return 1;
}

static JogType *rebuild(Check *ck, JogType *partial, CheckJob *job) {
    if (!partial || partial->kind != JOG_TY_FN) {
        return partial;
    }
    if (job->inferred_count == 0) {
        return partial;
    }
    return jog_type_fn(ck->program->types, partial->as.fn.params, partial->as.fn.param_count, partial->as.fn.ret,
                       job->inferred_contexts, job->inferred_count, partial->as.fn.variadic, partial->as.fn.escaping,
                       partial->as.fn.is_extern);
}

JogType *jog_check_context_infer(Check *ck, CheckJob *job, JogType *partial) {
    if (!ck || !job || !partial || partial->kind != JOG_TY_FN || ck->stop) {
        return ck && ck->stop ? NULL : partial;
    }
    job->infer_contexts = true;
    job->decl->type = partial;
    if (job->symbol) {
        job->symbol->type = partial;
    }
    job->sig_ready = true;
    job->infer_collecting = true;
    ck->current_job = job;
    jog_check_fn_body(ck, job);
    job->infer_collecting = false;
    ck->current_job = job;
    if (ck->stop) {
        return NULL;
    }
    if (!edges_done(job)) {
        while (!ck->stop && !job->infer_solved && !edges_done(job)) {
            InferState *st = job->infer_state;
            CheckJob *wait = NULL;
            size_t i;
            if (st) {
                for (i = 0; i < st->edges.count; i++) {
                    Edge *edge = ct_da_at(&st->edges, i);
                    if (edge && edge->callee && !edge->callee->done) {
                        wait = edge->callee;
                        break;
                    }
                }
            }
            job->infer_parked = true;
            job->parked = true;
            job->waiting_for = wait;
            coroutine_yield();
            ck->current_job = job;
            job->parked = false;
            job->infer_parked = false;
            job->waiting_for = NULL;
        }
    }
    if (ck->stop) {
        return NULL;
    }
    if (!job->infer_solved) {
        materialize(ck, job);
    }
    if (ck->stop) {
        return NULL;
    }
    if (!install_params(ck, job)) {
        return ck->stop ? NULL : jog_check_error_type(ck);
    }
    {
        JogType *built = rebuild(ck, partial, job);
        if (!built) {
            jog_check_oom(ck);
            return NULL;
        }
        return built;
    }
}

static int work_has(const Work *work, const JogType *type) {
    size_t i;
    for (i = 0; i < work->count; i++) {
        if (work->needs[i].type == type) {
            return 1;
        }
    }
    return 0;
}

static int work_add(Check *ck, Work *work, JogType *type, const char *chain) {
    if (!type || is_error(type) || work_has(work, type)) {
        return 0;
    }
    if (work->count == work->cap) {
        size_t next = work->cap * 2 + 4;
        Need *grown = ct_arena_alloc(ck->program->arena, next * sizeof(*grown), _Alignof(Need));
        if (!grown) {
            jog_check_oom(ck);
            return -1;
        }
        if (work->needs && work->count > 0) {
            memcpy(grown, work->needs, work->count * sizeof(*grown));
        }
        work->needs = grown;
        work->cap = next;
    }
    work->needs[work->count].type = type;
    work->needs[work->count].chain = chain ? chain : "";
    work->count++;
    return 1;
}

static const char *work_chain(const Work *work, const JogType *type) {
    size_t i;
    for (i = 0; i < work->count; i++) {
        if (work->needs[i].type == type) {
            return work->needs[i].chain;
        }
    }
    return NULL;
}

static void callee_context_types(const CheckJob *callee, const Work *works, size_t nworks, JogType ***out, size_t *out_n) {
    size_t i;
    *out = NULL;
    *out_n = 0;
    for (i = 0; i < nworks; i++) {
        if (works[i].job == callee) {
            return;
        }
    }
    if (callee->infer_solved && callee->inferred_contexts) {
        *out = callee->inferred_contexts;
        *out_n = callee->inferred_count;
        return;
    }
    if (callee->done && callee->symbol && callee->symbol->type && callee->symbol->type->kind == JOG_TY_FN) {
        *out = callee->symbol->type->as.fn.contexts;
        *out_n = callee->symbol->type->as.fn.context_count;
    }
}

bool jog_check_context_solve(Check *ck) {
    size_t n;
    size_t i;
    size_t w;
    unsigned char *in_set;
    Work *works;
    int solved = 0;
    int changed;
    if (!ck || !ck->jobs || ck->job_count == 0) {
        return false;
    }
    n = ck->job_count;
    in_set = ct_arena_alloc(ck->program->arena, n, 1);
    if (!in_set) {
        jog_check_oom(ck);
        return false;
    }
    memset(in_set, 0, n);
    for (i = 0; i < n; i++) {
        if (ck->jobs[i].infer_parked && !ck->jobs[i].done) {
            in_set[i] = 1;
        }
    }
    changed = 1;
    while (changed) {
        changed = 0;
        for (i = 0; i < n; i++) {
            InferState *st;
            size_t e;
            if (!in_set[i]) {
                continue;
            }
            st = ck->jobs[i].infer_state;
            if (!st) {
                continue;
            }
            for (e = 0; e < st->edges.count; e++) {
                Edge *edge = ct_da_at(&st->edges, e);
                CheckJob *callee;
                size_t idx;
                if (!edge || !edge->callee || edge->callee->done || edge->callee->infer_solved) {
                    continue;
                }
                callee = edge->callee;
                idx = (size_t)(callee - ck->jobs);
                if (idx >= n || !in_set[idx]) {
                    in_set[i] = 0;
                    changed = 1;
                    break;
                }
            }
        }
    }
    w = 0;
    for (i = 0; i < n; i++) {
        if (in_set[i]) {
            w++;
        }
    }
    if (w == 0) {
        return false;
    }
    works = ct_arena_alloc(ck->program->arena, w * sizeof(*works), _Alignof(Work));
    if (!works) {
        jog_check_oom(ck);
        return false;
    }
    memset(works, 0, w * sizeof(*works));
    w = 0;
    for (i = 0; i < n; i++) {
        InferState *st;
        size_t k;
        if (!in_set[i]) {
            continue;
        }
        works[w].job = &ck->jobs[i];
        st = ck->jobs[i].infer_state;
        if (st) {
            for (k = 0; k < st->needs.count; k++) {
                Need *need = ct_da_at(&st->needs, k);
                if (need && work_add(ck, &works[w], need->type, need->chain) < 0) {
                    return false;
                }
            }
        }
        w++;
    }
    changed = 1;
    {
        size_t guard = 0;
        size_t limit = w * 8 + 4;
        while (changed && guard++ < limit && !ck->stop) {
            changed = 0;
            for (i = 0; i < w; i++) {
                InferState *st = works[i].job->infer_state;
                size_t e;
                if (!st) {
                    continue;
                }
                for (e = 0; e < st->edges.count; e++) {
                    Edge *edge = ct_da_at(&st->edges, e);
                    Work *src = NULL;
                    JogType **contexts = NULL;
                    size_t nctx = 0;
                    size_t c;
                    size_t s;
                    if (!edge || !edge->callee) {
                        continue;
                    }
                    for (s = 0; s < w; s++) {
                        if (works[s].job == edge->callee) {
                            src = &works[s];
                            break;
                        }
                    }
                    if (src) {
                        nctx = src->count;
                    } else {
                        callee_context_types(edge->callee, works, w, &contexts, &nctx);
                    }
                    for (c = 0; c < nctx; c++) {
                        JogType *type = src ? src->needs[c].type : (contexts ? contexts[c] : NULL);
                        const char *inner;
                        const char *chain;
                        int added;
                        if (!type || type_in(ck, edge->provided, edge->provided_count, type)) {
                            continue;
                        }
                        inner = src ? work_chain(src, type) : chain_on_job(edge->callee, type);
                        chain = join_chain(ck, fn_name(edge->callee->decl), inner);
                        added = work_add(ck, &works[i], type, chain);
                        if (added < 0) {
                            return false;
                        }
                        if (added > 0) {
                            changed = 1;
                        }
                    }
                }
            }
        }
        if (guard >= limit && changed) {
            jog_check_fail(ck, (JogPos){0}, "context inference did not finish");
            return false;
        }
    }
    for (i = 0; i < w && !ck->stop; i++) {
        CheckJob *job = works[i].job;
        copy_needs_to_job(ck, job, works[i].needs, works[i].count);
        job->infer_solved = true;
        job->infer_parked = false;
        job->parked = false;
        job->waiting_for = NULL;
        solved = 1;
    }
    return solved && !ck->stop;
}

/* A collecting function parked on a top-level context can resume once that
   context's type is published, even though the initializer is not done. */
bool jog_check_context_wake_type(Check *ck) {
    size_t i;
    int woke = 0;
    if (!ck || !ck->jobs) {
        return false;
    }
    for (i = 0; i < ck->job_count; i++) {
        CheckJob *job = &ck->jobs[i];
        CheckJob *owner;
        if (!job->infer_type_wait || !job->parked || !job->waiting_for) {
            continue;
        }
        owner = job->waiting_for;
        if (!owner->decl || owner->decl->kind != JOG_NODE_CONTEXT || !owner->symbol || !owner->symbol->type) {
            continue;
        }
        job->parked = false;
        job->waiting_for = NULL;
        job->infer_type_wait = false;
        woke = 1;
    }
    return woke;
}

void jog_check_context_cleanup(Check *ck) {
    size_t i;
    if (!ck || !ck->jobs) {
        return;
    }
    for (i = 0; i < ck->job_count; i++) {
        InferState *st = ck->jobs[i].infer_state;
        if (!st) {
            continue;
        }
        ct_da_free(&st->needs);
        ct_da_free(&st->edges);
        ck->jobs[i].infer_state = NULL;
    }
}
