#include "check_interface.h"

#include "check_constraint.h"
#include "check_context.h"
#include "check_error.h"
#include "check_struct.h"

#include <stdio.h>
#include <string.h>

/* Structural method satisfaction. Requirements live on the interface
   declaration and have no receiver parameter. A concrete method satisfies
   one when it is declared in the same package, on that nominal type, with
   the same pointer-ness, parameters, result, and contexts, and with no
   type parameters of its own. Extensions in other packages do not count.
   *const T converts only when every requirement has a value receiver. */

enum {
    FIT_OK = 0,
    FIT_NOT = 1,
    FIT_MISSING = 2,
    FIT_GENERIC = 3,
    FIT_RECV = 4,
    FIT_SIG = 5,
    FIT_CONST = 6,
    FIT_AMBIG = 7
};

typedef struct Fit {
    int kind;
    const char *method;
    int needs_ptr;
    int is_prop;
} Fit;

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static int is_universe_error(const JogType *type) {
    const char *name;
    if (!type || type->kind != JOG_TY_INTERFACE) {
        return 0;
    }
    if (type->as.nominal.package != NULL || type->as.nominal.decl != NULL) {
        return 0;
    }
    name = type->as.nominal.name;
    return name != NULL && strcmp(name, "error") == 0;
}

static int is_user_nominal(const JogType *type) {
    if (!type || (type->kind != JOG_TY_STRUCT && type->kind != JOG_TY_ENUM)) {
        return 0;
    }
    return type->as.nominal.decl != NULL;
}

static void type_text(const JogType *type, char *buf, size_t cap) {
    CtSb sb;
    const char *text;
    if (!buf || cap == 0) {
        return;
    }
    buf[0] = '\0';
    ct_sb_init(&sb);
    if (type) {
        jog_type_print(type, &sb);
    }
    text = ct_sb_cstr(&sb);
    snprintf(buf, cap, "%s", text && text[0] != '\0' ? text : "?");
    ct_sb_free(&sb);
}

static void publish(CheckJob *job, JogType *type) {
    size_t i;
    if (!job) {
        return;
    }
    if (job->decl) {
        job->decl->type = type;
    }
    for (i = 0; i < job->symbol_count; i++) {
        if (job->symbols && job->symbols[i]) {
            job->symbols[i]->type = type;
        }
    }
}

static const char *iface_name(const JogType *type) {
    if (type && type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    return "?";
}

static const char *concrete_name(const JogType *type) {
    if (type && type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    return "?";
}

static void explain(Check *ck, JogPos pos, JogType *have, JogType *need, Fit fit) {
    char a[160];
    char b[160];
    const char *iface = iface_name(need);
    const char *method = fit.method ? fit.method : "?";
    switch (fit.kind) {
    case FIT_MISSING:
        if (fit.is_prop) {
            jog_check_fail(ck, pos, "type '%s' is missing property '%s' for interface '%s'", concrete_name(have), method,
                           iface);
            return;
        }
        jog_check_fail(ck, pos, "type '%s' is missing method '%s' for interface '%s'", concrete_name(have), method,
                       iface);
        return;
    case FIT_GENERIC:
        jog_check_fail(ck, pos, "generic method '%s' cannot satisfy interface '%s'", method, iface);
        return;
    case FIT_RECV:
        if (fit.needs_ptr) {
            jog_check_fail(ck, pos, "method '%s' needs a pointer receiver", method);
        } else {
            jog_check_fail(ck, pos, "method '%s' has a pointer receiver", method);
        }
        return;
    case FIT_SIG:
        if (fit.is_prop) {
            jog_check_fail(ck, pos, "property '%s' has the wrong type for interface '%s'", method, iface);
            return;
        }
        jog_check_fail(ck, pos, "method '%s' has the wrong signature for interface '%s'", method, iface);
        return;
    case FIT_CONST:
        type_text(have, a, sizeof(a));
        type_text(need, b, sizeof(b));
        jog_check_fail(ck, pos, "cannot convert %s to %s", a, b);
        return;
    case FIT_AMBIG:
        if (fit.is_prop) {
            jog_check_fail(ck, pos, "ambiguous field '%s'", method);
            return;
        }
        jog_check_fail(ck, pos, "ambiguous method '%s' for interface '%s'", method, iface);
        return;
    default:
        return;
    }
}

static int same_pkg(const JogType *type, const JogSymbol *sym) {
    const char *want = type && type->as.nominal.package ? type->as.nominal.package : "";
    const char *have = sym && sym->package && sym->package->path ? sym->package->path : "";
    return strcmp(want, have) == 0;
}

static int sig_ok(const JogType *method, const JogType *req) {
    size_t i;
    if (!method || !req || method->kind != JOG_TY_FN || req->kind != JOG_TY_FN) {
        return 0;
    }
    if (method->as.fn.variadic || req->as.fn.variadic) {
        return 0;
    }
    if (method->as.fn.param_count != req->as.fn.param_count + 1) {
        return 0;
    }
    if (method->as.fn.context_count != req->as.fn.context_count) {
        return 0;
    }
    if (method->as.fn.ret != req->as.fn.ret) {
        return 0;
    }
    if (req->as.fn.param_count > 0 && (!method->as.fn.params || !req->as.fn.params)) {
        return 0;
    }
    for (i = 0; i < req->as.fn.param_count; i++) {
        if (method->as.fn.params[i + 1] != req->as.fn.params[i]) {
            return 0;
        }
    }
    if (req->as.fn.context_count > 0 && (!method->as.fn.contexts || !req->as.fn.contexts)) {
        return 0;
    }
    for (i = 0; i < req->as.fn.context_count; i++) {
        if (method->as.fn.contexts[i] != req->as.fn.contexts[i]) {
            return 0;
        }
    }
    return 1;
}

static int any_ptr_req_depth(const JogType *iface, int depth) {
    JogNode *decl;
    size_t i;
    if (!iface || !iface->as.nominal.decl || depth > 16) {
        return 0;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return 0;
    }
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN && member->as.fn.receiver_is_ptr) {
            return 1;
        }
        if (member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
            continue;
        }
        emb = member->as.embed.type->type;
        if (emb && emb->kind == JOG_TY_INTERFACE && !jog_constraint_is_typeset(emb) &&
            any_ptr_req_depth(emb, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

static int any_ptr_req(const JogType *iface) {
    return any_ptr_req_depth(iface, 0);
}

/* FIT_ code, or -1 when the checker stopped. A var property needs a
   mutable pointer. The field type matches exactly. */
static int prop_fit(Check *ck, CheckJob *job, JogType *elem, JogType *have, JogNode *prop, Fit *fit) {
    JogStructPath hit;
    JogSymbol *tsym;
    const char *file;
    const char *name;
    if (!prop || prop->kind != JOG_NODE_FIELD) {
        return FIT_NOT;
    }
    name = prop->as.field.name ? prop->as.field.name : "?";
    if (have && have->is_const && prop->as.field.is_var) {
        if (fit) {
            fit->kind = FIT_CONST;
            fit->method = name;
            fit->is_prop = 1;
        }
        return FIT_CONST;
    }
    if (!prop->as.field.name || !prop->type || elem->kind != JOG_TY_STRUCT) {
        if (fit) {
            fit->kind = FIT_MISSING;
            fit->method = name;
            fit->is_prop = 1;
        }
        return FIT_MISSING;
    }
    tsym = elem->as.nominal.decl ? elem->as.nominal.decl->symbol : NULL;
    if (tsym && (!job || job->symbol != tsym)) {
        if (!jog_check_wait_finished(ck, job, tsym)) {
            return -1;
        }
    }
    if (ck->stop) {
        return -1;
    }
    file = job && job->decl ? job->decl->pos.file : NULL;
    memset(&hit, 0, sizeof(hit));
    jog_struct_lookup_field(elem, prop->as.field.name, job ? job->decl : NULL, job ? job->package : NULL, file, &hit);
    if (hit.ambiguous) {
        if (fit) {
            fit->kind = FIT_AMBIG;
            fit->method = name;
            fit->is_prop = 1;
        }
        return FIT_AMBIG;
    }
    if (hit.hidden || !hit.found || !hit.field) {
        if (fit) {
            fit->kind = FIT_MISSING;
            fit->method = name;
            fit->is_prop = 1;
        }
        return FIT_MISSING;
    }
    if (prop->type->kind != JOG_TY_ERROR && hit.field->type != prop->type) {
        if (fit) {
            fit->kind = FIT_SIG;
            fit->method = name;
            fit->is_prop = 1;
        }
        return FIT_SIG;
    }
    return FIT_OK;
}

/* -1 when the checker stopped. Otherwise a FIT_ code. have_elem is the
   pointed-to struct or enum when kind is not FIT_NOT. */
static int satisfy(Check *ck, CheckJob *job, JogType *have, JogType *need, Fit *fit, int depth) {
    JogType *elem;
    JogNode *decl;
    JogSymbol *isym;
    size_t i;
    int failed;
    if (fit) {
        fit->kind = FIT_NOT;
        fit->method = NULL;
        fit->needs_ptr = 0;
        fit->is_prop = 0;
    }
    if (!ck || ck->stop || !have || !need || need->kind != JOG_TY_INTERFACE || is_universe_error(need)) {
        return ck && ck->stop ? -1 : FIT_NOT;
    }
    if (depth > 16 || jog_constraint_is_typeset(need)) {
        return FIT_NOT;
    }
    if (have->kind != JOG_TY_PTR) {
        return FIT_NOT;
    }
    elem = have->as.elem.elem;
    if (!is_user_nominal(elem)) {
        return FIT_NOT;
    }
    decl = need->as.nominal.decl;
    if (!decl || decl->kind != JOG_NODE_INTERFACE) {
        return FIT_NOT;
    }
    if (job && job->decl == decl) {
        return FIT_NOT;
    }
    isym = decl->symbol;
    if (isym && (!job || job->symbol != isym)) {
        if (!jog_check_wait_finished(ck, job, isym)) {
            return -1;
        }
    }
    if (ck->stop) {
        return -1;
    }
    if (have->is_const && any_ptr_req(need)) {
        if (fit) {
            fit->kind = FIT_CONST;
        }
        return FIT_CONST;
    }
    failed = FIT_OK;
    for (i = 0; i < decl->as.aggregate.members.count && !ck->stop; i++) {
        JogNode *req = list_at(decl->as.aggregate.members, i);
        JogType *req_ty;
        const char *name;
        size_t p;
        int full = 0;
        int saw_generic = 0;
        int saw_recv = 0;
        int saw_sig = 0;
        int needs_ptr = 0;
        const char *bad_name = NULL;
        if (!req) {
            continue;
        }
        if (req->kind == JOG_NODE_EMBED) {
            JogType *emb = req->as.embed.type ? req->as.embed.type->type : NULL;
            int sub;
            if (!emb || emb->kind != JOG_TY_INTERFACE || jog_constraint_is_typeset(emb)) {
                continue;
            }
            sub = satisfy(ck, job, have, emb, fit, depth + 1);
            if (sub != FIT_OK) {
                return sub;
            }
            continue;
        }
        if (req->kind == JOG_NODE_FIELD) {
            int sub = prop_fit(ck, job, elem, have, req, fit);
            if (sub != FIT_OK) {
                return sub;
            }
            continue;
        }
        if (req->kind != JOG_NODE_FN || !req->as.fn.name) {
            continue;
        }
        name = req->as.fn.name;
        req_ty = req->type;
        if (!req_ty || req_ty->kind != JOG_TY_FN) {
            if (fit) {
                fit->kind = FIT_MISSING;
                fit->method = name;
            }
            return FIT_MISSING;
        }
        for (p = 0; p < ck->program->package_count; p++) {
            JogPackage *pkg = ck->program->packages[p];
            size_t f;
            if (!pkg) {
                continue;
            }
            for (f = 0; f < pkg->file_count; f++) {
                JogSourceFile *source = pkg->files ? pkg->files[f] : NULL;
                JogNode *file;
                size_t d;
                if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
                    continue;
                }
                file = source->ast;
                for (d = 0; d < file->as.file.decls.count; d++) {
                    JogNode *fn = list_at(file->as.file.decls, d);
                    JogSymbol *sym;
                    JogType *on = NULL;
                    int is_ptr = 0;
                    if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.receiver || !fn->as.fn.name) {
                        continue;
                    }
                    if (strcmp(fn->as.fn.name, name) != 0) {
                        continue;
                    }
                    sym = fn->symbol;
                    if (!sym || !same_pkg(elem, sym)) {
                        continue;
                    }
                    if (!sym->type) {
                        if (job && job->symbol == sym) {
                            continue;
                        }
                        if (!jog_check_wait(ck, job, sym)) {
                            return -1;
                        }
                    }
                    if (!sym->type || sym->type->kind != JOG_TY_FN) {
                        continue;
                    }
                    jog_method_receiver(fn, &on, &is_ptr);
                    if (!on || on->kind != elem->kind || !on->as.nominal.decl || on->as.nominal.decl != elem->as.nominal.decl) {
                        continue;
                    }
                    if (sym->type_param_count > 0) {
                        saw_generic = 1;
                        bad_name = name;
                        continue;
                    }
                    if (on != elem) {
                        continue;
                    }
                    if ((is_ptr ? 1 : 0) != (req->as.fn.receiver_is_ptr ? 1 : 0)) {
                        saw_recv = 1;
                        needs_ptr = req->as.fn.receiver_is_ptr ? 1 : 0;
                        bad_name = name;
                        continue;
                    }
                    if (!sig_ok(sym->type, req_ty)) {
                        saw_sig = 1;
                        bad_name = name;
                        continue;
                    }
                    full++;
                    bad_name = name;
                }
            }
        }
        if (ck->stop) {
            return -1;
        }
        if (full == 1) {
            continue;
        }
        if (full > 1) {
            if (fit) {
                fit->kind = FIT_AMBIG;
                fit->method = name;
            }
            return FIT_AMBIG;
        }
        if (saw_sig) {
            failed = FIT_SIG;
        } else if (saw_recv) {
            failed = FIT_RECV;
        } else if (saw_generic) {
            failed = FIT_GENERIC;
        } else {
            failed = FIT_MISSING;
        }
        if (fit) {
            fit->kind = failed;
            fit->method = bad_name ? bad_name : name;
            fit->needs_ptr = needs_ptr;
        }
        return failed;
    }
    if (ck->stop) {
        return -1;
    }
    if (fit) {
        fit->kind = FIT_OK;
    }
    return FIT_OK;
}

bool jog_iface_converts(Check *ck, JogType *have, JogType *need) {
    Fit fit;
    int kind = satisfy(ck, ck ? ck->current_job : NULL, have, need, &fit, 0);
    return kind == FIT_OK;
}

int jog_iface_expect(Check *ck, JogNode *node, JogType *got, JogType *want, JogType **out) {
    Fit fit;
    int kind;
    int concrete;
    JogPos pos;
    if (out) {
        *out = NULL;
    }
    concrete = jog_check_error_expect(ck, node, got, want, out);
    if (concrete != 0) {
        return concrete;
    }
    if (!ck || !want || want->kind != JOG_TY_INTERFACE) {
        return 0;
    }
    pos = node ? node->pos : (JogPos){0};
    kind = satisfy(ck, ck->current_job, got, want, &fit, 0);
    if (kind < 0) {
        return -1;
    }
    if (kind == FIT_NOT) {
        return 0;
    }
    if (kind == FIT_OK) {
        if (out) {
            *out = want;
        }
        return 1;
    }
    if (kind == FIT_MISSING || kind == FIT_GENERIC || kind == FIT_RECV || kind == FIT_SIG) {
        JogType *elem = got && got->kind == JOG_TY_PTR ? got->as.elem.elem : got;
        explain(ck, pos, elem, want, fit);
    } else {
        explain(ck, pos, got, want, fit);
    }
    if (out) {
        *out = jog_check_error_type(ck);
    }
    if (node && out) {
        node->type = *out;
    }
    return 1;
}

static JogType *build_req(Check *ck, CheckJob *job, JogNode *fn, int *bad) {
    size_t param_count = fn->as.fn.params.count;
    size_t context_count = fn->as.fn.contexts.count;
    JogType **params = NULL;
    JogType **contexts = NULL;
    JogType *ret;
    JogType *built;
    size_t i;
    if (param_count > 0) {
        params = ct_arena_alloc(ck->program->arena, param_count * sizeof(*params), _Alignof(JogType *));
        if (!params) {
            jog_check_oom(ck);
            return NULL;
        }
        memset(params, 0, param_count * sizeof(*params));
    }
    if (context_count > 0) {
        contexts = ct_arena_alloc(ck->program->arena, context_count * sizeof(*contexts), _Alignof(JogType *));
        if (!contexts) {
            jog_check_oom(ck);
            return NULL;
        }
        memset(contexts, 0, context_count * sizeof(*contexts));
    }
    for (i = 0; i < param_count && !ck->stop; i++) {
        JogNode *param = list_at(fn->as.fn.params, i);
        JogType *type;
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.type) {
            jog_check_fail(ck, param ? param->pos : fn->pos, "missing parameter type");
            *bad = 1;
            continue;
        }
        type = jog_check_resolve_type(ck, job, param->as.param.type);
        if (!type) {
            return NULL;
        }
        param->type = type;
        if (params) {
            params[i] = type;
        }
        if (type->kind == JOG_TY_ERROR) {
            *bad = 1;
        }
    }
    for (i = 0; i < context_count && !ck->stop; i++) {
        JogNode *param = list_at(fn->as.fn.contexts, i);
        JogType *type;
        if (!param || param->kind != JOG_NODE_PARAM || !param->as.param.type) {
            jog_check_fail(ck, param ? param->pos : fn->pos, "missing parameter type");
            *bad = 1;
            continue;
        }
        if (param->as.param.default_value) {
            jog_check_fail(ck, param->pos, "a context parameter cannot have a default");
            *bad = 1;
        }
        type = jog_check_resolve_type(ck, job, param->as.param.type);
        if (!type) {
            return NULL;
        }
        param->type = type;
        if (contexts) {
            contexts[i] = type;
        }
        if (type->kind == JOG_TY_ERROR) {
            *bad = 1;
        }
    }
    if (ck->stop) {
        return NULL;
    }
    if (fn->as.fn.ret) {
        ret = jog_check_resolve_type(ck, job, fn->as.fn.ret);
        if (!ret) {
            return NULL;
        }
        if (ret->kind == JOG_TY_ERROR) {
            *bad = 1;
        }
    } else {
        ret = jog_check_unit_type(ck);
        if (!ret) {
            return NULL;
        }
    }
    if (*bad) {
        return jog_check_error_type(ck);
    }
    built = jog_type_fn(ck->program->types, params, param_count, ret, contexts, context_count, false, false, false);
    if (!built) {
        jog_check_oom(ck);
        return NULL;
    }
    return built;
}

JogType *jog_check_interface(Check *ck, CheckJob *job) {
    JogNode *node;
    JogType *type;
    size_t i;
    if (!ck || !job || !job->decl) {
        return NULL;
    }
    node = job->decl;
    type = jog_type_new_nominal(ck->program->types, JOG_TY_INTERFACE, node->as.aggregate.name,
                                job->package ? job->package->path : NULL, node);
    if (!type) {
        jog_check_oom(ck);
        return NULL;
    }
    publish(job, type);
    if (!jog_type_layout(ck->program->types, type)) {
        jog_check_oom(ck);
        return NULL;
    }
    for (i = 0; i < node->as.aggregate.members.count && !ck->stop; i++) {
        JogNode *member = list_at(node->as.aggregate.members, i);
        JogType *fn_ty;
        JogSymbol *sym;
        size_t prev;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD || member->kind == JOG_NODE_EMBED || member->kind == JOG_NODE_TYPE_SET) {
            if (!jog_constraint_iface_member(ck, job, node, member)) {
                return NULL;
            }
            continue;
        }
        if (member->kind != JOG_NODE_FN) {
            jog_check_fail(ck, member->pos, "interface member is not checked yet");
            continue;
        }
        if (!member->as.fn.name || member->as.fn.name[0] == '\0') {
            jog_check_fail(ck, member->pos, "missing method name");
            continue;
        }
        if (member->as.fn.type_params.count > 0) {
            jog_check_fail(ck, member->pos, "an interface method cannot have type parameters");
            continue;
        }
        if (member->as.fn.variadic) {
            jog_check_fail(ck, member->pos, "an interface method cannot be variadic");
            continue;
        }
        if (member->as.fn.body) {
            jog_check_fail(ck, member->pos, "an interface method cannot have a body");
            continue;
        }
        for (prev = 0; prev < i; prev++) {
            JogNode *earlier = list_at(node->as.aggregate.members, prev);
            if (earlier && earlier->kind == JOG_NODE_FN && earlier->as.fn.name &&
                strcmp(earlier->as.fn.name, member->as.fn.name) == 0) {
                jog_check_fail(ck, member->pos, "duplicate method '%s'", member->as.fn.name);
                break;
            }
        }
        {
            int member_bad = 0;
            fn_ty = build_req(ck, job, member, &member_bad);
            if (!fn_ty) {
                return NULL;
            }
            if (member_bad || fn_ty->kind != JOG_TY_FN) {
                continue;
            }
        }
        sym = jog_symbol_new(ck->program->arena, JOG_SYM_METHOD, member->as.fn.name, member, fn_ty, job->package);
        if (!sym) {
            jog_check_oom(ck);
            return NULL;
        }
        member->symbol = sym;
        member->type = fn_ty;
    }
    if (ck->stop) {
        return NULL;
    }
    return type;
}

static JogType *peel_iface(JogType *type, int *needs_load) {
    if (needs_load) {
        *needs_load = 0;
    }
    if (!type) {
        return NULL;
    }
    if (type->kind == JOG_TY_INTERFACE) {
        return type;
    }
    if (type->kind == JOG_TY_PTR && type->as.elem.elem && type->as.elem.elem->kind == JOG_TY_INTERFACE) {
        if (needs_load) {
            *needs_load = 1;
        }
        return type->as.elem.elem;
    }
    return NULL;
}

static int param_named(JogNode *fn, const char *name, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        JogNode *param = list_at(fn->as.fn.params, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.name && strcmp(param->as.param.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static JogNode *find_req_depth(JogType *iface, const char *name, int depth) {
    JogNode *decl;
    size_t i;
    if (!iface || !name || !iface->as.nominal.decl || depth > 16) {
        return NULL;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return NULL;
    }
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        JogNode *nested;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN && member->as.fn.name && strcmp(member->as.fn.name, name) == 0) {
            return member;
        }
        if (member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
            continue;
        }
        emb = member->as.embed.type->type;
        if (!emb || emb->kind != JOG_TY_INTERFACE || jog_constraint_is_typeset(emb)) {
            continue;
        }
        nested = find_req_depth(emb, name, depth + 1);
        if (nested) {
            return nested;
        }
    }
    return NULL;
}

static JogNode *find_req(JogType *iface, const char *name) {
    return find_req_depth(iface, name, 0);
}

static JogNode *find_prop_depth(JogType *iface, const char *name, int depth) {
    JogNode *decl;
    size_t i;
    if (!iface || !name || !iface->as.nominal.decl || depth > 16) {
        return NULL;
    }
    decl = iface->as.nominal.decl;
    if (decl->kind != JOG_NODE_INTERFACE) {
        return NULL;
    }
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        JogNode *nested;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD && member->as.field.name && strcmp(member->as.field.name, name) == 0) {
            return member;
        }
        if (member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
            continue;
        }
        emb = member->as.embed.type->type;
        if (!emb || emb->kind != JOG_TY_INTERFACE || jog_constraint_is_typeset(emb)) {
            continue;
        }
        nested = find_prop_depth(emb, name, depth + 1);
        if (nested) {
            return nested;
        }
    }
    return NULL;
}

static JogNode *find_prop(JogType *iface, const char *name) {
    return find_prop_depth(iface, name, 0);
}

int jog_check_interface_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *base_ty,
                             JogType **out) {
    JogNode *callee;
    JogType *iface;
    JogType *error;
    JogNode *req;
    JogType *fn;
    size_t count;
    size_t i;
    size_t positional = 0;
    int seen_named = 0;
    unsigned char *filled;
    JogSymbol *isym;
    if (!ck || !call || !out) {
        return 0;
    }
    *out = NULL;
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_MEMBER || callee->as.member.safe || !callee->as.member.name) {
        return 0;
    }
    iface = peel_iface(base_ty, NULL);
    if (!iface || is_universe_error(iface)) {
        return 0;
    }
    error = jog_check_error_type(ck);
    isym = iface->as.nominal.decl ? iface->as.nominal.decl->symbol : NULL;
    if (isym && (!job || job->symbol != isym)) {
        if (!jog_check_wait_finished(ck, job, isym)) {
            return -1;
        }
    }
    if (ck->stop) {
        return -1;
    }
    if (callee->as.member.type_args.count > 0) {
        jog_check_fail(ck, callee->pos, "an interface method cannot have type parameters");
        call->type = error;
        *out = error;
        return 1;
    }
    req = find_req(iface, callee->as.member.name);
    if (!req || !req->symbol || !req->type || req->type->kind != JOG_TY_FN) {
        jog_check_fail(ck, call->pos, "missing method '%s'", callee->as.member.name);
        call->type = error;
        *out = error;
        return 1;
    }
    callee->symbol = req->symbol;
    callee->type = req->type;
    call->symbol = req->symbol;
    fn = req->type;
    count = fn->as.fn.param_count;
    filled = NULL;
    if (count > 0) {
        filled = ct_arena_alloc(ck->program->arena, count, 1);
        if (!filled) {
            jog_check_oom(ck);
            return -1;
        }
        memset(filled, 0, count);
    }
    for (i = 0; i < call->as.call.args.count && !ck->stop; i++) {
        JogNode *arg = list_at(call->as.call.args, i);
        JogNode *value;
        JogType *want = NULL;
        int index;
        if (!arg || arg->kind != JOG_NODE_ARG) {
            jog_check_fail(ck, arg ? arg->pos : call->pos, "expected an argument");
            continue;
        }
        value = arg->as.arg.value;
        if (arg->as.arg.name) {
            seen_named = 1;
            index = param_named(req, arg->as.arg.name, count);
            if (index < 0) {
                jog_check_fail(ck, arg->pos, "unknown parameter '%s'", arg->as.arg.name);
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            if (filled && filled[index]) {
                jog_check_fail(ck, arg->pos, "duplicate argument '%s'", arg->as.arg.name);
                continue;
            }
            if (filled) {
                filled[index] = 1;
            }
            want = fn->as.fn.params ? fn->as.fn.params[index] : NULL;
        } else {
            if (seen_named) {
                jog_check_fail(ck, arg->pos, "positional argument after a named argument");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            if (positional >= count) {
                jog_check_fail(ck, arg->pos, "too many arguments");
                if (value) {
                    (void)jog_check_expr(ck, job, scope, value, NULL);
                }
                continue;
            }
            index = (int)positional;
            positional++;
            if (filled) {
                filled[index] = 1;
            }
            want = fn->as.fn.params ? fn->as.fn.params[index] : NULL;
        }
        if (value) {
            (void)jog_check_expr(ck, job, scope, value, want);
        }
    }
    if (call->as.call.trailing && !ck->stop) {
        JogNode *lam = call->as.call.trailing;
        JogType *want = NULL;
        JogType *inner = NULL;
        if (count == 0 || !fn->as.fn.params) {
            jog_check_fail(ck, lam->pos, "trailing lambda needs a function parameter");
            (void)jog_check_expr(ck, job, scope, lam, NULL);
        } else if (filled && filled[count - 1]) {
            jog_check_fail(ck, lam->pos, "trailing lambda does not fit");
            (void)jog_check_expr(ck, job, scope, lam, NULL);
        } else {
            want = fn->as.fn.params[count - 1];
            inner = want ? jog_type_underlying(want) : NULL;
            if (!inner || inner->kind != JOG_TY_FN) {
                jog_check_fail(ck, lam->pos, "trailing lambda needs a function parameter");
                (void)jog_check_expr(ck, job, scope, lam, NULL);
            } else {
                if (filled) {
                    filled[count - 1] = 1;
                }
                (void)jog_check_expr(ck, job, scope, lam, want);
            }
        }
    }
    for (i = 0; i < count && !ck->stop; i++) {
        JogNode *param;
        if (filled && filled[i]) {
            continue;
        }
        param = list_at(req->as.fn.params, i);
        if (param && param->kind == JOG_NODE_PARAM && param->as.param.default_value) {
            continue;
        }
        jog_check_fail(ck, call->pos, "missing argument '%s'",
                       param && param->as.param.name ? param->as.param.name : "?");
    }
    if (ck->stop) {
        return -1;
    }
    if (jog_check_context_resolve(ck, job, call) < 0) {
        return -1;
    }
    call->type = fn->as.fn.ret ? fn->as.fn.ret : error;
    *out = call->type;
    return 1;
}

static int concrete_ptr(const JogType *type) {
    const JogType *elem;
    if (!type || type->kind != JOG_TY_PTR) {
        return 0;
    }
    elem = type->as.elem.elem;
    return is_user_nominal(elem);
}

int jog_check_interface_asq(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node) {
    JogType *dest;
    JogType *got;
    JogType *opt;
    JogType *error;
    if (!ck || !node) {
        return 0;
    }
    dest = jog_check_resolve_type(ck, job, node->as.cast.type);
    if (!dest || ck->stop) {
        return -1;
    }
    got = jog_check_expr(ck, job, scope, node->as.cast.expr, NULL);
    if (!got || ck->stop) {
        return -1;
    }
    if (got->kind != JOG_TY_INTERFACE || is_universe_error(got)) {
        return 0;
    }
    error = jog_check_error_type(ck);
    if (got->kind == JOG_TY_ERROR) {
        node->type = got;
        return 1;
    }
    if (!concrete_ptr(dest)) {
        jog_check_fail(ck, node->pos, "as? expects a pointer to a struct or enum");
        node->type = error;
        return 1;
    }
    opt = jog_type_option(ck->program->types, dest);
    if (!opt) {
        jog_check_oom(ck);
        return -1;
    }
    node->type = opt;
    return 1;
}

int jog_check_interface_as(Check *ck, JogNode *node, JogType *got, JogType *dest) {
    Fit fit;
    int kind;
    if (!ck || !node || !got || !dest || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    if (dest->kind == JOG_TY_INTERFACE && !is_universe_error(dest)) {
        kind = satisfy(ck, ck->current_job, got, dest, &fit, 0);
        if (kind < 0) {
            return -1;
        }
        if (kind == FIT_OK) {
            node->type = dest;
            return 1;
        }
        if (kind == FIT_NOT) {
            return 0;
        }
        if (kind == FIT_MISSING || kind == FIT_GENERIC || kind == FIT_RECV || kind == FIT_SIG) {
            JogType *elem = got->kind == JOG_TY_PTR ? got->as.elem.elem : got;
            explain(ck, node->pos, elem, dest, fit);
        } else {
            explain(ck, node->pos, got, dest, fit);
        }
        node->type = jog_check_error_type(ck);
        return 1;
    }
    if (got->kind == JOG_TY_INTERFACE && !is_universe_error(got) && concrete_ptr(dest)) {
        node->type = dest;
        return 1;
    }
    return 0;
}

static int user_iface(const JogType *type) {
    return type && type->kind == JOG_TY_INTERFACE && !is_universe_error(type) && !jog_constraint_is_typeset(type);
}

static JogType *named_concrete(JogType *dest) {
    JogType *elem;
    if (!dest) {
        return NULL;
    }
    if ((dest->kind == JOG_TY_STRUCT || dest->kind == JOG_TY_ENUM) && dest->as.nominal.decl) {
        return dest;
    }
    if (dest->kind != JOG_TY_PTR) {
        return NULL;
    }
    elem = dest->as.elem.elem;
    if (elem && (elem->kind == JOG_TY_STRUCT || elem->kind == JOG_TY_ENUM) && elem->as.nominal.decl) {
        return elem;
    }
    return NULL;
}

static int iface_ready(Check *ck, CheckJob *job, JogType *iface) {
    JogSymbol *isym;
    if (!iface) {
        return 0;
    }
    isym = iface->as.nominal.decl ? iface->as.nominal.decl->symbol : NULL;
    if (isym && (!job || job->symbol != isym)) {
        if (!jog_check_wait_finished(ck, job, isym)) {
            return -1;
        }
    }
    return ck->stop ? -1 : 1;
}

int jog_check_interface_member(Check *ck, CheckJob *job, JogNode *member, JogType *iface, JogType **out) {
    JogType *error;
    JogNode *prop;
    JogNode *method;
    int ready;
    if (!ck || !member || !out) {
        return 0;
    }
    *out = NULL;
    if (!user_iface(iface)) {
        return 0;
    }
    ready = iface_ready(ck, job, iface);
    if (ready <= 0) {
        return ready;
    }
    error = jog_check_error_type(ck);
    prop = member->as.member.name ? find_prop(iface, member->as.member.name) : NULL;
    if (prop && prop->type && prop->type->kind != JOG_TY_ERROR) {
        member->symbol = prop->symbol;
        member->type = prop->type;
        *out = prop->type;
        return 1;
    }
    method = member->as.member.name ? find_req(iface, member->as.member.name) : NULL;
    if (method) {
        jog_check_fail(ck, member->pos, "method '%s' must be called", member->as.member.name);
        member->symbol = method->symbol;
        member->type = error;
        *out = error;
        return 1;
    }
    jog_check_fail(ck, member->pos, "unknown property '%s'", member->as.member.name ? member->as.member.name : "?");
    member->type = error;
    *out = error;
    return 1;
}

int jog_check_interface_store(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign) {
    JogNode *target;
    JogNode *decl;
    JogType *base_ty;
    JogType *iface;
    JogType *unit;
    int needs_load = 0;
    if (!ck || !assign) {
        return 0;
    }
    target = assign->as.assign.target;
    if (!target || target->kind != JOG_NODE_MEMBER || !target->as.member.base) {
        return 0;
    }
    base_ty = target->as.member.base->type;
    iface = peel_iface(base_ty, &needs_load);
    if (!user_iface(iface)) {
        return 0;
    }
    unit = jog_check_unit_type(ck);
    if (!target->symbol || target->symbol->kind != JOG_SYM_FIELD || !target->type || target->type->kind == JOG_TY_ERROR) {
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, NULL);
        }
        assign->type = unit;
        return 1;
    }
    decl = target->symbol->decl;
    if (!decl || decl->kind != JOG_NODE_FIELD) {
        return 0;
    }
    if ((needs_load && base_ty && base_ty->is_const) || !decl->as.field.is_var) {
        jog_check_fail(ck, target->pos, "cannot assign to property '%s'",
                       target->as.member.name ? target->as.member.name : "?");
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, target->type);
        }
        assign->type = unit;
        return 1;
    }
    if (assign->as.assign.op != JOG_TOK_ASSIGN) {
        jog_check_fail(ck, assign->pos, "operator '%s' is not checked yet", jog_token_spelling(assign->as.assign.op));
        return 1;
    }
    if (assign->as.assign.value) {
        (void)jog_check_expr(ck, job, scope, assign->as.assign.value, target->type);
    }
    assign->type = unit;
    return 1;
}

int jog_check_interface_is(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType **out) {
    JogNode *expr;
    JogType *got;
    JogType *dest;
    JogType *concrete;
    JogType *boolean;
    if (out) {
        *out = NULL;
    }
    if (!ck || !node || node->kind != JOG_NODE_IS) {
        return 0;
    }
    expr = node->as.is.expr;
    got = expr ? jog_check_expr(ck, job, scope, expr, NULL) : NULL;
    if (ck->stop) {
        return -1;
    }
    if (!got || got->kind == JOG_TY_ERROR || !user_iface(got)) {
        return 0;
    }
    boolean = jog_type_builtin(ck->program->types, JOG_BUILTIN_BOOL);
    if (!boolean) {
        jog_check_oom(ck);
        return -1;
    }
    dest = jog_check_resolve_type(ck, job, node->as.is.type);
    if (!dest || ck->stop) {
        return -1;
    }
    if (dest->kind == JOG_TY_ERROR) {
        node->type = dest;
        if (out) {
            *out = dest;
        }
        return 1;
    }
    concrete = named_concrete(dest);
    if (!concrete) {
        jog_check_fail(ck, node->as.is.type ? node->as.is.type->pos : node->pos, "is expects a struct or enum");
        node->type = jog_check_error_type(ck);
        if (out) {
            *out = node->type;
        }
        return 1;
    }
    if (node->as.is.type) {
        node->as.is.type->type = concrete;
    }
    node->type = boolean;
    node->symbol = NULL;
    if (expr->kind == JOG_NODE_IDENT && expr->symbol &&
        (expr->symbol->kind == JOG_SYM_CONSTANT || expr->symbol->kind == JOG_SYM_PARAMETER) && expr->symbol->type &&
        expr->symbol->type->kind == JOG_TY_INTERFACE) {
        node->symbol = expr->symbol;
    }
    if (out) {
        *out = boolean;
    }
    return 1;
}

int jog_check_interface_pat_is(Check *ck, CheckJob *job, JogNode *pat, JogType *subject) {
    JogType *dest;
    JogType *concrete;
    JogNode *type_node;
    if (!ck || !pat || pat->kind != JOG_NODE_PAT_IS || !user_iface(subject)) {
        return 0;
    }
    type_node = pat->as.pat_is.type;
    dest = jog_check_resolve_type(ck, job, type_node);
    if (!dest || ck->stop) {
        return -1;
    }
    if (dest->kind == JOG_TY_ERROR) {
        pat->type = dest;
        return 0;
    }
    concrete = named_concrete(dest);
    if (!concrete) {
        jog_check_fail(ck, type_node ? type_node->pos : pat->pos, "is expects a struct or enum");
        pat->type = jog_check_error_type(ck);
        return 0;
    }
    if (type_node) {
        type_node->type = concrete;
    }
    pat->type = subject;
    pat->symbol = NULL;
    return 1;
}

static void narrow_push(JogIfaceNarrow *st, JogSymbol *sym, JogType *to) {
    size_t i;
    if (!st || !sym || !to) {
        return;
    }
    for (i = 0; i < st->count; i++) {
        if (st->syms[i] != sym) {
            continue;
        }
        if (sym->type != to) {
            sym->type = st->saved[i];
            st->syms[i] = NULL;
        }
        return;
    }
    if (st->count >= JOG_IFACE_NARROW) {
        return;
    }
    if (!sym->type || sym->type->kind != JOG_TY_INTERFACE) {
        return;
    }
    st->syms[st->count] = sym;
    st->saved[st->count] = sym->type;
    sym->type = to;
    st->count++;
}

static void narrow_one(Check *ck, JogIfaceNarrow *st, JogSymbol *sym, JogType *concrete) {
    JogType *ptr;
    if (!ck || !sym || !concrete) {
        return;
    }
    if (sym->kind != JOG_SYM_CONSTANT && sym->kind != JOG_SYM_PARAMETER) {
        return;
    }
    ptr = jog_type_ptr(ck->program->types, concrete, false);
    if (!ptr) {
        jog_check_oom(ck);
        return;
    }
    narrow_push(st, sym, ptr);
}

static void narrow_walk(Check *ck, JogNode *node, JogIfaceNarrow *st) {
    JogType *concrete;
    if (!ck || !node || ck->stop || !st) {
        return;
    }
    if (node->kind == JOG_NODE_BINARY && node->as.binary.op == JOG_TOK_AMPAMP) {
        narrow_walk(ck, node->as.binary.lhs, st);
        narrow_walk(ck, node->as.binary.rhs, st);
        return;
    }
    if (node->kind != JOG_NODE_IS || !node->symbol) {
        return;
    }
    concrete = node->as.is.type ? node->as.is.type->type : NULL;
    if (!concrete || (concrete->kind != JOG_TY_STRUCT && concrete->kind != JOG_TY_ENUM)) {
        return;
    }
    narrow_one(ck, st, node->symbol, concrete);
}

void jog_iface_narrow_enter(Check *ck, JogNode *cond, JogIfaceNarrow *st) {
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
    narrow_walk(ck, cond, st);
}

void jog_iface_narrow_pat(Check *ck, JogNode *subject, JogNode *pat, JogIfaceNarrow *st) {
    JogType *concrete;
    if (!st) {
        return;
    }
    memset(st, 0, sizeof(*st));
    if (!ck || !subject || !pat || pat->kind != JOG_NODE_PAT_IS || subject->kind != JOG_NODE_IDENT) {
        return;
    }
    concrete = pat->as.pat_is.type ? pat->as.pat_is.type->type : NULL;
    if (!concrete || (concrete->kind != JOG_TY_STRUCT && concrete->kind != JOG_TY_ENUM)) {
        return;
    }
    narrow_one(ck, st, subject->symbol, concrete);
}

void jog_iface_narrow_leave(JogIfaceNarrow *st) {
    size_t i;
    if (!st) {
        return;
    }
    for (i = 0; i < st->count; i++) {
        if (st->syms[i]) {
            st->syms[i]->type = st->saved[i];
        }
    }
    st->count = 0;
}
