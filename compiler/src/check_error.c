#include "check_error.h"

#include "check_struct.h"

#include "str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* error? is an option whose element is the universe interface error.
   A tuple field is .0, .1, ... with no implicit conversion between tuples.
   Field access dereferences one pointer, so p.0 is (*p).0.
   Destructuring with one name is grouping: val (a) = e is val a = e.
   (a, err) = f() writes lvalues: names, _, *p, indexes, fields, tuple
   fields, and nested tuples of those.
   panic is a builtin only when no binding named panic is in scope. */

static bool is_poison(const JogType *type) {
    return type && type->kind == JOG_TY_ERROR;
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static void type_text(Check *ck, const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!type) {
        snprintf(buf, cap, "<null type>");
        return;
    }
    if (ck && type == jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES)) {
        snprintf(buf, cap, "str");
        return;
    }
    ct_sb_init(&sb);
    jog_type_print(type, &sb);
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text ? text : "?");
    ct_sb_free(&sb);
}

static void fail_mismatch(Check *ck, JogPos pos, const JogType *want, const JogType *got) {
    char a[160];
    char b[160];
    type_text(ck, want, a, sizeof(a));
    type_text(ck, got, b, sizeof(b));
    jog_check_fail(ck, pos, "type mismatch: expected %s, got %s", a, b);
}

static void fail_types(Check *ck, JogPos pos, const char *what, const JogType *left, const JogType *right) {
    char a[160];
    char b[160];
    type_text(ck, left, a, sizeof(a));
    type_text(ck, right, b, sizeof(b));
    jog_check_fail(ck, pos, "%s %s and %s", what, a, b);
}

static JogType *poison(Check *ck) {
    return jog_check_error_type(ck);
}

static JogType *builtin(Check *ck, JogBuiltin which) {
    JogType *type = ck && ck->program ? jog_type_builtin(ck->program->types, which) : NULL;
    if (!type && ck) {
        jog_check_oom(ck);
    }
    return type;
}

static bool same_type(const JogType *a, const JogType *b) {
    if (!a || !b) {
        return false;
    }
    if (a->kind == JOG_TY_ERROR || b->kind == JOG_TY_ERROR) {
        return true;
    }
    return a == b;
}

static JogType *underlying(JogType *type) {
    while (type && type->kind == JOG_TY_DISTINCT) {
        type = type->as.nominal.base;
    }
    return type;
}

/* The std error interface, including a distinct type over it. */
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

static const char *name_at(const JogNode *node, size_t index) {
    if (!node || !node->as.binding.names.items || index >= node->as.binding.names.count) {
        return NULL;
    }
    return node->as.binding.names.items[index];
}

static bool remember_symbols(Check *ck, JogNode *node, JogSymbol **symbols, size_t count) {
    if (count == 0) {
        node->bound_symbols = NULL;
        node->bound_symbol_count = 0;
        return true;
    }
    node->bound_symbols = ct_arena_alloc(ck->program->arena, count * sizeof(JogSymbol *), _Alignof(JogSymbol *));
    if (!node->bound_symbols) {
        jog_check_oom(ck);
        return false;
    }
    memcpy(node->bound_symbols, symbols, count * sizeof(JogSymbol *));
    node->bound_symbol_count = count;
    return true;
}

static JogType *require_tuple(Check *ck, JogPos pos, JogType *type, size_t expect) {
    char text[160];
    if (!type || ck->stop) {
        return type;
    }
    if (is_poison(type)) {
        return type;
    }
    if (type->kind != JOG_TY_TUPLE) {
        type_text(ck, type, text, sizeof(text));
        jog_check_fail(ck, pos, "destructuring expects a tuple, got %s", text);
        return poison(ck);
    }
    if (type->as.tuple.count != expect) {
        jog_check_fail(ck, pos, "destructuring expects %" PRIu64 " values, got %" PRIu64, (uint64_t)expect,
                       (uint64_t)type->as.tuple.count);
        return poison(ck);
    }
    return type;
}

static bool assignable(const JogSymbol *symbol) {
    return symbol && (symbol->kind == JOG_SYM_VARIABLE || symbol->kind == JOG_SYM_PARAMETER);
}

static JogSymbol *lookup_here(CheckJob *job, JogBlockScope *scope, const char *name) {
    JogSymbol *found = jog_check_body_lookup(job, name);
    if (found) {
        return found;
    }
    if (scope) {
        found = jog_block_scope_lookup(scope, name);
        if (found) {
            return found;
        }
    }
    if (job) {
        return jog_check_lookup_file(job, name);
    }
    return NULL;
}

int jog_check_error_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *target) {
    JogNode *callee;
    JogNode *arg;
    JogNode *value;
    JogType *str;
    JogType *got;
    JogType *unit;
    const char *name;
    if (!ck || !call || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_IDENT || callee->as.ident.type_args.count > 0) {
        return 0;
    }
    name = callee->as.ident.name;
    if (!name || strcmp(name, "panic") != 0) {
        return 0;
    }
    if (lookup_here(job, scope, name)) {
        return 0;
    }
    if (call->as.call.trailing || call->as.call.contexts.count > 0) {
        jog_check_fail(ck, call->pos, "panic expects 1 argument");
        call->type = poison(ck);
        return ck->stop ? -1 : 1;
    }
    if (call->as.call.args.count != 1) {
        jog_check_fail(ck, call->pos, "panic expects 1 argument");
        call->type = poison(ck);
        return ck->stop ? -1 : 1;
    }
    arg = list_at(call->as.call.args, 0);
    if (!arg || arg->kind != JOG_NODE_ARG || arg->as.arg.name) {
        jog_check_fail(ck, arg ? arg->pos : call->pos, "panic expects 1 argument");
        call->type = poison(ck);
        return ck->stop ? -1 : 1;
    }
    str = builtin(ck, JOG_BUILTIN_CONST_BYTES);
    if (!str) {
        return -1;
    }
    value = arg->as.arg.value;
    got = jog_check_expr(ck, job, scope, value, str);
    if (!got || ck->stop) {
        return -1;
    }
    if (!is_poison(got) && !same_type(got, str)) {
        fail_mismatch(ck, value ? value->pos : call->pos, str, got);
        call->type = poison(ck);
        return ck->stop ? -1 : 1;
    }
    if (is_poison(got)) {
        call->type = got;
        return 1;
    }
    unit = jog_check_unit_type(ck);
    if (target && !is_poison(target)) {
        call->type = target;
    } else {
        call->type = unit;
    }
    jog_check_body_unreachable(job);
    return ck->stop ? -1 : 1;
}

JogType *jog_check_error_null(Check *ck, JogNode *node, JogType *target) {
    JogType *null_ty;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    if (target && is_poison(target)) {
        node->type = target;
        return target;
    }
    if (target && target->kind == JOG_TY_OPTION) {
        node->type = target;
        return target;
    }
    if (!target) {
        null_ty = builtin(ck, JOG_BUILTIN_NULL);
        node->type = null_ty;
        return null_ty;
    }
    fail_mismatch(ck, node->pos, target, builtin(ck, JOG_BUILTIN_NULL));
    node->type = poison(ck);
    return node->type;
}

JogType *jog_check_error_tuple(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target) {
    size_t count;
    size_t i;
    JogType **elems;
    JogType *built;
    JogType *hint;
    bool bad;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    count = node->as.tuple.elems.count;
    if (count < 2) {
        jog_check_fail(ck, node->pos, "a tuple needs at least 2 elements");
        node->type = poison(ck);
        return node->type;
    }
    hint = NULL;
    if (target && target->kind == JOG_TY_TUPLE && target->as.tuple.count == count) {
        hint = target;
    }
    elems = ct_arena_alloc(ck->program->arena, count * sizeof(JogType *), _Alignof(JogType *));
    if (!elems) {
        jog_check_oom(ck);
        return NULL;
    }
    bad = false;
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *elem = list_at(node->as.tuple.elems, i);
        JogType *want = hint && hint->as.tuple.elems ? hint->as.tuple.elems[i] : NULL;
        JogType *got = jog_check_expr(ck, job, scope, elem, want);
        if (!got) {
            return NULL;
        }
        if (is_poison(got)) {
            bad = true;
        }
        elems[i] = got;
    }
    if (ck->stop) {
        return NULL;
    }
    if (bad) {
        node->type = poison(ck);
        return node->type;
    }
    if (hint) {
        node->type = hint;
        return hint;
    }
    built = jog_type_tuple(ck->program->types, elems, count);
    if (!built) {
        jog_check_oom(ck);
        return NULL;
    }
    node->type = built;
    return built;
}

int jog_check_error_field(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *member, JogType **out) {
    JogType *base;
    JogType *field;
    char text[160];
    if (!member || member->kind != JOG_NODE_MEMBER || member->as.member.name) {
        return 0;
    }
    if (!ck || !out || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    if (member->as.member.safe) {
        jog_check_fail(ck, member->pos, "operator '?.' is not checked yet");
        *out = poison(ck);
        member->type = *out;
        return ck->stop ? -1 : 1;
    }
    base = jog_check_expr(ck, job, scope, member->as.member.base, NULL);
    if (!base || ck->stop) {
        return -1;
    }
    if (is_poison(base)) {
        *out = base;
        member->type = base;
        return 1;
    }
    /* One pointer, the same rule as a struct field: p.0 is (*p).0. */
    if (base->kind == JOG_TY_PTR && base->as.elem.elem) {
        base = base->as.elem.elem;
    }
    if (base->kind != JOG_TY_TUPLE) {
        type_text(ck, base, text, sizeof(text));
        jog_check_fail(ck, member->pos, "tuple field on %s", text);
        *out = poison(ck);
        member->type = *out;
        return ck->stop ? -1 : 1;
    }
    if ((size_t)member->as.member.tuple_index >= base->as.tuple.count || !base->as.tuple.elems) {
        jog_check_fail(ck, member->pos, "tuple field %u is out of range", member->as.member.tuple_index);
        *out = poison(ck);
        member->type = *out;
        return ck->stop ? -1 : 1;
    }
    field = base->as.tuple.elems[member->as.member.tuple_index];
    *out = field;
    member->type = field;
    return 1;
}

/* 1 when elem declares message(): str in its package. 0 when it does not.
   -1 when the checker stopped. Value and pointer receivers both count. */
static int message_ok(Check *ck, JogType *elem) {
    size_t p;
    int found = 0;
    if (!ck || !ck->program || !elem || !elem->as.nominal.decl) {
        return 0;
    }
    if (elem->kind != JOG_TY_STRUCT && elem->kind != JOG_TY_ENUM) {
        return 0;
    }
    for (p = 0; p < ck->program->package_count && !ck->stop; p++) {
        JogPackage *pkg = ck->program->packages ? ck->program->packages[p] : NULL;
        size_t f;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !ck->stop; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file;
            size_t d;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count && !ck->stop; d++) {
                JogNode *fn = list_at(file->as.file.decls, d);
                JogSymbol *sym;
                JogType *on = NULL;
                JogType *fn_ty;
                const char *want_pkg;
                const char *have_pkg;
                if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.receiver || !fn->as.fn.name) {
                    continue;
                }
                if (strcmp(fn->as.fn.name, "message") != 0) {
                    continue;
                }
                sym = fn->symbol;
                if (!sym) {
                    continue;
                }
                want_pkg = elem->as.nominal.package ? elem->as.nominal.package : "";
                have_pkg = sym->package && sym->package->path ? sym->package->path : "";
                if (strcmp(want_pkg, have_pkg) != 0) {
                    continue;
                }
                if (!sym->type) {
                    if (ck->current_job && ck->current_job->symbol == sym) {
                        continue;
                    }
                    if (!jog_check_wait(ck, ck->current_job, sym)) {
                        return -1;
                    }
                }
                if (!sym->type || sym->type->kind != JOG_TY_FN || sym->type_param_count > 0) {
                    continue;
                }
                jog_method_receiver(fn, &on, NULL);
                if (on != elem) {
                    continue;
                }
                fn_ty = sym->type;
                if (fn_ty->as.fn.variadic || fn_ty->as.fn.context_count != 0 || fn_ty->as.fn.param_count != 1) {
                    continue;
                }
                if (fn_ty->as.fn.ret != jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES)) {
                    continue;
                }
                found = 1;
            }
        }
    }
    if (ck->stop) {
        return -1;
    }
    return found;
}

/* 1 when type is the error interface or a sentinel pointer. 0 when it is not.
   -1 when the checker stopped. */
static int sentinel_type(Check *ck, JogType *type) {
    JogType *elem;
    if (is_error_interface(type)) {
        return 1;
    }
    if (!type || type->kind != JOG_TY_PTR || !type->as.elem.elem) {
        return 0;
    }
    elem = type->as.elem.elem;
    if (elem->kind != JOG_TY_STRUCT && elem->kind != JOG_TY_ENUM) {
        return 0;
    }
    return message_ok(ck, elem);
}

/* 1 when one side is error? and the other is a sentinel. 0 otherwise.
   -1 when the checker stopped. Two error? values are not a sentinel pair. */
static int sentinel_cmp(Check *ck, JogType *left, JogType *right) {
    if (is_error_option(left) && !is_error_option(right)) {
        return sentinel_type(ck, right);
    }
    if (is_error_option(right) && !is_error_option(left)) {
        return sentinel_type(ck, left);
    }
    return 0;
}

int jog_check_error_compare(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out) {
    JogType *boolean;
    JogTokenKind op;
    bool left_null;
    bool right_null;
    bool left_opt;
    bool right_opt;
    if (!ck || !node || !out || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    op = node->as.binary.op;
    if (op != JOG_TOK_EQ && op != JOG_TOK_NEQ) {
        return 0;
    }
    left_null = left && left->kind == JOG_TY_NULL;
    right_null = right && right->kind == JOG_TY_NULL;
    left_opt = left && left->kind == JOG_TY_OPTION;
    right_opt = right && right->kind == JOG_TY_OPTION;
    if (!left_null && !right_null && !left_opt && !right_opt) {
        return 0;
    }
    boolean = builtin(ck, JOG_BUILTIN_BOOL);
    if (!boolean) {
        return -1;
    }
    if (left_null && right_null) {
        jog_check_fail(ck, node->pos, "null needs an option type");
        *out = poison(ck);
        node->type = *out;
        return ck->stop ? -1 : 1;
    }
    if ((left_null && right_opt) || (right_null && left_opt)) {
        *out = boolean;
        node->type = boolean;
        return 1;
    }
    if (left_opt && right_opt && same_type(left, right) && jog_type_option_is_niche(left)) {
        *out = boolean;
        node->type = boolean;
        return 1;
    }
    {
        int sent = sentinel_cmp(ck, left, right);
        if (sent < 0) {
            return -1;
        }
        if (sent > 0) {
            *out = boolean;
            node->type = boolean;
            return 1;
        }
    }
    if (left_opt || right_opt || left_null || right_null) {
        if (left_opt && right_opt && !same_type(left, right)) {
            fail_types(ck, node->pos, "mismatched types", left, right);
        } else if ((left_null && !right_opt) || (right_null && !left_opt)) {
            fail_types(ck, node->pos, "mismatched types", left, right);
        } else {
            char text[160];
            type_text(ck, left_opt ? left : right, text, sizeof(text));
            jog_check_fail(ck, node->pos, "operator '%s' does not apply to %s", jog_token_spelling(op), text);
        }
        *out = poison(ck);
        node->type = *out;
        return ck->stop ? -1 : 1;
    }
    return 0;
}

int jog_check_error_expect(Check *ck, JogNode *node, JogType *got, JogType *want, JogType **out) {
    int sent;
    (void)node;
    if (out) {
        *out = NULL;
    }
    if (!ck || ck->stop || !got || !want || is_poison(got) || is_poison(want)) {
        return ck && ck->stop ? -1 : 0;
    }
    if (is_error_option(want) && is_error_interface(got)) {
        if (out) {
            *out = want;
        }
        return 1;
    }
    if (!is_error_option(want) && !is_error_interface(want)) {
        return 0;
    }
    if (got->kind != JOG_TY_PTR) {
        return 0;
    }
    sent = sentinel_type(ck, got);
    if (sent < 0) {
        return -1;
    }
    if (sent == 0) {
        return 0;
    }
    if (out) {
        *out = want;
    }
    return 1;
}

static JogSymbolKind binding_kind(JogNodeKind kind) {
    if (kind == JOG_NODE_VAL) {
        return JOG_SYM_CONSTANT;
    }
    return JOG_SYM_VARIABLE;
}

static void bind_elements(Check *ck, CheckJob *job, JogNode *node, JogType *tuple, bool from_init) {
    size_t count;
    size_t i;
    JogSymbol **symbols;
    JogSymbolKind kind;
    if (!tuple || tuple->kind != JOG_TY_TUPLE || ck->stop) {
        return;
    }
    count = node->as.binding.names.count;
    symbols = ct_arena_alloc(ck->program->arena, count * sizeof(JogSymbol *), _Alignof(JogSymbol *));
    if (!symbols) {
        jog_check_oom(ck);
        return;
    }
    memset(symbols, 0, count * sizeof(JogSymbol *));
    kind = binding_kind(node->kind);
    node->symbol = NULL;
    for (i = 0; i < count && !ck->stop; i++) {
        const char *name = name_at(node, i);
        JogType *elem = tuple->as.tuple.elems ? tuple->as.tuple.elems[i] : NULL;
        JogSymbol *symbol;
        if (is_discard(name)) {
            continue;
        }
        symbol = jog_check_body_bind(job, node, kind, name, elem);
        symbols[i] = symbol;
        if (!node->symbol) {
            node->symbol = symbol;
        }
        if (!symbol) {
            continue;
        }
        if (from_init || jog_check_type_has_zero(elem)) {
            jog_check_body_assigned(job, symbol);
        }
    }
    if (!ck->stop) {
        (void)remember_symbols(ck, node, symbols, count);
    }
    node->type = tuple;
}

void jog_check_error_binding(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *annotated = NULL;
    JogType *got = NULL;
    size_t count;
    if (!ck || !node || ck->stop) {
        return;
    }
    count = node->as.binding.names.count;
    if (node->kind == JOG_NODE_VAL && !node->as.binding.init) {
        jog_check_fail(ck, node->pos, "val requires an initializer");
    }
    if (node->as.binding.type) {
        annotated = jog_check_resolve_type(ck, job, node->as.binding.type);
        if (!annotated || ck->stop) {
            return;
        }
        annotated = require_tuple(ck, node->as.binding.type->pos, annotated, count);
        if (!annotated || ck->stop) {
            return;
        }
    }
    if (!annotated && !node->as.binding.init) {
        jog_check_fail(ck, node->pos, "missing type");
        return;
    }
    if (node->as.binding.init) {
        got = jog_check_expr(ck, job, scope, node->as.binding.init, annotated && !is_poison(annotated) ? annotated : NULL);
        if (!got || ck->stop) {
            return;
        }
        if (!annotated) {
            got = require_tuple(ck, node->as.binding.init->pos, got, count);
        } else if (!is_poison(got) && !is_poison(annotated) && !same_type(got, annotated)) {
            fail_mismatch(ck, node->as.binding.init->pos, annotated, got);
            got = poison(ck);
        }
        if (!got || ck->stop) {
            return;
        }
    }
    if (annotated && !is_poison(annotated)) {
        bind_elements(ck, job, node, annotated, node->as.binding.init != NULL);
        return;
    }
    if (got && !is_poison(got)) {
        bind_elements(ck, job, node, got, true);
        return;
    }
    node->type = poison(ck);
}

static JogSymbol *top_symbol(CheckJob *job, const char *name) {
    size_t i;
    if (!job || !job->symbols || is_discard(name)) {
        return NULL;
    }
    for (i = 0; i < job->symbol_count; i++) {
        JogSymbol *symbol = job->symbols[i];
        if (symbol && symbol->name && strcmp(symbol->name, name) == 0) {
            return symbol;
        }
    }
    return NULL;
}

JogType *jog_check_error_top_binding(Check *ck, CheckJob *job, JogNode *node) {
    JogType *annotated = NULL;
    JogType *got = NULL;
    JogType *tuple;
    size_t count;
    size_t i;
    JogSymbol **symbols;
    if (!ck || !node || ck->stop) {
        return NULL;
    }
    count = node->as.binding.names.count;
    if (node->kind == JOG_NODE_VAL && !node->as.binding.init) {
        jog_check_fail(ck, node->pos, "val requires an initializer");
    }
    if (node->as.binding.type) {
        annotated = jog_check_resolve_type(ck, job, node->as.binding.type);
        if (!annotated || ck->stop) {
            return NULL;
        }
        if (!is_poison(annotated)) {
            annotated = require_tuple(ck, node->as.binding.type->pos, annotated, count);
            if (!annotated || ck->stop) {
                return NULL;
            }
        }
    }
    if (!annotated && !node->as.binding.init) {
        jog_check_fail(ck, node->pos, "missing type");
        return poison(ck);
    }
    if (node->as.binding.init) {
        JogType *hint = annotated && !is_poison(annotated) ? annotated : NULL;
        got = jog_check_expr(ck, job, NULL, node->as.binding.init, hint);
        if (ck->stop) {
            return NULL;
        }
        if (!annotated) {
            got = require_tuple(ck, node->as.binding.init->pos, got, count);
        } else if (got && !is_poison(got) && annotated && !is_poison(annotated) && !same_type(got, annotated)) {
            fail_mismatch(ck, node->as.binding.init->pos, annotated, got);
            got = poison(ck);
        }
        if (ck->stop) {
            return NULL;
        }
    }
    tuple = annotated && !is_poison(annotated) ? annotated : got;
    if (!tuple || is_poison(tuple) || tuple->kind != JOG_TY_TUPLE) {
        return tuple && is_poison(tuple) ? tuple : poison(ck);
    }
    symbols = ct_arena_alloc(ck->program->arena, count * sizeof(JogSymbol *), _Alignof(JogSymbol *));
    if (!symbols) {
        jog_check_oom(ck);
        return NULL;
    }
    memset(symbols, 0, count * sizeof(JogSymbol *));
    for (i = 0; i < count; i++) {
        const char *name = name_at(node, i);
        JogSymbol *symbol = top_symbol(job, name);
        JogType *elem = tuple->as.tuple.elems ? tuple->as.tuple.elems[i] : NULL;
        if (is_discard(name)) {
            continue;
        }
        if (!symbol) {
            jog_check_fail(ck, node->pos, "undefined name '%s'", name ? name : "?");
            continue;
        }
        symbol->type = elem;
        symbols[i] = symbol;
    }
    if (!remember_symbols(ck, node, symbols, count)) {
        return NULL;
    }
    node->type = tuple;
    return tuple;
}

typedef enum DestPlace {
    DEST_NO = 0,
    DEST_MUT,
    DEST_CONST
} DestPlace;

/* One destructuring slot. A nested slot is itself a tuple of lvalues. */
typedef struct Expect {
    JogNode *target;
    JogType *type;
    bool discard;
    bool writable;
    bool nested;
    bool checked; /* the value written here was a tuple literal of this shape */
    struct Expect *kids;
    size_t kid_count;
} Expect;

static DestPlace dest_place(const JogNode *node) {
    JogType *base_ty;
    if (!node) {
        return DEST_NO;
    }
    if (node->kind == JOG_NODE_IDENT && node->symbol) {
        if (node->symbol->kind == JOG_SYM_VARIABLE || node->symbol->kind == JOG_SYM_PARAMETER) {
            return DEST_MUT;
        }
        if (node->symbol->kind == JOG_SYM_CONSTANT) {
            return DEST_CONST;
        }
        return DEST_NO;
    }
    if (node->kind == JOG_NODE_MEMBER) {
        base_ty = node->as.member.base ? node->as.member.base->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? DEST_CONST : DEST_MUT;
        }
        return dest_place(node->as.member.base);
    }
    if (node->kind == JOG_NODE_INDEX) {
        base_ty = node->as.index.base ? node->as.index.base->type : NULL;
        if (!base_ty) {
            return DEST_NO;
        }
        if (base_ty->kind == JOG_TY_PTR || base_ty->kind == JOG_TY_SLICE || base_ty->kind == JOG_TY_BYTES) {
            return base_ty->is_const ? DEST_CONST : DEST_MUT;
        }
        if (base_ty->kind == JOG_TY_ARRAY || base_ty->kind == JOG_TY_BYTES_N) {
            return dest_place(node->as.index.base);
        }
        return DEST_NO;
    }
    if (node->kind == JOG_NODE_UNARY && node->as.unary.op == JOG_TOK_STAR) {
        base_ty = node->as.unary.operand ? node->as.unary.operand->type : NULL;
        if (base_ty && base_ty->kind == JOG_TY_PTR) {
            return base_ty->is_const ? DEST_CONST : DEST_MUT;
        }
    }
    return DEST_NO;
}

static void reject_place(Check *ck, JogNode *target, DestPlace place) {
    if (place == DEST_MUT) {
        return;
    }
    if (target && target->kind == JOG_NODE_IDENT) {
        const char *name = target->symbol && target->symbol->name ? target->symbol->name : target->as.ident.name;
        jog_check_fail(ck, target->pos, "cannot assign to '%s'", name ? name : "?");
        return;
    }
    if (target && target->kind == JOG_NODE_MEMBER && target->as.member.name) {
        jog_check_fail(ck, target->pos, "cannot assign to field '%s'", target->as.member.name);
        return;
    }
    if (place == DEST_CONST) {
        jog_check_fail(ck, target ? target->pos : (JogPos){0}, "cannot assign through a const view");
        return;
    }
    jog_check_fail(ck, target ? target->pos : (JogPos){0}, "cannot assign to this value");
}

/* 1 when the slot is filled. -1 when the checker has stopped. */
static int fill_expect(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *target, Expect *out) {
    memset(out, 0, sizeof(*out));
    out->target = target;
    if (!target || ck->stop) {
        return ck && ck->stop ? -1 : 1;
    }
    if (target->kind == JOG_NODE_IDENT && is_discard(target->as.ident.name)) {
        out->discard = true;
        return 1;
    }
    if (target->kind == JOG_NODE_IDENT) {
        JogSymbol *symbol = lookup_here(job, scope, target->as.ident.name);
        if (!symbol) {
            jog_check_fail(ck, target->pos, "undefined name '%s'", target->as.ident.name ? target->as.ident.name : "?");
            return ck->stop ? -1 : 1;
        }
        if (!jog_check_wait(ck, job, symbol)) {
            return -1;
        }
        target->symbol = symbol;
        target->type = symbol->type;
        out->type = symbol->type;
        if (!assignable(symbol)) {
            jog_check_fail(ck, target->pos, "cannot assign to '%s'", symbol->name ? symbol->name : "?");
            return ck->stop ? -1 : 1;
        }
        out->writable = true;
        return 1;
    }
    if (target->kind == JOG_NODE_TUPLE) {
        size_t n = target->as.tuple.elems.count;
        size_t i;
        bool complete = true;
        JogType **elems;
        if (n < 2) {
            jog_check_fail(ck, target->pos, "a tuple needs at least 2 elements");
            return ck->stop ? -1 : 1;
        }
        out->kids = ct_arena_alloc(ck->program->arena, n * sizeof(Expect), _Alignof(Expect));
        elems = ct_arena_alloc(ck->program->arena, n * sizeof(JogType *), _Alignof(JogType *));
        if (!out->kids || !elems) {
            jog_check_oom(ck);
            return -1;
        }
        memset(out->kids, 0, n * sizeof(Expect));
        memset(elems, 0, n * sizeof(JogType *));
        out->nested = true;
        out->kid_count = n;
        for (i = 0; i < n && !ck->stop; i++) {
            if (fill_expect(ck, job, scope, list_at(target->as.tuple.elems, i), &out->kids[i]) < 0) {
                return -1;
            }
            if (out->kids[i].discard || !out->kids[i].type || is_poison(out->kids[i].type)) {
                complete = false;
            } else {
                elems[i] = out->kids[i].type;
            }
        }
        if (ck->stop) {
            return -1;
        }
        if (complete) {
            out->type = jog_type_tuple(ck->program->types, elems, n);
            if (!out->type) {
                jog_check_oom(ck);
                return -1;
            }
            target->type = out->type;
        }
        return 1;
    }
    out->type = jog_check_expr(ck, job, scope, target, NULL);
    if (!out->type || ck->stop) {
        return -1;
    }
    if (!is_poison(out->type)) {
        DestPlace place = dest_place(target);
        if (place != DEST_MUT) {
            reject_place(ck, target, place);
            return ck->stop ? -1 : 1;
        }
        out->writable = true;
    }
    return ck->stop ? -1 : 1;
}

static JogType *want_of(const Expect *slot) {
    if (!slot || slot->discard || !slot->writable || !slot->type || is_poison(slot->type)) {
        return NULL;
    }
    return slot->type;
}

/* A tuple literal is checked element by element so an untyped integer adopts
   the slot type. Any other value is checked on its own, then matched. */
static JogType *check_against(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, Expect *slots, size_t n) {
    size_t i;
    JogType **elems;
    JogType *built;
    if (!node || ck->stop) {
        return NULL;
    }
    if (node->kind == JOG_NODE_TUPLE && node->as.tuple.elems.count == n) {
        elems = ct_arena_alloc(ck->program->arena, n * sizeof(JogType *), _Alignof(JogType *));
        if (!elems) {
            jog_check_oom(ck);
            return NULL;
        }
        for (i = 0; i < n && !ck->stop; i++) {
            JogNode *child = list_at(node->as.tuple.elems, i);
            JogType *got;
            if (slots[i].nested) {
                slots[i].checked = child && child->kind == JOG_NODE_TUPLE &&
                                   child->as.tuple.elems.count == slots[i].kid_count;
                got = check_against(ck, job, scope, child, slots[i].kids, slots[i].kid_count);
            } else {
                got = jog_check_expr(ck, job, scope, child, want_of(&slots[i]));
            }
            if (!got || ck->stop) {
                return NULL;
            }
            elems[i] = got;
        }
        if (ck->stop) {
            return NULL;
        }
        built = jog_type_tuple(ck->program->types, elems, n);
        if (!built) {
            jog_check_oom(ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    built = jog_check_expr(ck, job, scope, node, NULL);
    if (!built || ck->stop) {
        return NULL;
    }
    return require_tuple(ck, node->pos, built, n);
}

static void accept_slots(Check *ck, CheckJob *job, Expect *slots, size_t n, JogType *tuple, bool checked) {
    size_t i;
    if (!tuple || is_poison(tuple) || tuple->kind != JOG_TY_TUPLE || ck->stop) {
        return;
    }
    for (i = 0; i < n && !ck->stop; i++) {
        JogType *elem = tuple->as.tuple.elems ? tuple->as.tuple.elems[i] : NULL;
        JogNode *target = slots[i].target;
        if (slots[i].discard) {
            if (target) {
                target->type = elem;
            }
            continue;
        }
        if (slots[i].nested) {
            if (!elem || is_poison(elem)) {
                continue;
            }
            if (elem->kind != JOG_TY_TUPLE || elem->as.tuple.count != slots[i].kid_count) {
                if (!checked) {
                    char text[160];
                    if (!elem || elem->kind != JOG_TY_TUPLE) {
                        type_text(ck, elem, text, sizeof(text));
                        jog_check_fail(ck, target ? target->pos : (JogPos){0}, "destructuring expects a tuple, got %s", text);
                    } else {
                        jog_check_fail(ck, target ? target->pos : (JogPos){0},
                                       "destructuring expects %" PRIu64 " values, got %" PRIu64,
                                       (uint64_t)slots[i].kid_count, (uint64_t)elem->as.tuple.count);
                    }
                }
                continue;
            }
            accept_slots(ck, job, slots[i].kids, slots[i].kid_count, elem, slots[i].checked);
            continue;
        }
        if (!slots[i].writable || !slots[i].type || !elem || is_poison(slots[i].type) || is_poison(elem)) {
            continue;
        }
        if (!same_type(slots[i].type, elem)) {
            if (!checked) {
                fail_mismatch(ck, target ? target->pos : (JogPos){0}, slots[i].type, elem);
            }
            continue;
        }
        if (target && target->kind == JOG_NODE_IDENT && target->symbol) {
            jog_check_body_assigned(job, target->symbol);
        }
    }
}

void jog_check_error_destructure(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *got;
    JogType *unit;
    Expect *slots;
    size_t count;
    size_t i;
    bool literal;
    if (!ck || !node || ck->stop) {
        return;
    }
    count = node->as.destructure.targets.count;
    if (count < 2) {
        jog_check_fail(ck, node->pos, "a tuple needs at least 2 elements");
        return;
    }
    slots = ct_arena_alloc(ck->program->arena, count * sizeof(Expect), _Alignof(Expect));
    if (!slots) {
        jog_check_oom(ck);
        return;
    }
    memset(slots, 0, count * sizeof(Expect));
    for (i = 0; i < count && !ck->stop; i++) {
        if (fill_expect(ck, job, scope, list_at(node->as.destructure.targets, i), &slots[i]) < 0) {
            return;
        }
    }
    if (ck->stop) {
        return;
    }
    literal = node->as.destructure.value && node->as.destructure.value->kind == JOG_NODE_TUPLE &&
              node->as.destructure.value->as.tuple.elems.count == count;
    got = check_against(ck, job, scope, node->as.destructure.value, slots, count);
    if (!got || ck->stop) {
        return;
    }
    if (!is_poison(got)) {
        accept_slots(ck, job, slots, count, got, literal);
    }
    unit = jog_check_unit_type(ck);
    node->type = unit;
}

void jog_check_error_return(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *ret) {
    size_t count;
    size_t expect;
    size_t i;
    if (!ck || !node || ck->stop) {
        return;
    }
    count = node->as.ret.values.count;
    expect = 0;
    if (ret && ret->kind == JOG_TY_TUPLE) {
        expect = ret->as.tuple.count;
    } else if (ret && ret->kind != JOG_TY_UNIT && !is_poison(ret)) {
        expect = 1;
    }
    if (!ret || ret->kind != JOG_TY_TUPLE || ret->as.tuple.count != count) {
        if (expect == 1) {
            jog_check_fail(ck, node->pos, "return expects 1 value, got %" PRIu64, (uint64_t)count);
        } else {
            jog_check_fail(ck, node->pos, "return expects %" PRIu64 " values, got %" PRIu64, (uint64_t)expect,
                           (uint64_t)count);
        }
        node->type = ret ? ret : jog_check_unit_type(ck);
        return;
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogType *want = ret->as.tuple.elems ? ret->as.tuple.elems[i] : NULL;
        (void)jog_check_expr(ck, job, scope, list_at(node->as.ret.values, i), want);
    }
    node->type = ret;
}

void jog_check_error_defer(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *ret) {
    (void)scope;
    if (!ck || !node || ck->stop) {
        return;
    }
    if (node->kind == JOG_NODE_ERRDEFER && !is_error_option(last_result(ret))) {
        jog_check_fail(ck, node->pos, "errdefer requires a function that returns error?");
    }
    if (node->as.defer.body) {
        jog_check_body_stmt_isolated(job, node->as.defer.body);
    }
    node->type = jog_check_unit_type(ck);
}
