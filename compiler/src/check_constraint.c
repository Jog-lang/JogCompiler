#include "check_constraint.h"

#include "check_context.h"
#include "check_generic.h"
#include "check_struct.h"

#include <stdio.h>
#include <string.h>

/* A constraint is an interface. Type-set members contribute the builtin
   operations every alternative shares. Method and property requirements
   contribute those operations directly, specialized after monomorphization.
   In a method requirement, the constraint interface stands for the type
   argument: fn op<=>(o: Ordered) is satisfied by fn T.op<=>(o: T).
   ~T is the underlying type with every distinct layer removed, matching
   T and every distinct type built on that underlying type.
   An open type argument satisfies a constraint only when it is a type
   parameter whose own constraint entails the required one. */

enum {
    OP_ADD = 1 << 0, /* + - * */
    OP_DIV = 1 << 1, /* / */
    OP_REM = 1 << 2, /* % */
    OP_BIT = 1 << 3, /* & | ^ << >> and unary ~ */
    OP_NEG = 1 << 4, /* unary - */
    OP_ORD = 1 << 5, /* < <= > >= from op<=>, or integer order */
    OP_EQ = 1 << 6,  /* == != */
    OP_NOT = 1 << 7, /* unary ! */
    OP_INT = OP_ADD | OP_DIV | OP_REM | OP_BIT | OP_NEG | OP_ORD | OP_EQ,
    OP_NUM = OP_ADD | OP_DIV | OP_NEG | OP_ORD | OP_EQ,
    OP_BOOL = OP_EQ | OP_NOT,
    OP_STR = OP_EQ
};

enum { WALK_MAX = 24, TERM_MAX = 32 };

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
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

static const char *iface_name(const JogType *type) {
    if (type && type->as.nominal.name && type->as.nominal.name[0] != '\0') {
        return type->as.nominal.name;
    }
    return "?";
}

static int is_iface(const JogType *type) {
    return type && type->kind == JOG_TY_INTERFACE && type->as.nominal.decl &&
           type->as.nominal.decl->kind == JOG_NODE_INTERFACE;
}

static int is_float_type(const JogType *type) {
    JogType *base = jog_type_underlying((JogType *)type);
    return base && (base->kind == JOG_TY_FLOAT || base->kind == JOG_TY_UNTYPED_FLOAT);
}

static int is_str_type(const Check *ck, const JogType *type) {
    JogType *str;
    if (!ck || !ck->program || !ck->program->types || !type) {
        return 0;
    }
    str = jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES);
    return str && type == str;
}

static JogType *peel_ptr(JogType *type) {
    if (type && type->kind == JOG_TY_PTR && type->as.elem.elem) {
        return type->as.elem.elem;
    }
    return type;
}

static int same_pkg(const JogType *type, const JogSymbol *sym) {
    const char *want = type && type->as.nominal.package ? type->as.nominal.package : "";
    const char *have = sym && sym->package && sym->package->path ? sym->package->path : "";
    return strcmp(want, have) == 0;
}

static JogType *term_type(JogNode *term) {
    if (!term) {
        return NULL;
    }
    if (term->kind == JOG_NODE_TYPE_NAME || term->type) {
        return term->type;
    }
    return term->type;
}

static JogType *embed_type(JogNode *member) {
    if (!member || member->kind != JOG_NODE_EMBED || !member->as.embed.type) {
        return NULL;
    }
    return member->as.embed.type->type;
}

static int typeset_rec(const JogType *type, int depth);

bool jog_constraint_is_typeset(const JogType *type) {
    return typeset_rec(type, 0);
}

static int typeset_rec(const JogType *type, int depth) {
    JogNode *decl;
    size_t i;
    if (!is_iface(type) || depth > WALK_MAX) {
        return 0;
    }
    decl = type->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_TYPE_SET) {
            return 1;
        }
        if (member->kind != JOG_NODE_EMBED) {
            continue;
        }
        emb = embed_type(member);
        if (!emb) {
            continue;
        }
        if (!is_iface(emb)) {
            return 1;
        }
        if (typeset_rec(emb, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

static JogType *replace_type(JogTypes *types, JogType *type, JogType *from, JogType *to) {
    size_t i;
    int changed;
    if (!type || type == from) {
        return type == from ? to : type;
    }
    if (!types || !from || !to) {
        return type;
    }
    switch (type->kind) {
    case JOG_TY_PTR:
    case JOG_TY_SLICE:
    case JOG_TY_OPTION: {
        JogType *elem = replace_type(types, type->as.elem.elem, from, to);
        if (elem == type->as.elem.elem) {
            return type;
        }
        if (type->kind == JOG_TY_PTR) {
            return jog_type_ptr(types, elem, type->is_const);
        }
        if (type->kind == JOG_TY_SLICE) {
            return jog_type_slice(types, elem, type->is_const);
        }
        return jog_type_option(types, elem);
    }
    case JOG_TY_ARRAY: {
        JogType *elem = replace_type(types, type->as.array.elem, from, to);
        if (elem == type->as.array.elem) {
            return type;
        }
        return jog_type_array(types, elem, type->as.array.len);
    }
    case JOG_TY_TUPLE: {
        JogType *buf[16];
        if (!type->as.tuple.elems || type->as.tuple.count == 0) {
            return type;
        }
        if (type->as.tuple.count > 16) {
            return type;
        }
        changed = 0;
        for (i = 0; i < type->as.tuple.count; i++) {
            buf[i] = replace_type(types, type->as.tuple.elems[i], from, to);
            if (buf[i] != type->as.tuple.elems[i]) {
                changed = 1;
            }
        }
        if (!changed) {
            return type;
        }
        return jog_type_tuple(types, buf, type->as.tuple.count);
    }
    case JOG_TY_FN: {
        JogType *params_buf[16];
        JogType *ctx_buf[8];
        JogType **params = params_buf;
        JogType **ctxs = ctx_buf;
        JogType *ret;
        size_t n = type->as.fn.param_count;
        size_t nc = type->as.fn.context_count;
        if (n > 16 || nc > 8) {
            return type;
        }
        changed = 0;
        for (i = 0; i < n; i++) {
            params[i] = replace_type(types, type->as.fn.params ? type->as.fn.params[i] : NULL, from, to);
            if (!type->as.fn.params || params[i] != type->as.fn.params[i]) {
                changed = 1;
            }
        }
        for (i = 0; i < nc; i++) {
            ctxs[i] = replace_type(types, type->as.fn.contexts ? type->as.fn.contexts[i] : NULL, from, to);
            if (!type->as.fn.contexts || ctxs[i] != type->as.fn.contexts[i]) {
                changed = 1;
            }
        }
        ret = replace_type(types, type->as.fn.ret, from, to);
        if (ret != type->as.fn.ret) {
            changed = 1;
        }
        if (!changed) {
            return type;
        }
        return jog_type_fn(types, n ? params : NULL, n, ret, nc ? ctxs : NULL, nc, type->as.fn.variadic,
                           type->as.fn.escaping, type->as.fn.is_extern);
    }
    default:
        return type;
    }
}

/* cand_family is 1 when cand stands for ~underlying(cand), not one type. */
static int in_typeset(const JogType *cand, const JogType *iface, int cand_family, int depth);

static int match_one(const JogType *cand, const JogType *term, int term_approx, int cand_family) {
    JogType *base_c;
    JogType *base_t;
    if (!cand || !term || term->kind == JOG_TY_ERROR) {
        return 0;
    }
    base_c = jog_type_underlying((JogType *)cand);
    base_t = jog_type_underlying((JogType *)term);
    if (cand_family) {
        return term_approx && base_c && base_c == base_t;
    }
    if (term_approx) {
        return base_c && base_c == base_t;
    }
    return cand == term;
}

static int match_union(const JogType *cand, JogNode *set, int cand_family, int depth) {
    size_t i;
    if (!set || set->kind != JOG_NODE_TYPE_SET) {
        return 0;
    }
    for (i = 0; i < set->as.type_set.terms.count; i++) {
        JogNode *term_node = list_at(set->as.type_set.terms, i);
        JogType *term = term_type(term_node);
        int approx = set->as.type_set.approximate && i < set->as.type_set.terms.count && set->as.type_set.approximate[i];
        if (!term) {
            continue;
        }
        if (is_iface(term) && typeset_rec(term, depth + 1)) {
            if (in_typeset(cand, term, cand_family, depth + 1)) {
                return 1;
            }
            continue;
        }
        if (match_one(cand, term, approx, cand_family)) {
            return 1;
        }
    }
    return 0;
}

static int in_typeset(const JogType *cand, const JogType *iface, int cand_family, int depth) {
    JogNode *decl;
    size_t i;
    int any = 0;
    if (!is_iface(iface) || depth > WALK_MAX || !cand) {
        return 0;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        int ok;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_TYPE_SET) {
            any = 1;
            ok = match_union(cand, member, cand_family, depth);
            if (!ok) {
                return 0;
            }
            continue;
        }
        if (member->kind != JOG_NODE_EMBED) {
            continue;
        }
        emb = embed_type(member);
        if (!emb) {
            continue;
        }
        if (is_iface(emb)) {
            if (!typeset_rec(emb, depth + 1)) {
                continue;
            }
            any = 1;
            if (!in_typeset(cand, emb, cand_family, depth + 1)) {
                return 0;
            }
            continue;
        }
        any = 1;
        if (!match_one(cand, emb, 0, cand_family)) {
            return 0;
        }
    }
    return any ? 1 : 1;
}

static int iface_has_set(const JogType *iface) {
    return typeset_rec(iface, 0);
}

static int ops_of_type(const Check *ck, const JogType *type, int depth);

static int ops_of_union(const Check *ck, JogNode *set, int depth) {
    size_t i;
    int ops = ~0;
    int any = 0;
    if (!set || set->kind != JOG_NODE_TYPE_SET) {
        return 0;
    }
    for (i = 0; i < set->as.type_set.terms.count; i++) {
        JogType *term = term_type(list_at(set->as.type_set.terms, i));
        int one;
        if (!term) {
            continue;
        }
        any = 1;
        one = ops_of_type(ck, term, depth + 1);
        ops &= one;
    }
    return any ? ops : 0;
}

static int ops_of_type(const Check *ck, const JogType *type, int depth) {
    JogType *base;
    JogNode *decl;
    size_t i;
    int ops;
    int any;
    if (!type || depth > WALK_MAX) {
        return 0;
    }
    if (is_iface(type) && iface_has_set(type)) {
        decl = type->as.nominal.decl;
        ops = ~0;
        any = 0;
        for (i = 0; i < decl->as.aggregate.members.count; i++) {
            JogNode *member = list_at(decl->as.aggregate.members, i);
            JogType *emb;
            int one;
            if (!member) {
                continue;
            }
            if (member->kind == JOG_NODE_TYPE_SET) {
                any = 1;
                ops &= ops_of_union(ck, member, depth);
                continue;
            }
            if (member->kind != JOG_NODE_EMBED) {
                continue;
            }
            emb = embed_type(member);
            if (!emb || (is_iface(emb) && !iface_has_set(emb))) {
                continue;
            }
            any = 1;
            one = ops_of_type(ck, emb, depth + 1);
            ops &= one;
        }
        return any ? ops : 0;
    }
    base = jog_type_underlying((JogType *)type);
    if (!base) {
        return 0;
    }
    if (base->kind == JOG_TY_INT) {
        return OP_INT;
    }
    if (base->kind == JOG_TY_FLOAT) {
        return OP_NUM;
    }
    if (base->kind == JOG_TY_BOOL) {
        return OP_BOOL;
    }
    if (is_str_type(ck, base) || is_str_type(ck, type)) {
        return OP_STR;
    }
    return 0;
}

static int method_op_bits(const char *name, size_t params) {
    if (!name) {
        return 0;
    }
    if (strcmp(name, "op+") == 0 || strcmp(name, "op*") == 0) {
        return OP_ADD;
    }
    if (strcmp(name, "op-") == 0) {
        return params == 0 ? OP_NEG : OP_ADD;
    }
    if (strcmp(name, "op/") == 0) {
        return OP_DIV;
    }
    if (strcmp(name, "op%") == 0) {
        return OP_REM;
    }
    if (strcmp(name, "op&") == 0 || strcmp(name, "op|") == 0 || strcmp(name, "op^") == 0 ||
        strcmp(name, "op<<") == 0 || strcmp(name, "op>>") == 0) {
        return OP_BIT;
    }
    if (strcmp(name, "op~") == 0) {
        return OP_BIT;
    }
    if (strcmp(name, "op<=>") == 0 || strcmp(name, "op<") == 0 || strcmp(name, "op<=") == 0 ||
        strcmp(name, "op>") == 0 || strcmp(name, "op>=") == 0) {
        return OP_ORD;
    }
    if (strcmp(name, "op==") == 0 || strcmp(name, "op!=") == 0) {
        return OP_EQ;
    }
    if (strcmp(name, "op!") == 0) {
        return OP_NOT;
    }
    return 0;
}

static int requirement_ops(const JogType *iface, int depth) {
    JogNode *decl;
    size_t i;
    int ops = 0;
    if (!is_iface(iface) || depth > WALK_MAX) {
        return 0;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN && member->as.fn.name && member->type && member->type->kind == JOG_TY_FN) {
            ops |= method_op_bits(member->as.fn.name, member->type->as.fn.param_count);
            continue;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            if (is_iface(emb)) {
                ops |= requirement_ops(emb, depth + 1);
            }
        }
    }
    return ops;
}

static int param_ops(const Check *ck, const JogType *param) {
    const JogType *constraint;
    int ops;
    if (!param || param->kind != JOG_TY_TYPE_PARAM) {
        return 0;
    }
    constraint = param->as.param.constraint;
    if (!is_iface(constraint)) {
        return 0;
    }
    ops = iface_has_set(constraint) ? ops_of_type(ck, constraint, 0) : 0;
    ops |= requirement_ops(constraint, 0);
    return ops;
}

static JogNode *find_method_req(const JogType *iface, const char *name, int depth) {
    JogNode *decl;
    size_t i;
    if (!is_iface(iface) || !name || depth > WALK_MAX) {
        return NULL;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogNode *nested;
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN && member->as.fn.name && strcmp(member->as.fn.name, name) == 0) {
            return member;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            nested = find_method_req(emb, name, depth + 1);
            if (nested) {
                return nested;
            }
        }
    }
    return NULL;
}

static JogNode *find_prop_req(const JogType *iface, const char *name, int depth) {
    JogNode *decl;
    size_t i;
    if (!is_iface(iface) || !name || depth > WALK_MAX) {
        return NULL;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogNode *nested;
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD && member->as.field.name && strcmp(member->as.field.name, name) == 0) {
            return member;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            nested = find_prop_req(emb, name, depth + 1);
            if (nested) {
                return nested;
            }
        }
    }
    return NULL;
}

static int sig_match(JogTypes *types, const JogType *method, const JogType *req, JogType *iface, JogType *cand) {
    size_t i;
    JogType *want_ret;
    if (!method || !req || method->kind != JOG_TY_FN || req->kind != JOG_TY_FN || !types) {
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
    want_ret = replace_type(types, req->as.fn.ret, iface, cand);
    if (!want_ret || method->as.fn.ret != want_ret) {
        return 0;
    }
    for (i = 0; i < req->as.fn.param_count; i++) {
        JogType *want = replace_type(types, req->as.fn.params ? req->as.fn.params[i] : NULL, iface, cand);
        JogType *got = method->as.fn.params ? method->as.fn.params[i + 1] : NULL;
        if (!want || got != want) {
            return 0;
        }
    }
    for (i = 0; i < req->as.fn.context_count; i++) {
        JogType *want = req->as.fn.contexts ? req->as.fn.contexts[i] : NULL;
        JogType *got = method->as.fn.contexts ? method->as.fn.contexts[i] : NULL;
        if (got != want) {
            return 0;
        }
    }
    return 1;
}

static int intrinsic_ok(const Check *ck, const JogType *cand, JogNode *req) {
    int ops;
    JogType *base;
    if (!req || !req->as.fn.name || !cand) {
        return 0;
    }
    base = jog_type_underlying((JogType *)cand);
    if (!base) {
        return 0;
    }
    if (base->kind == JOG_TY_INT) {
        ops = OP_INT;
    } else if (base->kind == JOG_TY_BOOL) {
        ops = OP_BOOL;
    } else if (is_str_type(ck, base) || is_str_type(ck, cand)) {
        ops = OP_STR;
    } else {
        return 0;
    }
    if (!req->type || req->type->kind != JOG_TY_FN) {
        return 0;
    }
    return (method_op_bits(req->as.fn.name, req->type->as.fn.param_count) & ops) != 0;
}

static int method_satisfies(Check *ck, CheckJob *job, JogType *cand, JogType *iface, JogNode *req) {
    const char *name;
    size_t p;
    int saw_generic = 0;
    int saw_sig = 0;
    int saw_recv = 0;
    int full = 0;
    if (!ck || !cand || !req || !req->as.fn.name || !req->type || req->type->kind != JOG_TY_FN) {
        return 0;
    }
    name = req->as.fn.name;
    if (cand->kind != JOG_TY_STRUCT && cand->kind != JOG_TY_ENUM && cand->kind != JOG_TY_DISTINCT) {
        return intrinsic_ok(ck, cand, req);
    }
    for (p = 0; p < ck->program->package_count && !ck->stop; p++) {
        JogPackage *pkg = ck->program->packages ? ck->program->packages[p] : NULL;
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
                uint32_t path[JOG_STRUCT_PATH];
                uint8_t path_len = 0;
                if (!fn || fn->kind != JOG_NODE_FN || !fn->as.fn.receiver || !fn->as.fn.name) {
                    continue;
                }
                if (strcmp(fn->as.fn.name, name) != 0) {
                    continue;
                }
                sym = fn->symbol;
                if (!sym || !same_pkg(cand, sym)) {
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
                if (!on || !jog_struct_embed_path(cand, on, path, &path_len)) {
                    continue;
                }
                if (sym->type_param_count > 0) {
                    saw_generic = 1;
                    continue;
                }
                if ((is_ptr ? 1 : 0) != (req->as.fn.receiver_is_ptr ? 1 : 0)) {
                    saw_recv = 1;
                    continue;
                }
                if (!sig_match(ck->program->types, sym->type, req->type, iface, cand)) {
                    saw_sig = 1;
                    continue;
                }
                full++;
            }
        }
    }
    if (ck->stop) {
        return -1;
    }
    if (full == 1) {
        return 1;
    }
    if (full > 1) {
        return 0;
    }
    if (intrinsic_ok(ck, cand, req)) {
        return 1;
    }
    (void)saw_generic;
    (void)saw_sig;
    (void)saw_recv;
    (void)name;
    (void)iface;
    return 0;
}

static int each_method(Check *ck, CheckJob *job, JogType *cand, JogType *iface, int depth) {
    JogNode *decl;
    size_t i;
    if (!is_iface(iface) || depth > WALK_MAX) {
        return 1;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count && !ck->stop; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        int fit;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN) {
            fit = method_satisfies(ck, job, cand, iface, member);
            if (fit <= 0) {
                return fit;
            }
            continue;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            if (is_iface(emb)) {
                fit = each_method(ck, job, cand, emb, depth + 1);
                if (fit <= 0) {
                    return fit;
                }
            }
        }
    }
    return ck->stop ? -1 : 1;
}

static int prop_satisfies(Check *ck, CheckJob *job, JogType *cand, JogType *iface, JogNode *prop) {
    JogStructPath hit;
    JogType *want;
    const char *file;
    if (!prop || !cand || cand->kind != JOG_TY_STRUCT) {
        return 0;
    }
    want = prop->type;
    if (iface && want) {
        want = replace_type(ck->program->types, want, iface, cand);
    }
    file = job && job->decl ? job->decl->pos.file : NULL;
    memset(&hit, 0, sizeof(hit));
    jog_struct_lookup_field(cand, prop->as.field.name, job ? job->decl : NULL, job ? job->package : NULL, file, &hit);
    if (!hit.found || hit.ambiguous || hit.hidden || !hit.field || !want || hit.field->type != want) {
        return 0;
    }
    return 1;
}

static int each_prop(Check *ck, CheckJob *job, JogType *cand, JogType *iface, int depth) {
    JogNode *decl;
    size_t i;
    if (!is_iface(iface) || depth > WALK_MAX) {
        return 1;
    }
    decl = iface->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count && !ck->stop; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FIELD) {
            if (!prop_satisfies(ck, job, cand, iface, member)) {
                return 0;
            }
            continue;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            if (is_iface(emb)) {
                int fit = each_prop(ck, job, cand, emb, depth + 1);
                if (fit <= 0) {
                    return fit;
                }
            }
        }
    }
    return ck->stop ? -1 : 1;
}

static int satisfies_sample(Check *ck, CheckJob *job, JogType *sample, int family, JogType *req, int report);

/* have entails need when every requirement of need is a requirement of have.
   A var property is not met by val. Depth stops an embed cycle. */
static int covers(Check *ck, JogType *have, JogType *need, int depth) {
    JogNode *decl;
    size_t i;
    if (!ck || !is_iface(have) || !is_iface(need) || depth > WALK_MAX) {
        return 0;
    }
    decl = need->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        JogType *emb;
        if (!member) {
            continue;
        }
        if (member->kind == JOG_NODE_FN && member->as.fn.name && member->type && member->type->kind == JOG_TY_FN) {
            JogNode *got = find_method_req(have, member->as.fn.name, 0);
            JogType *rewritten;
            if (!got || !got->type || (got->as.fn.receiver_is_ptr ? 1 : 0) != (member->as.fn.receiver_is_ptr ? 1 : 0)) {
                return 0;
            }
            rewritten = replace_type(ck->program->types, got->type, have, need);
            if (!rewritten || rewritten != member->type) {
                return 0;
            }
            continue;
        }
        if (member->kind == JOG_NODE_FIELD && member->as.field.name && member->type) {
            JogNode *got = find_prop_req(have, member->as.field.name, 0);
            JogType *rewritten;
            if (!got || !got->type) {
                return 0;
            }
            if (member->as.field.is_var && !got->as.field.is_var) {
                return 0;
            }
            rewritten = replace_type(ck->program->types, got->type, have, need);
            if (!rewritten || rewritten != member->type) {
                return 0;
            }
            continue;
        }
        if (member->kind == JOG_NODE_EMBED) {
            emb = embed_type(member);
            if (is_iface(emb) && !iface_has_set(emb) && !covers(ck, have, emb, depth + 1)) {
                return 0;
            }
        }
    }
    return 1;
}

static int entails(Check *ck, CheckJob *job, JogType *have, JogType *need) {
    JogNode *decl;
    size_t i;
    if (!is_iface(have) || !is_iface(need)) {
        return 0;
    }
    if (have == need) {
        return 1;
    }
    if (!iface_has_set(have)) {
        if (iface_has_set(need)) {
            return 0;
        }
        /* An open interface is the set of types that meet its requirements.
           It entails another open interface when it lists each of that
           interface's methods and properties. The constraint name in a
           signature stands for the type argument, so it is rewritten to the
           needed interface before the types are compared. */
        return covers(ck, have, need, 0);
    }
    decl = have->as.nominal.decl;
    for (i = 0; i < decl->as.aggregate.members.count; i++) {
        JogNode *member = list_at(decl->as.aggregate.members, i);
        if (!member || member->kind != JOG_NODE_TYPE_SET) {
            JogType *emb;
            int fit;
            if (!member || member->kind != JOG_NODE_EMBED) {
                continue;
            }
            emb = embed_type(member);
            if (is_iface(emb) && iface_has_set(emb)) {
                fit = entails(ck, job, emb, need);
                if (fit <= 0) {
                    return fit;
                }
            } else if (emb && !is_iface(emb)) {
                fit = satisfies_sample(ck, job, emb, 0, need, 0);
                if (fit <= 0) {
                    return fit;
                }
            }
            continue;
        }
        {
            size_t t;
            for (t = 0; t < member->as.type_set.terms.count; t++) {
                JogNode *term_node = list_at(member->as.type_set.terms, t);
                JogType *term = term_type(term_node);
                int approx = member->as.type_set.approximate && member->as.type_set.approximate[t];
                int fit;
                if (!term) {
                    return 0;
                }
                if (is_iface(term) && iface_has_set(term)) {
                    fit = entails(ck, job, term, need);
                } else {
                    fit = satisfies_sample(ck, job, term, approx, need, 0);
                }
                if (fit <= 0) {
                    return fit;
                }
            }
        }
    }
    return 1;
}

static int satisfies_sample(Check *ck, CheckJob *job, JogType *sample, int family, JogType *req, int report) {
    int methods;
    int props;
    char a[160];
    char b[160];
    if (!sample || !is_iface(req) || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    if (sample->kind == JOG_TY_TYPE_PARAM) {
        if (!is_iface(sample->as.param.constraint)) {
            return 0;
        }
        return entails(ck, job, sample->as.param.constraint, req);
    }
    if (sample->kind == JOG_TY_ERROR) {
        return 1;
    }
    if (iface_has_set(req) && !in_typeset(sample, req, family, 0)) {
        return 0;
    }
    if (is_float_type(sample)) {
        return report ? 2 : 0;
    }
    methods = each_method(ck, job, sample, req, 0);
    if (methods <= 0) {
        return methods;
    }
    props = each_prop(ck, job, sample, req, 0);
    if (props <= 0) {
        return props;
    }
    (void)a;
    (void)b;
    return 1;
}

static int satisfies(Check *ck, CheckJob *job, JogType *cand, JogType *req, JogPos pos, int report) {
    int ok;
    char a[160];
    char b[160];
    if (!cand || cand->kind == JOG_TY_ERROR) {
        return cand && cand->kind == JOG_TY_ERROR ? 1 : 0;
    }
    if (cand->kind == JOG_TY_TYPE_PARAM || jog_generic_has_param(cand)) {
        if (cand->kind != JOG_TY_TYPE_PARAM) {
            if (report) {
                type_text(cand, a, sizeof(a));
                type_text(req, b, sizeof(b));
                jog_check_fail(ck, pos, "type %s does not satisfy %s", a, b);
            }
            return 0;
        }
    }
    ok = satisfies_sample(ck, job, cand, 0, req, report);
    if (ok < 0 || ck->stop) {
        return -1;
    }
    if (ok == 2) {
        if (report) {
            jog_check_fail(ck, pos, "float values are not checked yet");
        }
        return 0;
    }
    if (!ok && report) {
        type_text(cand, a, sizeof(a));
        type_text(req, b, sizeof(b));
        jog_check_fail(ck, pos, "type %s does not satisfy %s", a, b);
    }
    return ok;
}

bool jog_constraint_bind(Check *ck, CheckJob *job, JogNode *param, JogType **constraint, int *bad) {
    JogType *type;
    int saved;
    if (constraint) {
        *constraint = NULL;
    }
    if (!ck || !param || ck->stop) {
        return !ck || !ck->stop;
    }
    if (!param->as.type_param.constraint) {
        return !ck->stop;
    }
    saved = ck->allow_typeset;
    ck->allow_typeset = 1;
    type = jog_check_resolve_type(ck, job, param->as.type_param.constraint);
    ck->allow_typeset = saved;
    if (!type || ck->stop) {
        return !ck->stop;
    }
    if (type->kind == JOG_TY_ERROR) {
        if (bad) {
            *bad = 1;
        }
        return !ck->stop;
    }
    if (!is_iface(type)) {
        jog_check_fail(ck, param->pos, "a constraint must be an interface");
        if (bad) {
            *bad = 1;
        }
        return !ck->stop;
    }
    if (constraint) {
        *constraint = type;
    }
    return !ck->stop;
}

JogType *jog_constraint_check_args(Check *ck, CheckJob *job, const JogSymbol *sym, JogType *const *args, size_t count,
                                  JogPos pos) {
    size_t i;
    if (!ck || ck->stop) {
        return NULL;
    }
    if (!sym || !sym->type_params || count == 0) {
        return NULL;
    }
    for (i = 0; i < count && i < sym->type_param_count && !ck->stop; i++) {
        JogType *param = sym->type_params[i];
        JogType *arg = args ? args[i] : NULL;
        JogType *need;
        int ok;
        if (!param || param->kind != JOG_TY_TYPE_PARAM) {
            continue;
        }
        need = param->as.param.constraint;
        if (!need) {
            continue;
        }
        if (!arg) {
            jog_check_fail(ck, pos, "type %s does not satisfy %s", "?", iface_name(need));
            return jog_check_error_type(ck);
        }
        if (arg->kind == JOG_TY_ERROR) {
            return arg;
        }
        ok = satisfies(ck, job, arg, need, pos, 1);
        if (ok < 0 || ck->stop) {
            return NULL;
        }
        if (!ok) {
            return jog_check_error_type(ck);
        }
    }
    return NULL;
}

static JogType *resolve_allowing(Check *ck, CheckJob *job, JogNode *node) {
    JogType *type;
    int saved = ck->allow_typeset;
    ck->allow_typeset = 1;
    type = jog_check_resolve_type(ck, job, node);
    ck->allow_typeset = saved;
    return type;
}

bool jog_constraint_iface_member(Check *ck, CheckJob *job, JogNode *iface, JogNode *member) {
    size_t i;
    if (!ck || !member || ck->stop) {
        return !ck || !ck->stop;
    }
    if (member->kind == JOG_NODE_FIELD) {
        JogType *type;
        JogSymbol *sym;
        if (!member->as.field.name || member->as.field.name[0] == '\0') {
            jog_check_fail(ck, member->pos, "missing property name");
            return !ck->stop;
        }
        if (member->as.field.default_value) {
            jog_check_fail(ck, member->pos, "an interface property cannot have a default");
        }
        type = jog_check_resolve_type(ck, job, member->as.field.type);
        if (!type || ck->stop) {
            return !ck->stop;
        }
        member->type = type;
        if (iface) {
            for (i = 0; i < iface->as.aggregate.members.count; i++) {
                JogNode *earlier = list_at(iface->as.aggregate.members, i);
                if (earlier == member) {
                    break;
                }
                if (earlier && earlier->kind == JOG_NODE_FIELD && earlier->as.field.name &&
                    strcmp(earlier->as.field.name, member->as.field.name) == 0) {
                    jog_check_fail(ck, member->pos, "duplicate property '%s'", member->as.field.name);
                    break;
                }
                if (earlier && earlier->kind == JOG_NODE_FN && earlier->as.fn.name &&
                    strcmp(earlier->as.fn.name, member->as.field.name) == 0) {
                    jog_check_fail(ck, member->pos, "duplicate property '%s'", member->as.field.name);
                    break;
                }
            }
        }
        sym = jog_symbol_new(ck->program->arena, JOG_SYM_FIELD, member->as.field.name, member, type,
                             job ? job->package : NULL);
        if (!sym) {
            jog_check_oom(ck);
            return false;
        }
        member->symbol = sym;
        return !ck->stop;
    }
    if (member->kind == JOG_NODE_EMBED) {
        JogType *type = member->as.embed.type ? resolve_allowing(ck, job, member->as.embed.type) : NULL;
        if (!type || ck->stop) {
            return !ck->stop;
        }
        member->type = type;
        return !ck->stop;
    }
    if (member->kind == JOG_NODE_TYPE_SET) {
        for (i = 0; i < member->as.type_set.terms.count && !ck->stop; i++) {
            JogNode *term = list_at(member->as.type_set.terms, i);
            JogType *type;
            if (!term) {
                jog_check_fail(ck, member->pos, "expected a type");
                continue;
            }
            type = resolve_allowing(ck, job, term);
            if (!type) {
                return !ck->stop;
            }
            if (type->kind == JOG_TY_TYPE_PARAM) {
                jog_check_fail(ck, term->pos, "a type set cannot name a type parameter");
            }
        }
        return !ck->stop;
    }
    jog_check_fail(ck, member->pos, "interface member is not checked yet");
    return !ck->stop;
}

static JogType *bool_type(Check *ck) {
    JogType *type = jog_type_builtin(ck->program->types, JOG_BUILTIN_BOOL);
    if (!type) {
        jog_check_oom(ck);
    }
    return type;
}

static int bin_bit(JogTokenKind op) {
    switch (op) {
    case JOG_TOK_PLUS:
    case JOG_TOK_MINUS:
    case JOG_TOK_STAR:
        return OP_ADD;
    case JOG_TOK_SLASH:
        return OP_DIV;
    case JOG_TOK_PERCENT:
        return OP_REM;
    case JOG_TOK_AMP:
    case JOG_TOK_PIPE:
    case JOG_TOK_CARET:
    case JOG_TOK_SHL:
    case JOG_TOK_SHR:
        return OP_BIT;
    case JOG_TOK_LT:
    case JOG_TOK_LE:
    case JOG_TOK_GT:
    case JOG_TOK_GE:
        return OP_ORD;
    case JOG_TOK_EQ:
    case JOG_TOK_NEQ:
        return OP_EQ;
    default:
        return 0;
    }
}

int jog_constraint_binary(Check *ck, JogNode *node, JogType *left, JogType *right, JogType **out) {
    int bit;
    int ops;
    int rel;
    JogType *result;
    if (!ck || !node || !out) {
        return 0;
    }
    *out = NULL;
    if (!left || !right || left->kind != JOG_TY_TYPE_PARAM || left != right) {
        return 0;
    }
    bit = bin_bit(node->as.binary.op);
    if (!bit) {
        return 0;
    }
    ops = param_ops(ck, left);
    if ((ops & bit) == 0) {
        return 0;
    }
    rel = bit == OP_ORD || bit == OP_EQ;
    result = rel ? bool_type(ck) : left;
    if (!result) {
        return -1;
    }
    node->type = result;
    *out = result;
    return 1;
}

int jog_constraint_unary(Check *ck, JogNode *node, JogType *operand, JogType **out) {
    int bit = 0;
    int ops;
    JogType *result;
    if (!ck || !node || !out) {
        return 0;
    }
    *out = NULL;
    if (!operand || operand->kind != JOG_TY_TYPE_PARAM) {
        return 0;
    }
    if (node->as.unary.op == JOG_TOK_MINUS) {
        bit = OP_NEG;
    } else if (node->as.unary.op == JOG_TOK_TILDE) {
        bit = OP_BIT;
    } else if (node->as.unary.op == JOG_TOK_BANG) {
        bit = OP_NOT;
    } else {
        return 0;
    }
    ops = param_ops(ck, operand);
    if ((ops & bit) == 0) {
        return 0;
    }
    result = bit == OP_NOT ? bool_type(ck) : operand;
    if (!result) {
        return -1;
    }
    node->type = result;
    *out = result;
    return 1;
}

int jog_constraint_member(Check *ck, CheckJob *job, JogNode *member, JogType *outer, JogType **out) {
    JogNode *prop;
    JogNode *method;
    JogType *constraint;
    JogType *error;
    if (!ck || !member || !out) {
        return 0;
    }
    *out = NULL;
    if (!outer || outer->kind != JOG_TY_TYPE_PARAM || !member->as.member.name) {
        return 0;
    }
    constraint = outer->as.param.constraint;
    if (!is_iface(constraint)) {
        return 0;
    }
    error = jog_check_error_type(ck);
    prop = find_prop_req(constraint, member->as.member.name, 0);
    if (prop && prop->symbol) {
        member->symbol = prop->symbol;
        member->type = replace_type(ck->program->types, prop->type, constraint, outer);
        if (!member->type) {
            jog_check_oom(ck);
            return -1;
        }
        *out = member->type;
        return 1;
    }
    method = find_method_req(constraint, member->as.member.name, 0);
    if (method) {
        jog_check_fail(ck, member->pos, "method '%s' must be called", member->as.member.name);
        member->type = error;
        *out = error;
        return 1;
    }
    jog_check_fail(ck, member->pos, "unknown property '%s'", member->as.member.name);
    member->type = error;
    *out = error;
    (void)job;
    return 1;
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

int jog_constraint_call(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *call, JogType *base_ty,
                        JogType **out) {
    JogNode *callee;
    JogType *outer;
    JogType *constraint;
    JogNode *req;
    JogType *fn;
    JogType *error;
    size_t count;
    size_t i;
    size_t positional = 0;
    int seen_named = 0;
    unsigned char *filled;
    if (!ck || !call || !out) {
        return 0;
    }
    *out = NULL;
    callee = call->as.call.callee;
    if (!callee || callee->kind != JOG_NODE_MEMBER || callee->as.member.safe || !callee->as.member.name) {
        return 0;
    }
    outer = peel_ptr(base_ty);
    if (!outer || outer->kind != JOG_TY_TYPE_PARAM) {
        return 0;
    }
    constraint = outer->as.param.constraint;
    if (!is_iface(constraint)) {
        return 0;
    }
    error = jog_check_error_type(ck);
    if (callee->as.member.type_args.count > 0) {
        jog_check_fail(ck, callee->pos, "an interface method cannot have type parameters");
        call->type = error;
        *out = error;
        return 1;
    }
    req = find_method_req(constraint, callee->as.member.name, 0);
    if (!req || !req->symbol || !req->type || req->type->kind != JOG_TY_FN) {
        jog_check_fail(ck, call->pos, "missing method '%s'", callee->as.member.name);
        call->type = error;
        *out = error;
        return 1;
    }
    fn = replace_type(ck->program->types, req->type, constraint, outer);
    if (!fn || fn->kind != JOG_TY_FN) {
        jog_check_oom(ck);
        return -1;
    }
    callee->symbol = req->symbol;
    callee->type = fn;
    call->symbol = req->symbol;
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
            arg->type = jog_check_expr(ck, job, scope, value, want);
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
        jog_check_fail(ck, call->pos, "missing argument");
        break;
    }
    if (ck->stop) {
        return -1;
    }
    if (jog_check_context_resolve(ck, job, call) < 0) {
        return -1;
    }
    call->type = fn->as.fn.ret ? fn->as.fn.ret : jog_check_unit_type(ck);
    *out = call->type;
    return 1;
}
