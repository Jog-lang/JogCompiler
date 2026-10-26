#include "check_package.h"

#include <string.h>

/* Packages, bytes and slice views, pointer `as`, and @test.
   A name in another package is visible only when it is public.
   private and private(package) stay in the declaring package.
   private(file) is not in the package scope, so another file does not see it.
   .ptr and .len are the two fields of bytes, str, and []T.
   `as` between pointer types, including a niche *T?, reinterprets the address. */

static JogNode *list_at(JogList list, size_t index) {
    if (!list.items || index >= list.count) {
        return NULL;
    }
    return list.items[index];
}

static int poison(Check *ck, JogNode *node, JogType **out) {
    JogType *error = jog_check_error_type(ck);
    if (node) {
        node->type = error;
    }
    if (out) {
        *out = error;
    }
    return ck->stop ? -1 : 1;
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

static JogType *view_ptr_type(Check *ck, JogType *type) {
    JogType *elem;
    if (type->kind == JOG_TY_BYTES) {
        elem = jog_type_builtin(ck->program->types, JOG_BUILTIN_U8);
    } else {
        elem = type->as.elem.elem;
    }
    if (!elem) {
        return NULL;
    }
    return jog_type_ptr(ck->program->types, elem, type->is_const);
}

int jog_check_package_view(Check *ck, CheckJob *job, JogNode *member, JogType **out) {
    JogNode *base;
    JogType *type;
    JogType *field;
    const char *name;
    (void)job;
    if (!ck || !member || !out) {
        return 0;
    }
    if (ck->stop) {
        return -1;
    }
    base = member->as.member.base;
    if (!base || !base->type) {
        return 0;
    }
    if (base->type->kind == JOG_TY_ERROR) {
        return poison(ck, member, out);
    }
    type = jog_type_underlying(base->type);
    if (!type || (type->kind != JOG_TY_BYTES && type->kind != JOG_TY_SLICE)) {
        return 0;
    }
    name = member->as.member.name;
    if (!name) {
        jog_check_fail(ck, member->pos, "field access is not checked yet");
        return poison(ck, member, out);
    }
    if (strcmp(name, "len") == 0) {
        field = jog_type_builtin(ck->program->types, JOG_BUILTIN_USIZE);
        if (!field) {
            jog_check_oom(ck);
            return -1;
        }
        member->type = field;
        *out = field;
        return 1;
    }
    if (strcmp(name, "ptr") == 0) {
        field = view_ptr_type(ck, type);
        if (!field) {
            jog_check_oom(ck);
            return -1;
        }
        member->type = field;
        *out = field;
        return 1;
    }
    jog_check_fail(ck, member->pos, "unknown field '%s'", name);
    return poison(ck, member, out);
}

int jog_check_package_view_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign) {
    JogNode *target;
    JogNode *base;
    JogType *type;
    JogType *field;
    JogType *got;
    const char *name;
    JogSymbol *sym;
    if (!ck || !assign || ck->stop) {
        return ck && ck->stop ? -1 : 0;
    }
    target = assign->as.assign.target;
    if (!target || target->kind != JOG_NODE_MEMBER) {
        return 0;
    }
    name = target->as.member.name;
    if (!name || (strcmp(name, "ptr") != 0 && strcmp(name, "len") != 0)) {
        return 0;
    }
    base = target->as.member.base;
    if (!base) {
        return 0;
    }
    /* A private function with no context clause is checked twice. The
       second pass binds a new local, so an ident base has to be resolved
       again even when the first pass already stored its type. Otherwise
       view.ptr = p keeps the first local and lowering cannot find it. */
    if (!base->type || base->kind == JOG_NODE_IDENT) {
        if (!jog_check_expr(ck, job, scope, base, NULL) || ck->stop) {
            return ck->stop ? -1 : 1;
        }
    }
    if (base->type && base->type->kind == JOG_TY_ERROR) {
        assign->type = base->type;
        return 1;
    }
    type = jog_type_underlying(base->type);
    if (!type || (type->kind != JOG_TY_BYTES && type->kind != JOG_TY_SLICE)) {
        return 0;
    }
    if (strcmp(name, "len") == 0) {
        field = jog_type_builtin(ck->program->types, JOG_BUILTIN_USIZE);
    } else {
        field = view_ptr_type(ck, type);
    }
    if (!field) {
        jog_check_oom(ck);
        return -1;
    }
    target->type = field;
    sym = base->kind == JOG_NODE_IDENT ? base->symbol : NULL;
    if (assign->as.assign.op != JOG_TOK_ASSIGN || type->is_const || !sym ||
        (sym->kind != JOG_SYM_VARIABLE && sym->kind != JOG_SYM_PARAMETER)) {
        jog_check_fail(ck, target->pos, "cannot assign to field '%s'", name);
        if (assign->as.assign.value) {
            (void)jog_check_expr(ck, job, scope, assign->as.assign.value, field);
        }
        assign->type = jog_check_unit_type(ck);
        return 1;
    }
    got = NULL;
    if (assign->as.assign.value) {
        got = jog_check_expr(ck, job, scope, assign->as.assign.value, field);
        if (ck->stop) {
            return -1;
        }
    }
    if (!got) {
        jog_check_fail(ck, assign->pos, "missing value");
    }
    assign->type = jog_check_unit_type(ck);
    return 1;
}

bool jog_check_package_cast(const Check *ck, const JogType *from, const JogType *to) {
    (void)ck;
    return ptr_like(from) && ptr_like(to);
}

static int is_test_attr(const JogNode *attr) {
    return attr && attr->kind == JOG_NODE_ATTRIBUTE && attr->as.attribute.name &&
           strcmp(attr->as.attribute.name, "test") == 0;
}

static int test_name_ok(Check *ck, JogNode *attr) {
    JogNode *arg;
    JogNode *value;
    if (!attr->as.attribute.has_parens || attr->as.attribute.args.count == 0) {
        return 1;
    }
    if (attr->as.attribute.args.count != 1) {
        jog_check_fail(ck, attr->pos, "@test name must be a string");
        return 0;
    }
    arg = list_at(attr->as.attribute.args, 0);
    value = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    if (!value || value->kind != JOG_NODE_STRING_LIT) {
        jog_check_fail(ck, attr->pos, "@test name must be a string");
        return 0;
    }
    return 1;
}

static int fn_returns_unit(const JogNode *fn) {
    JogType *ret;
    if (!fn->type || fn->type->kind != JOG_TY_FN) {
        return 0;
    }
    ret = fn->type->as.fn.ret;
    return !ret || ret->kind == JOG_TY_UNIT;
}

static void check_test_fn(Check *ck, JogNode *fn, int in_test_file) {
    size_t i;
    int saw = 0;
    if (!fn || fn->kind != JOG_NODE_FN) {
        return;
    }
    for (i = 0; i < fn->attrs.count && !ck->stop; i++) {
        JogNode *attr = list_at(fn->attrs, i);
        if (!is_test_attr(attr)) {
            continue;
        }
        saw = 1;
        if (!test_name_ok(ck, attr)) {
            return;
        }
    }
    if (!saw || ck->stop) {
        return;
    }
    if (!in_test_file) {
        jog_check_fail(ck, fn->pos, "@test is only allowed in a test file");
        return;
    }
    if (fn->as.fn.is_extern) {
        jog_check_fail(ck, fn->pos, "@test is not allowed on an extern function");
        return;
    }
    if (fn->as.fn.receiver || fn->as.fn.params.count > 0) {
        jog_check_fail(ck, fn->pos, "@test functions take no parameters");
        return;
    }
    if (!fn_returns_unit(fn)) {
        jog_check_fail(ck, fn->pos, "@test functions return ()");
    }
}

static void check_decl_tests(Check *ck, JogNode *decl, int in_test_file) {
    size_t i;
    if (!decl || ck->stop) {
        return;
    }
    if (decl->kind == JOG_NODE_FN) {
        check_test_fn(ck, decl, in_test_file);
        return;
    }
    if (decl->kind != JOG_NODE_EXTERN_BLOCK) {
        return;
    }
    for (i = 0; i < decl->as.extern_block.fns.count && !ck->stop; i++) {
        check_test_fn(ck, list_at(decl->as.extern_block.fns, i), in_test_file);
    }
}

void jog_check_package_tests(Check *ck) {
    size_t p;
    size_t f;
    size_t d;
    if (!ck || !ck->program || ck->failed || ck->stop) {
        return;
    }
    for (p = 0; p < ck->program->package_count && !ck->stop; p++) {
        JogPackage *pkg = ck->program->packages ? ck->program->packages[p] : NULL;
        if (!pkg) {
            continue;
        }
        for (f = 0; f < pkg->file_count && !ck->stop; f++) {
            JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            JogNode *root = file ? file->ast : NULL;
            int in_test = file && file->is_test;
            if (!root || root->kind != JOG_NODE_FILE) {
                continue;
            }
            for (d = 0; d < root->as.file.decls.count && !ck->stop; d++) {
                check_decl_tests(ck, list_at(root->as.file.decls, d), in_test);
            }
        }
    }
}
