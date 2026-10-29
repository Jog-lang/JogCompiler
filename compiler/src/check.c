#include "phases.h"

#include "check_body.h"
#include "check_platform.h"
#include "check_constraint.h"
#include "check_context.h"
#include "check_enum.h"
#include "check_error.h"
#include "check_generic.h"
#include "check_interface.h"
#include "check_package.h"
#include "check_struct.h"
#include "check_coro.h"
#include "const_eval.h"
#include "prelude.h"
#include "scalar_op.h"
#include "resolve_package.h"
#include "resolve_universe.h"

#include "coro.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* On-demand checking of top-level declarations. Each declaration runs in
   its own coroutine. When it needs the type of a declaration that is not
   finished, it parks until that one is done. If every remaining coroutine
   is parked, the wait edges form a cycle and the diagnostic is the chain,
   starting at the earliest declaration:

     declaration cycle: A -> B -> A

   Initializer names are demands too, so a cycle between global
   initializers uses the same chain. A function type is published when its
   signature is done, and the body is checked after that (see check_body.c).
   Interface and enum members are not checked here. A struct publishes its
   nominal type before its fields are resolved, so another struct may name
   it through a pointer without waiting for the fields. Function type
   parameters are bound in check_generic.c. Generic types are not checked. */

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
static void fail(Check *ck, JogPos pos, const char *fmt, ...) {
    char buf[512];
    va_list args;
    int wrote;
    ck->failed = true;
    va_start(args, fmt);
    wrote = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (wrote < 0) {
        buf[0] = '\0';
    }
    ct_diag_emit(ck->program->diag, CT_SEVERITY_ERROR, as_pos(pos), "%s", buf);
}

static void oom(Check *ck) {
    CtPos pos = {0};
    ck->failed = true;
    ck->stop = true;
    ct_diag_emit(ck->program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
}

static bool is_discard(const char *name) {
    return !name || name[0] == '\0' || (name[0] == '_' && name[1] == '\0');
}

static bool package_visible(JogVisibility vis) {
    return vis == JOG_VIS_PUBLIC || vis == JOG_VIS_PACKAGE;
}

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static bool is_scheduled(JogNodeKind kind) {
    switch (kind) {
    case JOG_NODE_FN:
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
    case JOG_NODE_TYPE_DECL:
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        return true;
    default:
        return false;
    }
}

static bool is_type_expr(JogNodeKind kind) {
    switch (kind) {
    case JOG_NODE_TYPE_NAME:
    case JOG_NODE_TYPE_PTR:
    case JOG_NODE_TYPE_SLICE:
    case JOG_NODE_TYPE_ARRAY:
    case JOG_NODE_TYPE_OPTION:
    case JOG_NODE_TYPE_TUPLE:
    case JOG_NODE_TYPE_FN:
    case JOG_NODE_TYPE_UNIT:
        return true;
    default:
        return false;
    }
}

static JogType *error_type(Check *ck) {
    JogType *type = jog_type_builtin(ck->program->types, JOG_BUILTIN_ERROR);
    if (!type) {
        oom(ck);
    }
    return type;
}

static JogType *unit_type(Check *ck) {
    JogType *type = jog_type_builtin(ck->program->types, JOG_BUILTIN_UNIT);
    if (!type) {
        oom(ck);
    }
    return type;
}

static void finish_job(CheckJob *job, JogType *type) {
    size_t i;
    if (!job || job->done) {
        return;
    }
    if (job->decl) {
        job->decl->type = type;
    }
    /* A destructuring binding publishes one type per name. The tuple type
       stays on the declaration; finish must not overwrite the elements. */
    {
        bool split = job->decl && (job->decl->kind == JOG_NODE_VAL || job->decl->kind == JOG_NODE_VAR) &&
                     job->decl->as.binding.destructure && job->decl->as.binding.names.count > 1 && type &&
                     type->kind == JOG_TY_TUPLE;
        for (i = 0; i < job->symbol_count; i++) {
            if (job->symbols[i] && !split) {
                job->symbols[i]->type = type;
            }
        }
    }
    job->parked = false;
    job->waiting_for = NULL;
    job->done = true;
}

static size_t job_index(const Check *ck, const CheckJob *job) {
    if (!ck->jobs || job < ck->jobs || job >= ck->jobs + ck->job_count) {
        return ck->job_count;
    }
    return (size_t)(job - ck->jobs);
}

static CheckJob *job_for(const Check *ck, const JogSymbol *symbol) {
    size_t i;
    size_t s;
    if (!symbol) {
        return NULL;
    }
    for (i = 0; i < ck->job_count; i++) {
        CheckJob *job = &ck->jobs[i];
        for (s = 0; s < job->symbol_count; s++) {
            if (job->symbols[s] == symbol) {
                return job;
            }
        }
    }
    return NULL;
}

static const char *job_name(const CheckJob *job) {
    if (job->symbol && job->symbol->name && job->symbol->name[0] != '\0') {
        return job->symbol->name;
    }
    return "?";
}

/* False when this coroutine must stop. A symbol with no job (a builtin,
   or a name that is not a top-level declaration) does not park. */
static bool wait_for_symbol(Check *ck, CheckJob *self, JogSymbol *symbol, bool until_done) {
    CheckJob *other;
    if (!symbol || ck->stop) {
        return !ck->stop;
    }
    other = job_for(ck, symbol);
    if (!other || other->done) {
        return !ck->stop;
    }
    /* A struct declaration may use another struct's identity before that
       struct's fields are finished. *Node inside struct Node, and *B
       inside struct A while B is still open, are not cycles. until_done
       is the layout wait: a value field needs the other struct finished.
       Function bodies still wait until the struct job is done. */
    /* A struct or enum may use another nominal's identity before its fields
       are finished. *Node inside enum Node is not a cycle. until_done is the
       layout wait a value payload still needs. */
    if (!until_done && symbol->type && self->decl && other->decl == symbol->decl) {
        if (symbol->type->kind == JOG_TY_STRUCT && self->decl->kind == JOG_NODE_STRUCT) {
            return !ck->stop;
        }
        if (symbol->type->kind == JOG_TY_ENUM && self->decl->kind == JOG_NODE_ENUM) {
            return !ck->stop;
        }
        if (symbol->type->kind == JOG_TY_INTERFACE && self->decl->kind == JOG_NODE_INTERFACE) {
            return !ck->stop;
        }
    }
    self->waiting_for = other;
    self->parked = true;
    while (!other->done && !ck->stop) {
        coroutine_yield();
        ck->current_job = self;
    }
    self->parked = false;
    self->waiting_for = NULL;
    return !ck->stop && other->done;
}

static JogSymbol *lookup_in_file(const CheckJob *job, const char *name) {
    JogPackageScope *scope;
    JogSymbol *found;
    if (!job->package || !name) {
        return NULL;
    }
    scope = job->package->scope;
    if (!scope) {
        return NULL;
    }
    found = jog_file_lookup(scope, job->file_index, name);
    if (found && found->kind == JOG_SYM_PACKAGE) {
        found->referenced = true;
    }
    return found;
}

static JogSymbol *lookup_binding(const CheckJob *job, const char *name) {
    JogPackageScope *scope;
    JogSymbol *found = NULL;
    if (!job->package || !name || is_discard(name)) {
        return NULL;
    }
    scope = job->package->scope;
    if (!scope) {
        return NULL;
    }
    if (package_visible(job->decl->visibility)) {
        found = jog_package_scope_lookup(scope, name);
    } else if (scope->file_scopes && job->file_index < scope->file_scope_count &&
               scope->file_scopes[job->file_index]) {
        found = jog_scope_map_lookup(scope->file_scopes[job->file_index]->bindings, name);
    }
    if (!found || found->decl != job->decl) {
        return NULL;
    }
    return found;
}

static void type_label(JogNames path, char *buf, size_t cap) {
    size_t i;
    size_t at = 0;
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!path.items) {
        return;
    }
    for (i = 0; i < path.count; i++) {
        const char *part = path.items[i] ? path.items[i] : "?";
        size_t n = strlen(part);
        size_t extra = i > 0 ? 1 : 0;
        if (at + extra + n + 1 > cap) {
            break;
        }
        if (i > 0) {
            buf[at++] = '.';
        }
        memcpy(buf + at, part, n);
        at += n;
        buf[at] = '\0';
    }
}

static JogSymbol *lookup_type_path(Check *ck, CheckJob *self, JogNames path, JogPos pos, const char *label) {
    JogSymbol *first;
    JogSymbol *sym;
    const char *shown = label && label[0] != '\0' ? label : "?";
    if (path.count == 0 || !path.items || !path.items[0] || path.items[0][0] == '\0') {
        fail(ck, pos, "undefined type '%s'", shown);
        return NULL;
    }
    first = lookup_in_file(self, path.items[0]);
    if (path.count == 1) {
        if (!first) {
            fail(ck, pos, "undefined type '%s'", shown);
        }
        return first;
    }
    if (!first || first->kind != JOG_SYM_PACKAGE || !first->package || !first->package->scope) {
        fail(ck, pos, "undefined type '%s'", shown);
        return NULL;
    }
    if (path.count != 2 || !path.items[1] || path.items[1][0] == '\0') {
        fail(ck, pos, "undefined type '%s'", shown);
        return NULL;
    }
    sym = jog_package_scope_lookup((JogPackageScope *)first->package->scope, path.items[1]);
    if (!sym) {
        fail(ck, pos, "undefined type '%s'", shown);
        return NULL;
    }
    if (!jog_symbol_visible_from(sym, self->package, self->decl->pos.file)) {
        fail(ck, pos, "type '%s' is not visible", path.items[1]);
        return NULL;
    }
    return sym;
}

static JogType *resolve_type(Check *ck, CheckJob *self, JogNode *node);

static JogType **alloc_types(Check *ck, size_t count) {
    JogType **items;
    if (count == 0) {
        return NULL;
    }
    items = ct_arena_alloc(ck->program->arena, count * sizeof(JogType *), _Alignof(JogType *));
    if (!items) {
        oom(ck);
    }
    return items;
}

static bool resolve_params(Check *ck, CheckJob *self, JogList list, JogType **out, bool *bad) {
    size_t i;
    for (i = 0; i < list.count; i++) {
        JogNode *param = list_at(list, i);
        JogType *type;
        if (!param || param->kind != JOG_NODE_PARAM) {
            fail(ck, param ? param->pos : self->decl->pos, "expected a parameter");
            *bad = true;
            if (out) {
                out[i] = error_type(ck);
            }
            continue;
        }
        if (!param->as.param.type) {
            fail(ck, param->pos, "missing parameter type");
            *bad = true;
            if (out) {
                out[i] = error_type(ck);
            }
            continue;
        }
        type = resolve_type(ck, self, param->as.param.type);
        if (!type) {
            return false;
        }
        /* A pack's local is a slice of the written element. T is allowed;
           do not reject it because it is a type parameter. */
        if (param->as.param.pack && type->kind != JOG_TY_ERROR) {
            JogType *slice = jog_type_slice(ck->program->types, type, false);
            if (!slice) {
                oom(ck);
                return false;
            }
            type = slice;
        }
        param->type = type;
        if (type->kind == JOG_TY_ERROR) {
            *bad = true;
        }
        if (out) {
            out[i] = type;
        }
    }
    return !ck->stop;
}

static bool mentions_size(const JogNode *node) {
    size_t i;
    if (!node) {
        return false;
    }
    switch (node->kind) {
    case JOG_NODE_CALL: {
        const JogNode *callee = node->as.call.callee;
        if (callee && callee->kind == JOG_NODE_IDENT && callee->as.ident.name &&
            (strcmp(callee->as.ident.name, "size") == 0 || strcmp(callee->as.ident.name, "align") == 0)) {
            return true;
        }
        for (i = 0; i < node->as.call.args.count; i++) {
            const JogNode *arg = node->as.call.args.items ? node->as.call.args.items[i] : NULL;
            const JogNode *value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
            if (mentions_size(value)) {
                return true;
            }
        }
        return mentions_size(callee);
    }
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        return mentions_size(node->as.unary.operand);
    case JOG_NODE_BINARY:
        return mentions_size(node->as.binary.lhs) || mentions_size(node->as.binary.rhs);
    case JOG_NODE_MEMBER:
        return mentions_size(node->as.member.base);
    default:
        return false;
    }
}

static uint64_t const_length(Check *ck, CheckJob *job, JogNode *len, JogPos pos, bool *bad) {
    JogConst value;
    if (!len) {
        fail(ck, pos, "array length is required");
        *bad = true;
        return 0;
    }
    /* size(T) and align(T) are typed here so the fold below can read the layout. */
    if (mentions_size(len)) {
        JogType *got = jog_check_expr(ck, job, jog_check_body_scope(job), len, NULL);
        if (ck->stop || !got || got->kind == JOG_TY_ERROR) {
            *bad = true;
            return 0;
        }
    }
    memset(&value, 0, sizeof(value));
    if (!jog_const_eval(ck->program->arena, ck->program->diag, ck->program->types, len, NULL, &value)) {
        ck->failed = true;
        *bad = true;
        return 0;
    }
    if (value.kind != JOG_CONST_INT) {
        fail(ck, len->pos, "array length is not a constant");
        *bad = true;
        return 0;
    }
    if (value.type && value.type->kind == JOG_TY_INT && value.type->as.integer.is_signed && (int64_t)value.bits < 0) {
        fail(ck, len->pos, "array length is not a constant");
        *bad = true;
        return 0;
    }
    len->type = value.type;
    return value.bits;
}

static JogType *resolve_type_name(Check *ck, CheckJob *self, JogNode *node) {
    char label[256];
    JogSymbol *symbol;
    JogType *error;
    type_label(node->as.type_name.path, label, sizeof(label));
    if (node->as.type_name.path.count == 1 && node->as.type_name.path.items && node->as.type_name.path.items[0]) {
        JogType *param = jog_generic_lookup_param(self, node->as.type_name.path.items[0]);
        if (param) {
            node->type = param;
            return param;
        }
    }
    if (node->as.type_name.path.count == 1 && node->as.type_name.path.items && node->as.type_name.path.items[0] &&
        strcmp(node->as.type_name.path.items[0], "Option") == 0) {
        JogSymbol *existing = self ? lookup_in_file(self, "Option") : NULL;
        if (!existing || existing->kind != JOG_SYM_TYPE || jog_symbol_is_core_option(existing)) {
            JogType *elem;
            JogType *built;
            JogNode *arg;
            error = error_type(ck);
            if (node->as.type_name.args.count != 1) {
                jog_check_fail(ck, node->pos, "Option takes one type argument");
                node->type = error;
                return error;
            }
            arg = node->as.type_name.args.items ? node->as.type_name.args.items[0] : NULL;
            elem = resolve_type(ck, self, arg);
            if (!elem || ck->stop) {
                return NULL;
            }
            if (elem->kind == JOG_TY_ERROR) {
                node->type = error;
                return error;
            }
            built = jog_type_option(ck->program->types, elem);
            if (!built) {
                jog_check_fail(ck, node->pos, "out of memory");
                return NULL;
            }
            node->type = built;
            return built;
        }
    }
    symbol = lookup_type_path(ck, self, node->as.type_name.path, node->pos, label);
    error = error_type(ck);
    if (!symbol) {
        node->type = error;
        return error;
    }
    node->symbol = symbol;
    if (!wait_for_symbol(ck, self, symbol, false)) {
        return NULL;
    }
    if (symbol->kind != JOG_SYM_TYPE) {
        fail(ck, node->pos, "'%s' is not a type", label[0] != '\0' ? label : "?");
        node->type = error;
        return error;
    }
    if (!symbol->type) {
        fail(ck, node->pos, "'%s' has no type", label[0] != '\0' ? label : "?");
        node->type = error;
        return error;
    }
    if (symbol->type->kind == JOG_TY_ERROR) {
        node->type = error;
        return error;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_TYPE_DECL && symbol->type_param_count > 0) {
        JogType *inst = jog_generic_alias_apply(ck, self, symbol, node->as.type_name.args, node->pos);
        if (!inst) {
            return NULL;
        }
        node->type = inst;
        return inst;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_ENUM && symbol->type->kind == JOG_TY_ENUM &&
        symbol->type_param_count > 0) {
        JogType *inst = jog_generic_enum_apply(ck, self, symbol, node->as.type_name.args, node->pos);
        if (!inst) {
            return NULL;
        }
        node->type = inst;
        return inst;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_STRUCT && symbol->type->kind == JOG_TY_STRUCT &&
        symbol->type_param_count > 0) {
        JogType *inst = jog_generic_struct_apply(ck, self, symbol, node->as.type_name.args, node->pos);
        if (!inst) {
            return NULL;
        }
        node->type = inst;
        return inst;
    }
    {
        JogType *coro_ty = NULL;
        int cr = jog_check_coro_type(ck, self, symbol, node, &coro_ty);
        if (cr < 0) {
            return NULL;
        }
        if (cr > 0) {
            node->type = coro_ty;
            return coro_ty;
        }
    }
    if (node->as.type_name.args.count > 0) {
        fail(ck, node->pos, "type arguments are not checked yet");
        node->type = error;
        return error;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_STRUCT && symbol->decl->as.aggregate.type_params.count > 0) {
        fail(ck, node->pos, "missing type arguments for '%s'", label[0] != '\0' ? label : "?");
        node->type = error;
        return error;
    }
    if (symbol->decl && symbol->decl->kind == JOG_NODE_TYPE_DECL && symbol->decl->as.type_decl.type_params.count > 0) {
        fail(ck, node->pos, "missing type arguments for '%s'", label[0] != '\0' ? label : "?");
        node->type = error;
        return error;
    }
    if (!ck->allow_typeset && jog_constraint_is_typeset(symbol->type)) {
        fail(ck, node->pos, "type set interface '%s' cannot be used as a value", label[0] != '\0' ? label : "?");
        node->type = error;
        return error;
    }
    node->type = symbol->type;
    return symbol->type;
}

/* const Name is allowed when Name is a pointer, a slice, or the bytes view.
   const on an array, including byte and [N]byte, is an error. */
static JogType *apply_name_const(Check *ck, JogNode *node, JogType *type) {
    JogTypes *types;
    JogType *built;
    if (!type || type->kind == JOG_TY_ERROR) {
        return type;
    }
    types = ck->program->types;
    built = NULL;
    if (type->kind == JOG_TY_PTR) {
        built = jog_type_ptr(types, type->as.elem.elem, true);
    } else if (type->kind == JOG_TY_SLICE) {
        built = jog_type_slice(types, type->as.elem.elem, true);
    } else if (type->kind == JOG_TY_BYTES) {
        built = jog_type_builtin(types, JOG_BUILTIN_CONST_BYTES);
    } else if (type->kind == JOG_TY_ARRAY || type->kind == JOG_TY_BYTES_N) {
        fail(ck, node->pos, "const is not allowed on an array type");
        node->type = error_type(ck);
        return node->type;
    } else {
        fail(ck, node->pos, "const is only allowed on a pointer, a slice, or bytes");
        node->type = error_type(ck);
        return node->type;
    }
    if (!built) {
        oom(ck);
        return NULL;
    }
    node->type = built;
    return built;
}

static JogType *resolve_type(Check *ck, CheckJob *self, JogNode *node) {
    JogTypes *types;
    JogType *error;
    bool bad;
    if (!node || ck->stop) {
        return NULL;
    }
    types = ck->program->types;
    error = error_type(ck);
    if (!error || ck->stop) {
        return NULL;
    }
    bad = false;
    switch (node->kind) {
    case JOG_NODE_TYPE_NAME: {
        JogType *named = resolve_type_name(ck, self, node);
        if (!named || !node->as.type_name.is_const) {
            return named;
        }
        return apply_name_const(ck, node, named);
    }
    case JOG_NODE_TYPE_UNIT: {
        JogType *unit = unit_type(ck);
        if (!unit) {
            return NULL;
        }
        node->type = unit;
        return unit;
    }
    case JOG_NODE_TYPE_PTR:
    case JOG_NODE_TYPE_SLICE:
    case JOG_NODE_TYPE_OPTION: {
        JogType *elem = node->as.type_elem.elem ? resolve_type(ck, self, node->as.type_elem.elem) : NULL;
        JogType *built;
        if (!node->as.type_elem.elem) {
            fail(ck, node->pos, "missing type");
            node->type = error;
            return error;
        }
        if (!elem) {
            return NULL;
        }
        if (elem->kind == JOG_TY_ERROR) {
            node->type = error;
            return error;
        }
        if (node->kind == JOG_NODE_TYPE_PTR) {
            built = jog_type_ptr(types, elem, node->as.type_elem.is_const);
        } else if (node->kind == JOG_NODE_TYPE_SLICE) {
            built = jog_type_slice(types, elem, node->as.type_elem.is_const);
        } else {
            built = jog_type_option(types, elem);
        }
        if (!built) {
            oom(ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    case JOG_NODE_TYPE_ARRAY: {
        JogType *elem;
        JogType *built;
        uint64_t len;
        if (node->as.type_array.infer_len || !node->as.type_array.len) {
            fail(ck, node->pos, "array length is required");
            node->type = error;
            return error;
        }
        elem = node->as.type_array.elem ? resolve_type(ck, self, node->as.type_array.elem) : NULL;
        if (!node->as.type_array.elem) {
            fail(ck, node->pos, "missing type");
            node->type = error;
            return error;
        }
        if (!elem) {
            return NULL;
        }
        if (elem->kind == JOG_TY_ERROR) {
            node->type = error;
            return error;
        }
        len = const_length(ck, self, node->as.type_array.len, node->pos, &bad);
        if (ck->stop) {
            return NULL;
        }
        if (bad) {
            node->type = error;
            return error;
        }
        built = jog_type_array(types, elem, len);
        if (!built) {
            oom(ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    case JOG_NODE_TYPE_TUPLE: {
        size_t count = node->as.type_tuple.elems.count;
        size_t i;
        JogType **elems = alloc_types(ck, count);
        JogType *built;
        if (ck->stop) {
            return NULL;
        }
        if (count > 0 && !elems) {
            return NULL;
        }
        for (i = 0; i < count; i++) {
            JogNode *elem_node = list_at(node->as.type_tuple.elems, i);
            JogType *elem;
            if (!elem_node) {
                fail(ck, node->pos, "missing type");
                bad = true;
                elems[i] = error;
                continue;
            }
            elem = resolve_type(ck, self, elem_node);
            if (!elem) {
                return NULL;
            }
            if (elem->kind == JOG_TY_ERROR) {
                bad = true;
            }
            elems[i] = elem;
        }
        if (bad) {
            node->type = error;
            return error;
        }
        built = jog_type_tuple(types, elems, count);
        if (!built) {
            oom(ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    case JOG_NODE_TYPE_FN: {
        size_t param_count = node->as.type_fn.params.count;
        size_t context_count = node->as.type_fn.contexts.count;
        size_t i;
        JogType **params = alloc_types(ck, param_count);
        JogType **contexts = alloc_types(ck, context_count);
        JogType *ret;
        JogType *built;
        if (ck->stop) {
            return NULL;
        }
        if ((param_count > 0 && !params) || (context_count > 0 && !contexts)) {
            return NULL;
        }
        for (i = 0; i < param_count; i++) {
            JogNode *param = list_at(node->as.type_fn.params, i);
            JogType *type = param ? resolve_type(ck, self, param) : NULL;
            if (!param) {
                fail(ck, node->pos, "missing type");
                bad = true;
                params[i] = error;
                continue;
            }
            if (!type) {
                return NULL;
            }
            if (type->kind == JOG_TY_ERROR) {
                bad = true;
            }
            params[i] = type;
        }
        for (i = 0; i < context_count; i++) {
            JogNode *ctx = list_at(node->as.type_fn.contexts, i);
            JogType *type = ctx ? resolve_type(ck, self, ctx) : NULL;
            if (!ctx) {
                fail(ck, node->pos, "missing type");
                bad = true;
                contexts[i] = error;
                continue;
            }
            if (!type) {
                return NULL;
            }
            if (type->kind == JOG_TY_ERROR) {
                bad = true;
            }
            contexts[i] = type;
        }
        if (node->as.type_fn.ret) {
            ret = resolve_type(ck, self, node->as.type_fn.ret);
            if (!ret) {
                return NULL;
            }
            if (ret->kind == JOG_TY_ERROR) {
                bad = true;
            }
        } else {
            ret = unit_type(ck);
            if (!ret) {
                return NULL;
            }
        }
        if (bad) {
            node->type = error;
            return error;
        }
        built = jog_type_fn(types, params, param_count, ret, contexts, context_count, false, node->as.type_fn.escaping,
                            node->as.type_fn.is_extern);
        if (!built) {
            oom(ck);
            return NULL;
        }
        node->type = built;
        return built;
    }
    default:
        fail(ck, node->pos, "not a type");
        node->type = error;
        return error;
    }
}

static void demand_uses(Check *ck, CheckJob *self, JogNode *node);

static void demand_list(Check *ck, CheckJob *self, JogList list) {
    size_t i;
    for (i = 0; i < list.count && !ck->stop; i++) {
        JogNode *item = list_at(list, i);
        if (!item) {
            continue;
        }
        if (item->kind == JOG_NODE_ARG) {
            demand_uses(ck, self, item->as.arg.value);
        } else {
            demand_uses(ck, self, item);
        }
    }
}

static bool remember_use(Check *ck, CheckJob *self, JogNode *node, JogSymbol *symbol) {
    if (!symbol) {
        return !ck->stop;
    }
    node->symbol = symbol;
    if (!wait_for_symbol(ck, self, symbol, false)) {
        return false;
    }
    node->type = symbol->type;
    return !ck->stop;
}

/* Names in an initializer. This does not type-check the expression. A
   bare identifier can later supply an inferred binding type. */
static void demand_uses(Check *ck, CheckJob *self, JogNode *node) {
    if (!node || ck->stop) {
        return;
    }
    if (is_type_expr(node->kind)) {
        /* [_]T in an array literal takes its length from the elements. */
        if (node->kind == JOG_NODE_TYPE_ARRAY && node->as.type_array.infer_len) {
            demand_uses(ck, self, node->as.type_array.elem);
            return;
        }
        (void)resolve_type(ck, self, node);
        return;
    }
    switch (node->kind) {
    case JOG_NODE_IDENT:
        if (!remember_use(ck, self, node, lookup_in_file(self, node->as.ident.name))) {
            return;
        }
        demand_list(ck, self, node->as.ident.type_args);
        return;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        demand_uses(ck, self, node->as.unary.operand);
        return;
    case JOG_NODE_BINARY:
        demand_uses(ck, self, node->as.binary.lhs);
        demand_uses(ck, self, node->as.binary.rhs);
        return;
    case JOG_NODE_CALL:
        demand_uses(ck, self, node->as.call.callee);
        demand_list(ck, self, node->as.call.args);
        demand_uses(ck, self, node->as.call.trailing);
        demand_list(ck, self, node->as.call.contexts);
        {
            size_t ci;
            for (ci = 0; ci < node->context_arg_count && !ck->stop; ci++) {
                demand_uses(ck, self, node->context_args ? node->context_args[ci] : NULL);
            }
        }
        return;
    case JOG_NODE_WITH:
        demand_list(ck, self, node->as.with.values);
        demand_uses(ck, self, node->as.with.body);
        return;
    case JOG_NODE_CONTEXT:
        demand_uses(ck, self, node->as.binding.type);
        demand_uses(ck, self, node->as.binding.init);
        return;
    case JOG_NODE_ARG:
        demand_uses(ck, self, node->as.arg.value);
        return;
    case JOG_NODE_INDEX:
        demand_uses(ck, self, node->as.index.base);
        demand_uses(ck, self, node->as.index.index);
        return;
    case JOG_NODE_SLICE:
        demand_uses(ck, self, node->as.slice.base);
        demand_uses(ck, self, node->as.slice.lo);
        demand_uses(ck, self, node->as.slice.hi);
        return;
    case JOG_NODE_MEMBER: {
        JogSymbol *base_sym;
        demand_uses(ck, self, node->as.member.base);
        if (ck->stop || !node->as.member.base || !node->as.member.name) {
            return;
        }
        base_sym = node->as.member.base->symbol;
        if (base_sym && base_sym->kind == JOG_SYM_PACKAGE && base_sym->package && base_sym->package->scope) {
            JogSymbol *mem = jog_package_scope_lookup((JogPackageScope *)base_sym->package->scope, node->as.member.name);
            if (mem && jog_symbol_visible_from(mem, self->package, self->decl->pos.file)) {
                (void)remember_use(ck, self, node, mem);
            }
        }
        return;
    }
    case JOG_NODE_TUPLE:
        demand_list(ck, self, node->as.tuple.elems);
        return;
    case JOG_NODE_CAST:
        demand_uses(ck, self, node->as.cast.expr);
        demand_uses(ck, self, node->as.cast.type);
        return;
    case JOG_NODE_IS:
        demand_uses(ck, self, node->as.is.expr);
        demand_uses(ck, self, node->as.is.type);
        return;
    case JOG_NODE_IF:
        demand_uses(ck, self, node->as.if_.cond);
        demand_uses(ck, self, node->as.if_.then_block);
        demand_uses(ck, self, node->as.if_.else_branch);
        return;
    case JOG_NODE_BLOCK:
        demand_list(ck, self, node->as.block.stmts);
        return;
    case JOG_NODE_EXPR_STMT:
        demand_uses(ck, self, node->as.expr_stmt.expr);
        return;
    case JOG_NODE_ASSIGN:
        demand_uses(ck, self, node->as.assign.target);
        demand_uses(ck, self, node->as.assign.value);
        return;
    case JOG_NODE_DESTRUCTURE:
        demand_list(ck, self, node->as.destructure.targets);
        demand_uses(ck, self, node->as.destructure.value);
        return;
    case JOG_NODE_RETURN:
        demand_list(ck, self, node->as.ret.values);
        return;
    case JOG_NODE_ARRAY_LIT:
        demand_uses(ck, self, node->as.array_lit.type);
        demand_list(ck, self, node->as.array_lit.elems);
        return;
    case JOG_NODE_INTERP:
        demand_list(ck, self, node->as.interp.parts);
        return;
    case JOG_NODE_LAMBDA: {
        size_t i;
        for (i = 0; i < node->as.lambda.params.count && !ck->stop; i++) {
            JogNode *param = list_at(node->as.lambda.params, i);
            if (param && param->kind == JOG_NODE_PARAM) {
                demand_uses(ck, self, param->as.param.type);
            }
        }
        demand_uses(ck, self, node->as.lambda.ret);
        demand_uses(ck, self, node->as.lambda.body);
        return;
    }
    case JOG_NODE_GO:
        demand_list(ck, self, node->as.go.args);
        demand_uses(ck, self, node->as.go.body);
        return;
    case JOG_NODE_FN:
        /* A nested function is its own declaration, not part of this initializer. */
        return;
    default:
        return;
    }
}

static JogType *make_distinct(Check *ck, CheckJob *job, JogType *base) {
    JogNode *node = job->decl;
    JogType *type = jog_type_new_nominal(ck->program->types, JOG_TY_DISTINCT, node->as.type_decl.name,
                                         job->package ? job->package->path : NULL, node);
    if (!type) {
        oom(ck);
        return NULL;
    }
    type->as.nominal.base = base;
    if (!jog_type_layout(ck->program->types, type)) {
        fail(ck, node->pos, "type '%s' has no layout", node->as.type_decl.name ? node->as.type_decl.name : "?");
        return error_type(ck);
    }
    jog_type_apply_intrinsic(ck->program->types, type, node);
    return type;
}

static JogType *check_type_decl(Check *ck, CheckJob *job) {
    JogNode *node = job->decl;
    JogType *base;
    bool bad = false;
    /* The notes show `type Pair<A, B> = (A, B)` and do not show a distinct
       type with type parameters. That form stays unchecked. */
    if (node->as.type_decl.type_params.count > 0 && node->as.type_decl.distinct) {
        fail(ck, node->pos, "generic distinct types are not checked yet");
        return error_type(ck);
    }
    if (node->as.type_decl.type_params.count > 0) {
        if (!jog_generic_bind_alias_params(ck, job, &bad)) {
            return NULL;
        }
    }
    if (!node->as.type_decl.target) {
        fail(ck, node->pos, "missing type");
        return error_type(ck);
    }
    base = resolve_type(ck, job, node->as.type_decl.target);
    if (!base) {
        return NULL;
    }
    if (bad || base->kind == JOG_TY_ERROR) {
        return base->kind == JOG_TY_ERROR ? base : error_type(ck);
    }
    if (!node->as.type_decl.distinct) {
        return base;
    }
    return make_distinct(ck, job, base);
}

static bool fn_has_attr(const JogNode *fn, const char *name) {
    size_t i;
    if (!fn || !name) {
        return false;
    }
    for (i = 0; i < fn->attrs.count; i++) {
        JogNode *attr = list_at(fn->attrs, i);
        if (attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
            strcmp(attr->as.attribute.name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* 1 when the receiver names a generic struct or enum. 2 for a generic alias.
   The names in List<T> are binders, not type arguments. *nparams is the
   declaration's type-parameter count. *owner is that struct or enum. */
static int generic_receiver_kind(CheckJob *job, JogNode *recv, size_t *nparams, JogSymbol **owner) {
    JogSymbol *sym;
    JogNames path;
    if (nparams) {
        *nparams = 0;
    }
    if (owner) {
        *owner = NULL;
    }
    if (!recv || !job) {
        return 0;
    }
    if (recv->kind == JOG_NODE_TYPE_PTR || recv->kind == JOG_NODE_TYPE_SLICE || recv->kind == JOG_NODE_TYPE_OPTION) {
        return generic_receiver_kind(job, recv->as.type_elem.elem, nparams, owner);
    }
    if (recv->kind != JOG_NODE_TYPE_NAME) {
        return 0;
    }
    path = recv->as.type_name.path;
    if (path.count == 0 || !path.items || !path.items[0]) {
        return 0;
    }
    sym = lookup_in_file(job, path.items[0]);
    if (path.count == 2 && sym && sym->kind == JOG_SYM_PACKAGE && sym->package && sym->package->scope && path.items[1]) {
        sym = jog_package_scope_lookup((JogPackageScope *)sym->package->scope, path.items[1]);
    } else if (path.count != 1) {
        return 0;
    }
    if (!sym || !sym->decl) {
        return 0;
    }
    if (sym->decl->kind == JOG_NODE_TYPE_DECL) {
        if (sym->decl->as.type_decl.type_params.count == 0) {
            return 0;
        }
        if (nparams) {
            *nparams = sym->decl->as.type_decl.type_params.count;
        }
        return 2;
    }
    if (sym->decl->kind != JOG_NODE_STRUCT && sym->decl->kind != JOG_NODE_ENUM) {
        return 0;
    }
    if (sym->decl->as.aggregate.type_params.count == 0) {
        return 0;
    }
    if (nparams) {
        *nparams = sym->decl->as.aggregate.type_params.count;
    }
    if (owner) {
        *owner = sym;
    }
    return 1;
}

static JogType *check_fn(Check *ck, CheckJob *job) {
    JogNode *fn = job->decl;
    size_t param_count = fn->as.fn.params.count;
    size_t context_count = fn->as.fn.contexts.count;
    JogType **params = alloc_types(ck, param_count);
    JogType **contexts = alloc_types(ck, context_count);
    JogType *ret;
    JogType *built;
    bool bad = false;
    if (ck->stop) {
        return NULL;
    }
    if ((param_count > 0 && !params) || (context_count > 0 && !contexts)) {
        return NULL;
    }
    if (!jog_generic_bind_params(ck, job, &bad)) {
        return NULL;
    }
    if (fn->as.fn.receiver) {
        JogType *receiver;
        JogSymbol *owner = NULL;
        size_t nparams = 0;
        int kind = generic_receiver_kind(job, fn->as.fn.receiver, &nparams, &owner);
        int skip_recv = 0;
        if (kind == 2) {
            /* A generic alias is the substituted target, not a nominal type
               a method can be declared on. */
            fail(ck, fn->pos, "generic methods are not checked yet");
            fn->as.fn.receiver->type = error_type(ck);
            bad = true;
            receiver = fn->as.fn.receiver->type;
        } else if (kind == 1) {
            if (!jog_generic_bind_receiver_params(ck, job, owner, nparams, &bad, &skip_recv)) {
                return NULL;
            }
            if (skip_recv) {
                fn->as.fn.receiver->type = error_type(ck);
                receiver = fn->as.fn.receiver->type;
            } else {
                receiver = resolve_type(ck, job, fn->as.fn.receiver);
            }
        } else {
            receiver = resolve_type(ck, job, fn->as.fn.receiver);
        }
        if (!receiver) {
            return NULL;
        }
        if (receiver->kind == JOG_TY_ERROR) {
            bad = true;
        }
    }
    if (!resolve_params(ck, job, fn->as.fn.params, params, &bad)) {
        return NULL;
    }
    {
        size_t i;
        for (i = 0; i < param_count; i++) {
            JogNode *param = list_at(fn->as.fn.params, i);
            if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.pack) {
                continue;
            }
            if (i + 1 != param_count) {
                fail(ck, param->pos, "a pack must be the last parameter");
                bad = true;
            }
        }
    }
    if (!resolve_params(ck, job, fn->as.fn.contexts, contexts, &bad)) {
        return NULL;
    }
    jog_generic_reject_param_defaults(ck, fn, &bad);
    {
        size_t i;
        for (i = 0; i < context_count; i++) {
            JogNode *param = list_at(fn->as.fn.contexts, i);
            if (param && param->kind == JOG_NODE_PARAM && param->as.param.default_value) {
                fail(ck, param->pos, "a context parameter cannot have a default");
                bad = true;
            }
        }
    }
    if (context_count > 0 && fn_has_attr(fn, "export")) {
        fail(ck, fn->pos, "an exported function cannot declare contexts");
        bad = true;
    }
    if (context_count > 0 && !fn->as.fn.receiver && fn->as.fn.name && strcmp(fn->as.fn.name, "main") == 0 &&
        job->package == ck->program->root) {
        fail(ck, fn->pos, "fn main cannot declare contexts yet");
        bad = true;
    }
    if (fn->as.fn.ret) {
        ret = resolve_type(ck, job, fn->as.fn.ret);
        if (!ret) {
            return NULL;
        }
        if (ret->kind == JOG_TY_ERROR) {
            bad = true;
        }
    } else {
        ret = unit_type(ck);
        if (!ret) {
            return NULL;
        }
    }
    if (bad || ck->stop) {
        return ck->stop ? NULL : error_type(ck);
    }
    built = jog_type_fn(ck->program->types, params, param_count, ret, contexts, context_count, fn->as.fn.variadic, false,
                        fn->as.fn.is_extern);
    if (!built) {
        oom(ck);
        return NULL;
    }
    return built;
}

static const char *binding_name(const JogNode *node) {
    if (!node || !node->as.binding.names.items || node->as.binding.names.count == 0) {
        return "?";
    }
    if (!node->as.binding.names.items[0] || node->as.binding.names.items[0][0] == '\0') {
        return "?";
    }
    return node->as.binding.names.items[0];
}

static JogType *check_binding(Check *ck, CheckJob *job) {
    JogNode *node = job->decl;
    JogType *annotated = NULL;
    JogType *got;
    JogType *hint;
    if ((node->kind == JOG_NODE_VAL || node->kind == JOG_NODE_VAR) && node->as.binding.destructure &&
        node->as.binding.names.count > 1) {
        if (node->as.binding.init) {
            demand_uses(ck, job, node->as.binding.init);
            if (ck->stop) {
                return NULL;
            }
        }
        return jog_check_error_top_binding(ck, job, node);
    }
    if (node->as.binding.type) {
        annotated = resolve_type(ck, job, node->as.binding.type);
        if (!annotated) {
            return NULL;
        }
    }
    if (node->kind == JOG_NODE_CONTEXT && !node->as.binding.init) {
        fail(ck, node->pos, "context requires an initializer");
        return error_type(ck);
    }
    /* Publish an annotated context before its initializer. A call in that
       initializer can match this context without waiting for the job to finish. */
    if (node->kind == JOG_NODE_CONTEXT && annotated && annotated->kind != JOG_TY_ERROR && job->symbol) {
        job->symbol->type = annotated;
        node->type = annotated;
    }
    if (node->as.binding.init) {
        demand_uses(ck, job, node->as.binding.init);
        if (ck->stop) {
            return NULL;
        }
    }
    if (annotated && annotated->kind == JOG_TY_ERROR) {
        if (node->as.binding.init) {
            (void)jog_check_expr(ck, job, NULL, node->as.binding.init, NULL);
            if (ck->stop) {
                return NULL;
            }
        }
        return annotated;
    }
    if (node->as.binding.init) {
        hint = annotated;
        got = jog_check_expr(ck, job, NULL, node->as.binding.init, hint);
        if (ck->stop) {
            return NULL;
        }
        if (got && got->kind == JOG_TY_NULL) {
            fail(ck, node->pos, "null needs an option type");
            return error_type(ck);
        }
        if (annotated) {
            return annotated;
        }
        return got;
    }
    if (node->kind == JOG_NODE_VAR && annotated && !jog_check_type_has_zero(annotated)) {
        fail(ck, node->pos, "variable '%s' must be initialized", binding_name(node));
    }
    return annotated;
}

static void check_decl(Check *ck, CheckJob *job) {
    JogType *type = NULL;
    switch (job->decl->kind) {
    case JOG_NODE_TYPE_DECL:
        type = check_type_decl(ck, job);
        break;
    case JOG_NODE_FN:
        type = check_fn(ck, job);
        if (type && type->kind == JOG_TY_FN && job->decl->as.fn.receiver) {
            type = jog_check_method_type(ck, job, type);
        }
        if (type && type->kind == JOG_TY_FN && jog_check_context_should_infer(job)) {
            type = jog_check_context_infer(ck, job, type);
        }
        break;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
        type = check_binding(ck, job);
        break;
    case JOG_NODE_STRUCT:
        type = jog_check_struct(ck, job);
        break;
    case JOG_NODE_INTERFACE:
        if (job->decl->as.aggregate.type_params.count > 0) {
            fail(ck, job->decl->pos, "generic types are not checked yet");
            type = error_type(ck);
            break;
        }
        type = jog_check_interface(ck, job);
        break;
    case JOG_NODE_ENUM:
        type = jog_check_enum(ck, job);
        break;
    default:
        break;
    }
    if (ck->stop) {
        finish_job(job, error_type(ck));
        return;
    }
    finish_job(job, type);
    /* Publish the signature before the body so a call waits on the callee's
       type, not on its body. Mutual calls are not cycles. */
    if (job->decl->kind == JOG_NODE_FN && type && type->kind != JOG_TY_ERROR && job->decl->as.fn.body) {
        jog_check_fn_body(ck, job);
    }
}

static void run_job(void *arg) {
    CheckJob *job = arg;
    job->check->current_job = job;
    if (!job->check->stop) {
        check_decl(job->check, job);
    }
    if (!job->done) {
        finish_job(job, error_type(job->check));
    }
}

static int gather_symbols(Check *ck, CheckJob *job) {
    JogNode *node = job->decl;
    size_t count;
    size_t i;
    size_t filled = 0;
    if (node->kind == JOG_NODE_VAR || node->kind == JOG_NODE_VAL || node->kind == JOG_NODE_CONTEXT) {
        count = node->as.binding.names.count;
        if (count == 0 || !node->as.binding.names.items) {
            return 0;
        }
        job->symbols = ct_arena_alloc(ck->program->arena, count * sizeof(JogSymbol *), _Alignof(JogSymbol *));
        if (!job->symbols) {
            oom(ck);
            return -1;
        }
        for (i = 0; i < count; i++) {
            JogSymbol *symbol = lookup_binding(job, node->as.binding.names.items[i]);
            if (symbol) {
                job->symbols[filled++] = symbol;
            }
        }
        job->symbol_count = filled;
        job->symbol = filled > 0 ? job->symbols[0] : NULL;
        return filled > 0 ? 1 : 0;
    }
    if (!node->symbol) {
        return 0;
    }
    job->symbols = ct_arena_alloc(ck->program->arena, sizeof(JogSymbol *), _Alignof(JogSymbol *));
    if (!job->symbols) {
        oom(ck);
        return -1;
    }
    job->symbols[0] = node->symbol;
    job->symbol_count = 1;
    job->symbol = node->symbol;
    return 1;
}

static bool push_job(Check *ck, CtDa *jobs, JogPackage *pkg, size_t file_index, JogNode *node) {
    CheckJob job;
    int got;
    memset(&job, 0, sizeof(job));
    job.check = ck;
    job.decl = node;
    job.package = pkg;
    job.file_index = file_index;
    got = gather_symbols(ck, &job);
    if (got < 0) {
        return false;
    }
    if (got == 0) {
        return true;
    }
    if (!ct_da_push(jobs, &job)) {
        oom(ck);
        return false;
    }
    return true;
}

static bool schedule_node(Check *ck, CtDa *jobs, JogPackage *pkg, size_t file_index, JogNode *node) {
    size_t i;
    if (!node) {
        return true;
    }
    if (node->kind == JOG_NODE_EXTERN_BLOCK) {
        for (i = 0; i < node->as.extern_block.fns.count; i++) {
            if (!schedule_node(ck, jobs, pkg, file_index, list_at(node->as.extern_block.fns, i))) {
                return false;
            }
        }
        return true;
    }
    if (!is_scheduled(node->kind)) {
        return true;
    }
    return push_job(ck, jobs, pkg, file_index, node);
}

static bool collect_jobs(Check *ck) {
    CtDa jobs;
    size_t p;
    size_t f;
    size_t d;
    size_t n;
    if (!ct_da_init(&jobs, sizeof(CheckJob))) {
        oom(ck);
        return false;
    }
    for (p = 0; p < ck->program->package_count; p++) {
        JogPackage *pkg = ck->program->packages ? ck->program->packages[p] : NULL;
        if (!pkg || !pkg->files) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files[f];
            JogNode *file;
            if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                continue;
            }
            file = source->ast;
            for (d = 0; d < file->as.file.decls.count; d++) {
                if (!schedule_node(ck, &jobs, pkg, f, list_at(file->as.file.decls, d))) {
                    ct_da_free(&jobs);
                    return false;
                }
            }
        }
    }
    n = jobs.count;
    if (n == 0) {
        ct_da_free(&jobs);
        return !ck->failed;
    }
    ck->jobs = ct_arena_alloc(ck->program->arena, n * sizeof(CheckJob), _Alignof(CheckJob));
    if (!ck->jobs || !jobs.data) {
        oom(ck);
        ct_da_free(&jobs);
        return false;
    }
    memcpy(ck->jobs, jobs.data, n * sizeof(CheckJob));
    ck->job_count = n;
    ct_da_free(&jobs);
    for (p = 0; p < n; p++) {
        ck->jobs[p].check = ck;
    }
    return true;
}

static void report_cycle(Check *ck, const size_t *idxs, size_t len) {
    size_t best = 0;
    size_t i;
    CtSb sb;
    CheckJob *first;
    if (len == 0) {
        return;
    }
    for (i = 1; i < len; i++) {
        if (idxs[i] < idxs[best]) {
            best = i;
        }
    }
    ct_sb_init(&sb);
    for (i = 0; i < len; i++) {
        const char *name = job_name(&ck->jobs[idxs[(best + i) % len]]);
        if (i > 0 && !ct_sb_append_cstr(&sb, " -> ")) {
            break;
        }
        if (!ct_sb_append_cstr(&sb, name)) {
            break;
        }
    }
    first = &ck->jobs[idxs[best]];
    if (sb.failed || !ct_sb_append_cstr(&sb, " -> ") || !ct_sb_append_cstr(&sb, job_name(first))) {
        fail(ck, first->decl ? first->decl->pos : no_pos(), "declaration cycle");
    } else {
        fail(ck, first->decl ? first->decl->pos : no_pos(), "declaration cycle: %s", ct_sb_cstr(&sb));
    }
    ct_sb_free(&sb);
}

static void report_wait_cycles(Check *ck) {
    size_t n = ck->job_count;
    size_t i;
    unsigned char *color;
    size_t *path;
    if (n == 0) {
        return;
    }
    color = ct_arena_alloc(ck->program->arena, n, 1);
    path = ct_arena_alloc(ck->program->arena, n * sizeof(size_t), _Alignof(size_t));
    if (!color || !path) {
        oom(ck);
        return;
    }
    memset(color, 0, n);
    for (i = 0; i < n; i++) {
        size_t at = i;
        size_t len = 0;
        size_t guard = 0;
        size_t k;
        if (color[i] || ck->jobs[i].done || !ck->jobs[i].parked || !ck->jobs[i].waiting_for) {
            continue;
        }
        while (guard++ <= n) {
            size_t found = n;
            CheckJob *next;
            if (at >= n || color[at] == 2) {
                break;
            }
            for (k = 0; k < len; k++) {
                if (path[k] == at) {
                    found = k;
                    break;
                }
            }
            if (found < n) {
                report_cycle(ck, path + found, len - found);
                break;
            }
            if (len >= n) {
                break;
            }
            path[len++] = at;
            next = ck->jobs[at].waiting_for;
            if (!next || next->done) {
                break;
            }
            at = job_index(ck, next);
        }
        for (k = 0; k < len; k++) {
            color[path[k]] = 2;
        }
    }
}

static bool blocked(const Check *ck) {
    size_t i;
    bool any = false;
    for (i = 0; i < ck->job_count; i++) {
        const CheckJob *job = &ck->jobs[i];
        if (job->done) {
            continue;
        }
        any = true;
        if (!job->parked || !job->waiting_for || job->waiting_for->done) {
            return false;
        }
    }
    return any;
}

static bool run_jobs(Check *ck) {
    size_t i;
    size_t spins = 0;
    size_t limit = ck->job_count * 2 + 8;
    if (!coroutine_init()) {
        fail(ck, no_pos(), "cannot start checker");
        return false;
    }
    for (i = 0; i < ck->job_count; i++) {
        if (!coroutine_go(run_job, &ck->jobs[i])) {
            ck->stop = true;
            fail(ck, no_pos(), "cannot start checker");
            break;
        }
    }
    while (coroutine_alive() > 1) {
        coroutine_yield();
        if (!ck->stop && blocked(ck)) {
            if (!jog_check_context_wake_type(ck) && !jog_check_context_solve(ck)) {
                report_wait_cycles(ck);
                ck->stop = true;
            }
        }
        if (!ck->stop) {
            spins++;
            if (spins > limit) {
                fail(ck, no_pos(), "declaration check did not finish");
                ck->stop = true;
            }
        }
    }
    coroutine_shutdown();
    return !ck->failed;
}

void jog_check_fail(Check *ck, JogPos pos, const char *fmt, ...) {
    char buf[512];
    va_list args;
    int wrote;
    va_start(args, fmt);
    /* Collection walks the body to discover contexts. The real pass reports. */
    if (ck->current_job && ck->current_job->infer_collecting) {
        va_end(args);
        return;
    }
    wrote = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (wrote < 0) {
        buf[0] = '\0';
    }
    fail(ck, pos, "%s", buf);
}

void jog_check_oom(Check *ck) {
    oom(ck);
}

JogType *jog_check_error_type(Check *ck) {
    return error_type(ck);
}

JogType *jog_check_unit_type(Check *ck) {
    return unit_type(ck);
}

JogType *jog_check_resolve_type(Check *ck, CheckJob *job, JogNode *node) {
    return resolve_type(ck, job, node);
}

bool jog_check_wait(Check *ck, CheckJob *job, JogSymbol *symbol) {
    /* A collecting private function may use another private function's
       parameter list before that function's inferred with clause is done.
       Waiting for done would deadlock the group. */
    if (ck && job && job->infer_collecting && symbol && !ck->stop) {
        CheckJob *other = job_for(ck, symbol);
        if (other && other->decl && other->decl->kind == JOG_NODE_FN && !other->done) {
            while (!other->done && !other->sig_ready && !ck->stop) {
                job->waiting_for = other;
                job->parked = true;
                coroutine_yield();
                ck->current_job = job;
                job->parked = false;
                job->waiting_for = NULL;
            }
            return !ck->stop;
        }
    }
    return wait_for_symbol(ck, job, symbol, false);
}

bool jog_check_wait_finished(Check *ck, CheckJob *job, JogSymbol *symbol) {
    return wait_for_symbol(ck, job, symbol, true);
}

JogSymbol *jog_check_lookup_file(const CheckJob *job, const char *name) {
    return lookup_in_file(job, name);
}

/* An import the file never names. A failed import has no symbol and
   already reported why. A parameter that hides the name marks it used. */
static void report_unused_imports(Check *ck) {
    size_t p;
    size_t f;
    size_t i;
    if (!ck || !ck->program) {
        return;
    }
    for (p = 0; p < ck->program->package_count; p++) {
        JogPackage *pkg = ck->program->packages ? ck->program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
            JogNode *file = source ? source->ast : NULL;
            if (!file || file->kind != JOG_NODE_FILE) {
                continue;
            }
            for (i = 0; i < file->as.file.imports.count; i++) {
                JogNode *import = file->as.file.imports.items ? file->as.file.imports.items[i] : NULL;
                JogSymbol *sym = import ? import->symbol : NULL;
                if (!sym || sym->kind != JOG_SYM_PACKAGE || sym->referenced) {
                    continue;
                }
                fail(ck, import->pos, "unused import '%s'", sym->name ? sym->name : "?");
            }
        }
    }
}

bool jog_check(JogProgram *program) {
    JogPackageScope *universe;
    Check ck;
    if (!program || !program->arena || !program->diag || !program->intern || !program->types) {
        if (program && program->diag) {
            CtPos pos = {0};
            ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "checker is missing program state");
        }
        return false;
    }
    universe = jog_universe_scope(program->arena, program->intern, program->types);
    if (!universe) {
        CtPos pos = {0};
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return false;
    }
    if (!program->core_root && !jog_locate_roots(program)) {
        return false;
    }
    if (!jog_bind_prelude(program->arena, program->diag, program->intern, program->types, universe,
                          program->core_root)) {
        return false;
    }
    if (!jog_resolve_packages(program, universe)) {
        return false;
    }
    if (!jog_reject_intrinsic(program)) {
        return false;
    }
    if (!jog_check_platform(program)) {
        return false;
    }
    memset(&ck, 0, sizeof(ck));
    ck.program = program;
    if (!collect_jobs(&ck)) {
        return false;
    }
    if (ck.job_count > 0 && !run_jobs(&ck) && ck.stop) {
        jog_check_context_cleanup(&ck);
        return false;
    }
    if (!ck.stop) {
        jog_check_method_duplicates(&ck);
    }
    if (!ck.failed && !ck.stop) {
        jog_check_package_tests(&ck);
    }
    if (!ck.stop) {
        report_unused_imports(&ck);
    }
    jog_check_context_cleanup(&ck);
    return !ck.failed;
}
