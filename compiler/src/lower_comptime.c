#include "lower_comptime.h"

#include "check_comptime.h"
#include "file.h"
#include "lower_generic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOG_COMPTIME_STEPS 10000000ull

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static const char *seq_name(Lower *L, const char *prefix) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%u", prefix, L->ct_seq);
    L->ct_seq++;
    if (!L->program || !L->program->arena) {
        return NULL;
    }
    return ct_arena_strdup(L->program->arena, buf);
}

static int type_has_ptr(JogType *type, int depth) {
    size_t i;
    if (!type || depth > 64) {
        return 0;
    }
    type = jog_type_underlying(type);
    if (!type) {
        return 0;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_BYTES:
    case JOG_TY_FN:
    case JOG_TY_INTERFACE:
        return 1;
    case JOG_TY_ARRAY:
        return type_has_ptr(type->as.array.elem, depth + 1);
    case JOG_TY_OPTION:
        if (jog_type_option_is_niche(type)) {
            return 1;
        }
        return type_has_ptr(type->as.elem.elem, depth + 1);
    case JOG_TY_TUPLE:
        for (i = 0; i < type->as.tuple.count; i++) {
            JogType *elem = type->as.tuple.elems ? type->as.tuple.elems[i] : NULL;
            if (type_has_ptr(elem, depth + 1)) {
                return 1;
            }
        }
        return 0;
    case JOG_TY_STRUCT:
        for (i = 0; i < type->as.nominal.field_count; i++) {
            JogType *elem = type->as.nominal.fields ? type->as.nominal.fields[i].type : NULL;
            if (type_has_ptr(elem, depth + 1)) {
                return 1;
            }
        }
        return 0;
    case JOG_TY_ENUM:
        for (i = 0; i < type->as.nominal.variant_count; i++) {
            JogVariant *variant = type->as.nominal.variants ? &type->as.nominal.variants[i] : NULL;
            size_t f;
            if (!variant) {
                continue;
            }
            for (f = 0; f < variant->field_count; f++) {
                JogType *elem = variant->fields ? variant->fields[f].type : NULL;
                if (type_has_ptr(elem, depth + 1)) {
                    return 1;
                }
            }
        }
        return 0;
    default:
        return 0;
    }
}

typedef struct Walk {
    Lower *L;
    int self;
    int n;
    uint8_t *deps;
    int *map;
    size_t nglobal;
} Walk;

static void walk(Walk *w, JogNode *node);

static void walk_list(Walk *w, JogList list) {
    size_t i;
    for (i = 0; i < list.count && !w->L->failed; i++) {
        walk(w, list_at(list, i));
    }
}

static void note(Walk *w, JogNode *node, JogSymbol *symbol) {
    GlobalBind *global;
    int ci;
    const char *name;
    if (!w || !symbol || !w->L || w->L->failed) {
        return;
    }
    global = jog_lower_global_bind(w->L, symbol);
    if (!global || !global->runtime || global->index < 0) {
        return;
    }
    if ((size_t)global->index >= w->nglobal) {
        return;
    }
    ci = w->map[global->index];
    if (ci < 0) {
        name = symbol->name ? symbol->name : "?";
        jog_lower_fail(w->L, node ? node->pos : (JogPos){0},
                       "compile-time initializer reads '%s', which is not a compile-time constant", name);
        return;
    }
    if (w->self >= 0 && w->self < w->n && ci < w->n) {
        w->deps[(size_t)w->self * (size_t)w->n + (size_t)ci] = 1;
    }
}

static void walk(Walk *w, JogNode *node) {
    if (!node || !w || w->L->failed) {
        return;
    }
    if (node->kind == JOG_NODE_FN) {
        return;
    }
    if ((node->kind == JOG_NODE_IDENT || node->kind == JOG_NODE_MEMBER) && node->symbol) {
        note(w, node, node->symbol);
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        walk_list(w, node->as.ident.type_args);
        return;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        walk(w, node->as.unary.operand);
        return;
    case JOG_NODE_BINARY:
        walk(w, node->as.binary.lhs);
        walk(w, node->as.binary.rhs);
        return;
    case JOG_NODE_CALL:
        walk(w, node->as.call.callee);
        walk_list(w, node->as.call.args);
        walk(w, node->as.call.trailing);
        walk_list(w, node->as.call.contexts);
        return;
    case JOG_NODE_ARG:
        walk(w, node->as.arg.value);
        return;
    case JOG_NODE_BUILTIN:
        walk_list(w, node->as.builtin.args);
        return;
    case JOG_NODE_INDEX:
        walk(w, node->as.index.base);
        walk(w, node->as.index.index);
        return;
    case JOG_NODE_SLICE:
        walk(w, node->as.slice.base);
        walk(w, node->as.slice.lo);
        walk(w, node->as.slice.hi);
        return;
    case JOG_NODE_MEMBER:
        walk(w, node->as.member.base);
        return;
    case JOG_NODE_TUPLE:
        walk_list(w, node->as.tuple.elems);
        return;
    case JOG_NODE_CAST:
        walk(w, node->as.cast.expr);
        walk(w, node->as.cast.type);
        return;
    case JOG_NODE_IF:
        walk(w, node->as.if_.cond);
        walk(w, node->as.if_.then_block);
        walk(w, node->as.if_.else_branch);
        return;
    case JOG_NODE_BLOCK:
        walk_list(w, node->as.block.stmts);
        return;
    case JOG_NODE_EXPR_STMT:
        walk(w, node->as.expr_stmt.expr);
        return;
    case JOG_NODE_ASSIGN:
        walk(w, node->as.assign.target);
        walk(w, node->as.assign.value);
        return;
    case JOG_NODE_RETURN:
        walk_list(w, node->as.ret.values);
        return;
    case JOG_NODE_FOR:
        walk(w, node->as.for_.expr);
        walk(w, node->as.for_.body);
        return;
    case JOG_NODE_ARRAY_LIT:
        walk(w, node->as.array_lit.type);
        walk_list(w, node->as.array_lit.elems);
        return;
    default:
        return;
    }
}

static int push_id(int32_t **stack, size_t *sp, size_t *cap, int32_t id) {
    if (*sp >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        int32_t *grown = realloc(*stack, ncap * sizeof(*grown));
        if (!grown) {
            return 0;
        }
        *stack = grown;
        *cap = ncap;
    }
    (*stack)[(*sp)++] = id;
    return 1;
}

static int prepare(Lower *L, int32_t root) {
    uint8_t *seen = NULL;
    size_t cap = 0;
    int32_t *stack = NULL;
    size_t sp = 0;
    size_t stack_cap = 0;
    int ok = 1;
    if (!push_id(&stack, &sp, &stack_cap, root)) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        return 0;
    }
    while (sp > 0 && ok && !L->failed) {
        int32_t id = stack[--sp];
        JirFunc *fn;
        size_t b;
        if (id < 0) {
            continue;
        }
        if ((size_t)id >= cap) {
            size_t ncap = L->module->funcs.count;
            uint8_t *grown;
            if ((size_t)id >= ncap) {
                ncap = (size_t)id + 1;
            }
            grown = realloc(seen, ncap ? ncap : 1);
            if (!grown) {
                jog_lower_fail(L, (JogPos){0}, "out of memory");
                ok = 0;
                break;
            }
            if (ncap > cap) {
                memset(grown + cap, 0, ncap - cap);
            }
            seen = grown;
            cap = ncap;
        }
        if (seen[id]) {
            continue;
        }
        seen[id] = 1;
        if (!jog_lower_ensure_index(L, id)) {
            ok = 0;
            break;
        }
        fn = jir_func(L->module, (uint32_t)id);
        if (!fn) {
            continue;
        }
        for (b = 0; b < fn->blocks.count && ok; b++) {
            JirBlock *block = jir_block_at(fn, (uint32_t)b);
            size_t k;
            if (!block) {
                continue;
            }
            for (k = 0; k < block->insts.count; k++) {
                JirInst *inst = ct_da_at(&block->insts, k);
                if (!inst || inst->op != JIR_CALL || inst->a.kind != JIR_OPND_FUNC) {
                    continue;
                }
                if (!push_id(&stack, &sp, &stack_cap, (int32_t)inst->a.as.func)) {
                    jog_lower_fail(L, inst->pos, "out of memory");
                    ok = 0;
                    break;
                }
            }
        }
    }
    free(seen);
    free(stack);
    return ok && !L->failed;
}

static JirOperand materialize(Lower *L, JogType *type, const uint8_t *bytes, size_t n, JogPos pos) {
    JogType *base;
    uint64_t bits = 0;
    size_t i;
    const char *gname;
    int32_t gindex;
    JirGlobal *global;
    uint32_t dst;
    JirInst *inst;
    if (!type) {
        jog_lower_fail(L, pos, "compile-time value has no type");
        return jir_none();
    }
    if (type_has_ptr(type, 0)) {
        jog_lower_fail(L, pos, "compile-time value contains a pointer");
        return jir_none();
    }
    if (!type->has_layout || type->size > SIZE_MAX || n != (size_t)type->size) {
        jog_lower_fail(L, pos, "compile-time value has the wrong size");
        return jir_none();
    }
    if (n > 0 && !bytes) {
        jog_lower_fail(L, pos, "compile-time value is missing");
        return jir_none();
    }
    base = jog_type_underlying(type);
    if (base && (base->kind == JOG_TY_INT || base->kind == JOG_TY_BOOL) && n <= 8) {
        for (i = 0; i < n; i++) {
            bits |= (uint64_t)bytes[i] << (8 * i);
        }
        return jir_int(type, bits);
    }
    if (n == 0) {
        return jir_none();
    }
    if (!bytes) {
        jog_lower_fail(L, pos, "compile-time value is missing");
        return jir_none();
    }
    gname = seq_name(L, "jog__ctdat_");
    if (!gname) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    gindex = jir_global_add(L->module, gname, type, 1);
    global = gindex >= 0 ? jir_global(L->module, (uint32_t)gindex) : NULL;
    if (!global) {
        jog_lower_fail(L, pos, "out of memory");
        return jir_none();
    }
    global->data = bytes;
    global->data_len = n;
    global->pos = pos;
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return jir_none();
    }
    inst = jog_lower_emit(L, JIR_LOAD, (int32_t)dst, pos);
    if (!inst) {
        return jir_none();
    }
    inst->a = jir_global_ref(L->module, (uint32_t)gindex);
    return jog_lower_local_op(L, dst);
}

static const char *dot_comptime_name(Lower *L) {
    char buf[256];
    const char *base = L->func && L->func->jog_name && L->func->jog_name[0] ? L->func->jog_name : "comptime";
    snprintf(buf, sizeof(buf), "%s.comptime", base);
    if (!L->program || !L->program->arena) {
        return NULL;
    }
    return ct_arena_strdup(L->program->arena, buf);
}

static int eval_expr(Lower *L, JogNode *expr, JogType *type, const char *jog_name, const uint8_t **out) {
    const char *cname;
    int32_t idx;
    *out = NULL;
    cname = seq_name(L, "jog__ctfn_");
    if (!cname || !jog_name) {
        jog_lower_fail(L, expr ? expr->pos : (JogPos){0}, "out of memory");
        return 0;
    }
    idx = jog_lower_isolate_expr(L, expr, type, cname, jog_name);
    if (idx < 0) {
        return 0;
    }
    if (!prepare(L, idx)) {
        return 0;
    }
    if (!jir_interp_call(L->module, (uint32_t)idx, NULL, 0, JOG_COMPTIME_STEPS, out, L->program->diag)) {
        L->failed = 1;
        return 0;
    }
    return 1;
}

static void fail_embed(Lower *L, JogPos pos, JogEmbedStatus status, const char *literal) {
    const char *text = literal ? literal : "";
    switch (status) {
    case JOG_EMBED_OK:
        return;
    case JOG_EMBED_OOM:
        jog_lower_fail(L, pos, "out of memory");
        return;
    case JOG_EMBED_COUNT:
        jog_lower_fail(L, pos, "@embed expects one argument");
        return;
    case JOG_EMBED_LITERAL:
        jog_lower_fail(L, pos, "@embed expects a string literal");
        return;
    case JOG_EMBED_RELATIVE:
        jog_lower_fail(L, pos, "@embed path must be a relative file path");
        return;
    case JOG_EMBED_ESCAPE:
        jog_lower_fail(L, pos, "@embed path escapes the module");
        return;
    case JOG_EMBED_DIR:
        jog_lower_fail(L, pos, "cannot embed '%s': path is a directory", text);
        return;
    case JOG_EMBED_MISSING:
        jog_lower_fail(L, pos, "cannot embed '%s': file is missing", text);
        return;
    }
}

static bool lower_embed(Lower *L, JogNode *node, JirOperand *out) {
    const char *literal = NULL;
    size_t literal_len = 0;
    const char *full = NULL;
    CtFile file;
    JogEmbedStatus status;
    JogType *type;
    JirOperand bytes;
    uint32_t dst = 0;
    int arg;
    arg = jog_embed_literal(node, &literal, &literal_len);
    if (arg != JOG_EMBED_OK) {
        fail_embed(L, node->pos, (JogEmbedStatus)arg, NULL);
        return true;
    }
    status = jog_embed_resolve(L->program, node->pos.file, literal, literal_len, &full);
    if (status == JOG_EMBED_OK) {
        status = jog_embed_stat(full);
    }
    if (status != JOG_EMBED_OK) {
        fail_embed(L, node->pos, status, literal);
        return true;
    }
    if (!L->func) {
        jog_lower_fail(L, node->pos, "@embed is outside a function");
        return true;
    }
    memset(&file, 0, sizeof(file));
    if (!L->program || !L->program->arena || !ct_file_read(L->program->arena, NULL, full, &file)) {
        fail_embed(L, node->pos, JOG_EMBED_MISSING, literal);
        return true;
    }
    type = node->type ? jog_lower_type(L, node->type) : NULL;
    if (!type || type->kind != JOG_TY_BYTES) {
        jog_lower_fail(L, node->pos, "compile-time embed has no type");
        return true;
    }
    bytes = jir_string(type, file.data ? file.data : "", file.size);
    if (!jog_lower_new_local(L, NULL, type, &dst)) {
        return true;
    }
    jog_lower_copy(L, dst, bytes, node->pos);
    *out = jog_lower_local_op(L, dst);
    return true;
}

bool jog_lower_comptime_builtin(Lower *L, JogNode *node, JirOperand *out) {
    const char *name;
    JogNode *arg;
    JogNode *expr;
    JogType *type;
    const char *jname;
    const uint8_t *bytes = NULL;
    if (out) {
        *out = jir_none();
    }
    if (!L || !node || node->kind != JOG_NODE_BUILTIN || !out) {
        return 0;
    }
    name = node->as.builtin.name ? node->as.builtin.name : "";
    if (strcmp(name, "embed") == 0) {
        return lower_embed(L, node, out);
    }
    if (strcmp(name, "comptime") != 0) {
        return 0;
    }
    if (node->as.builtin.args.count != 1 || !node->as.builtin.args.items) {
        jog_lower_fail(L, node->pos, "@comptime expects one argument");
        return 1;
    }
    arg = node->as.builtin.args.items[0];
    expr = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    if (!expr || (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name)) {
        jog_lower_fail(L, node->pos, "@comptime expects one argument");
        return 1;
    }
    type = node->type ? jog_lower_type(L, node->type) : NULL;
    if (!type) {
        jog_lower_fail(L, node->pos, "compile-time expression has no type");
        return 1;
    }
    jname = dot_comptime_name(L);
    if (!eval_expr(L, expr, type, jname, &bytes)) {
        return 1;
    }
    *out = materialize(L, type, bytes, type->has_layout ? (size_t)type->size : 0, node->pos);
    return 1;
}

static int deps_ready(const uint8_t *deps, const uint8_t *done, int n, int i) {
    int j;
    for (j = 0; j < n; j++) {
        if (deps[(size_t)i * (size_t)n + (size_t)j] && !done[j]) {
            return 0;
        }
    }
    return 1;
}

bool jog_lower_comptime_vals(Lower *L) {
    size_t i;
    size_t n = 0;
    size_t ng;
    size_t placed;
    int *map = NULL;
    int *order = NULL;
    uint8_t *deps = NULL;
    uint8_t *done = NULL;
    GlobalBind **cands = NULL;
    int ok = 1;
    if (!L || !L->module || L->failed) {
        return 0;
    }
    ng = L->module->globals.count;
    for (i = 0; i < L->globals.count; i++) {
        GlobalBind *bind = ct_da_at(&L->globals, i);
        JirGlobal *global;
        JogType *type;
        if (!bind || !bind->runtime || bind->part >= 0 || !bind->decl || bind->decl->kind != JOG_NODE_VAL ||
            bind->index < 0) {
            continue;
        }
        global = jir_global(L->module, (uint32_t)bind->index);
        type = global ? global->type : NULL;
        if (!type || !jog_type_layout(L->program->types, type) || !type->has_layout || type->size == 0 ||
            type->size > SIZE_MAX) {
            continue;
        }
        if (type_has_ptr(type, 0)) {
            continue;
        }
        n++;
    }
    if (n == 0) {
        return 1;
    }
    if (n > 4096) {
        jog_lower_fail(L, (JogPos){0}, "compile-time initializer is too large");
        return 0;
    }
    map = malloc((ng ? ng : 1) * sizeof(*map));
    cands = calloc(n, sizeof(*cands));
    order = calloc(n, sizeof(*order));
    deps = calloc(n * n, 1);
    done = calloc(n, 1);
    if (!map || !cands || !order || !deps || !done) {
        jog_lower_fail(L, (JogPos){0}, "out of memory");
        ok = 0;
    }
    if (ok) {
        size_t c = 0;
        for (i = 0; i < ng; i++) {
            map[i] = -1;
        }
        for (i = 0; i < L->globals.count; i++) {
            GlobalBind *bind = ct_da_at(&L->globals, i);
            JirGlobal *global;
            JogType *type;
            if (!bind || !bind->runtime || bind->part >= 0 || !bind->decl || bind->decl->kind != JOG_NODE_VAL ||
                bind->index < 0) {
                continue;
            }
            global = jir_global(L->module, (uint32_t)bind->index);
            type = global ? global->type : NULL;
            if (!type || !type->has_layout || type->size == 0 || type->size > SIZE_MAX || type_has_ptr(type, 0)) {
                continue;
            }
            if ((size_t)bind->index < ng) {
                map[bind->index] = (int)c;
            }
            cands[c++] = bind;
        }
        for (i = 0; i < n && !L->failed; i++) {
            Walk w;
            JogNode *init = cands[i]->decl->as.binding.init;
            memset(&w, 0, sizeof(w));
            w.L = L;
            w.self = (int)i;
            w.n = (int)n;
            w.deps = deps;
            w.map = map;
            w.nglobal = ng;
            walk(&w, init);
        }
        if (L->failed) {
            ok = 0;
        }
    }
    placed = 0;
    while (ok && placed < n) {
        int progress = 0;
        for (i = 0; i < n; i++) {
            if (done[i] || !deps_ready(deps, done, (int)n, (int)i)) {
                continue;
            }
            order[placed++] = (int)i;
            done[i] = 1;
            progress = 1;
            break;
        }
        if (!progress) {
            JogPos pos = cands[0] && cands[0]->decl ? cands[0]->decl->pos : (JogPos){0};
            for (i = 0; i < n; i++) {
                if (!done[i] && cands[i] && cands[i]->decl) {
                    pos = cands[i]->decl->pos;
                    break;
                }
            }
            jog_lower_fail(L, pos, "global initializer cycle");
            ok = 0;
        }
    }
    for (i = 0; ok && i < placed && !L->failed; i++) {
        GlobalBind *bind = cands[order[i]];
        JirGlobal *global = jir_global(L->module, (uint32_t)bind->index);
        JogNode *init = bind->decl->as.binding.init;
        JogType *type = global ? global->type : NULL;
        const char *jname = bind->symbol && bind->symbol->name ? bind->symbol->name : "val";
        const uint8_t *bytes = NULL;
        if (!init || !type) {
            jog_lower_fail(L, bind->decl->pos, "compile-time initializer is missing");
            ok = 0;
            break;
        }
        if (!eval_expr(L, init, type, jname, &bytes)) {
            ok = 0;
            break;
        }
        if (type->size > 0 && !bytes) {
            jog_lower_fail(L, init->pos, "compile-time value is missing");
            ok = 0;
            break;
        }
        global->data = type->size ? bytes : NULL;
        global->data_len = (size_t)type->size;
        bind->runtime = 0;
    }
    free(map);
    free(cands);
    free(order);
    free(deps);
    free(done);
    return ok && !L->failed;
}
