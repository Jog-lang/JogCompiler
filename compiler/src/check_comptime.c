#include "check_comptime.h"

#include "fs.h"

#include <stdlib.h>
#include <string.h>

static JogNode *arg_value(JogNode *arg, int *named) {
    if (named) {
        *named = 0;
    }
    if (!arg) {
        return NULL;
    }
    if (arg->kind != JOG_NODE_ARG) {
        return arg;
    }
    if (named && arg->as.arg.name) {
        *named = 1;
    }
    return arg->as.arg.value;
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static int is_drive(const char *path) {
    char c;
    if (!path || path[0] == '\0' || path[1] != ':') {
        return 0;
    }
    c = path[0];
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char *dup_range(CtArena *arena, const char *text, size_t len) {
    char *out = ct_arena_alloc(arena, len + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

/* Parent directory. A root ("/", "C:\") is its own parent. A relative path
   with no separator returns ".". */
static char *parent_dir(CtArena *arena, const char *path) {
    size_t len = 0;
    size_t slash = 0;
    if (!arena || !path || path[0] == '\0') {
        return NULL;
    }
    len = strlen(path);
    while (len > 1 && is_sep(path[len - 1])) {
        if (len == 3 && path[1] == ':') {
            break;
        }
        len--;
    }
    if ((len == 1 && is_sep(path[0])) || (len == 2 && path[1] == ':') ||
        (len == 3 && path[1] == ':' && is_sep(path[2]))) {
        return dup_range(arena, path, len);
    }
    slash = len;
    while (slash > 0 && !is_sep(path[slash - 1])) {
        slash--;
    }
    if (slash == 0) {
        return ct_arena_strdup(arena, ".");
    }
    if (slash == 1) {
        return dup_range(arena, path, 1);
    }
    if (slash == 3 && path[1] == ':') {
        char *root = ct_arena_alloc(arena, 4, 1);
        if (!root) {
            return NULL;
        }
        root[0] = path[0];
        root[1] = ':';
        root[2] = path[slash - 1];
        root[3] = '\0';
        return root;
    }
    return dup_range(arena, path, slash - 1);
}

typedef struct PathComp {
    const char *s;
    size_t n;
} PathComp;

static int comp_push(PathComp **cs, size_t *n, size_t *cap, const char *s, size_t len) {
    if (*n >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        PathComp *grown = realloc(*cs, ncap * sizeof(*grown));
        if (!grown) {
            return 0;
        }
        *cs = grown;
        *cap = ncap;
    }
    (*cs)[*n].s = s;
    (*cs)[*n].n = len;
    (*n)++;
    return 1;
}

static int norm_add(PathComp **cs, size_t *n, size_t *cap, const char *s, size_t len, int absolute) {
    int top_dotdot;
    int top_drive;
    if (len == 0 || (len == 1 && s[0] == '.')) {
        return 1;
    }
    if (len == 2 && s[0] == '.' && s[1] == '.') {
        if (*n > 0) {
            PathComp top = (*cs)[*n - 1];
            top_dotdot = top.n == 2 && top.s[0] == '.' && top.s[1] == '.';
            top_drive = top.n == 2 && top.s[1] == ':';
            if (!top_dotdot && !top_drive) {
                (*n)--;
                return 1;
            }
            if (top_drive || absolute) {
                return 1;
            }
        } else if (absolute) {
            return 1;
        }
    }
    return comp_push(cs, n, cap, s, len);
}

static char *render_path(CtArena *arena, PathComp *cs, size_t n, int absolute, int drive, int unc) {
    size_t bytes = 0;
    size_t i;
    char *out;
    char *w;
    if (!absolute && n == 0) {
        return ct_arena_strdup(arena, ".");
    }
    if (unc) {
        bytes += 2;
    } else if (absolute && !drive) {
        bytes += 1;
    }
    for (i = 0; i < n; i++) {
        if (i > 0) {
            bytes += 1;
        }
        bytes += cs[i].n;
    }
    if (drive && n == 1) {
        bytes += 1;
    }
    out = ct_arena_alloc(arena, bytes + 1, 1);
    if (!out) {
        return NULL;
    }
    w = out;
    if (unc) {
        *w++ = '/';
        *w++ = '/';
    } else if (absolute && !drive) {
        *w++ = '/';
    }
    for (i = 0; i < n; i++) {
        if (i > 0) {
            *w++ = '/';
        }
        memcpy(w, cs[i].s, cs[i].n);
        w += cs[i].n;
    }
    if (drive && n == 1) {
        *w++ = '/';
    }
    *w = '\0';
    return out;
}

/* Collapse separators, "." and "..". Does not follow symlinks. NULL and
   *oom = 1 on allocation failure. */
static char *norm_path(CtArena *arena, const char *path, int *oom) {
    PathComp *cs = NULL;
    size_t n = 0;
    size_t cap = 0;
    char *copy;
    char *out = NULL;
    const char *p;
    int absolute = 0;
    int drive = 0;
    int unc = 0;
    if (oom) {
        *oom = 0;
    }
    if (!arena || !path) {
        if (oom) {
            *oom = 1;
        }
        return NULL;
    }
    copy = ct_arena_strdup(arena, path);
    if (!copy) {
        if (oom) {
            *oom = 1;
        }
        return NULL;
    }
    p = copy;
    if (is_drive(p)) {
        absolute = 1;
        drive = 1;
        if (!comp_push(&cs, &n, &cap, p, 2)) {
            if (oom) {
                *oom = 1;
            }
            goto done;
        }
        p += 2;
    } else if (is_sep(p[0])) {
        absolute = 1;
        if (is_sep(p[1])) {
            unc = 1;
            p += 2;
        } else {
            p += 1;
        }
    }
    while (*p != '\0') {
        const char *start;
        size_t len;
        while (is_sep(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && !is_sep(*p)) {
            p++;
        }
        len = (size_t)(p - start);
        if (!norm_add(&cs, &n, &cap, start, len, absolute)) {
            if (oom) {
                *oom = 1;
            }
            goto done;
        }
    }
    out = render_path(arena, cs, n, absolute, drive, unc);
    if (!out && oom) {
        *oom = 1;
    }
done:
    free(cs);
    return out;
}

static int fold_cmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
#if defined(_WIN32)
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
#endif
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}

static int path_inside(const char *root, const char *child) {
    size_t n;
    size_t i;
    if (!root || !child || root[0] == '\0') {
        return 0;
    }
    if (strcmp(root, ".") == 0) {
        if (is_sep(child[0]) || is_drive(child)) {
            return 0;
        }
        if (child[0] == '.' && child[1] == '.' && (child[2] == '\0' || child[2] == '/')) {
            return 0;
        }
        return 1;
    }
    n = strlen(root);
    for (i = 0; i < n; i++) {
        if (child[i] == '\0') {
            return 0;
        }
    }
    if (fold_cmp(root, child, n) != 0) {
        return 0;
    }
    if (root[n - 1] == '/') {
        return 1;
    }
    return child[n] == '\0' || child[n] == '/';
}

int jog_embed_literal(const JogNode *node, const char **text, size_t *len) {
    JogNode *arg;
    JogNode *expr;
    if (text) {
        *text = NULL;
    }
    if (len) {
        *len = 0;
    }
    if (!node || node->kind != JOG_NODE_BUILTIN || node->as.builtin.args.count != 1 || !node->as.builtin.args.items) {
        return JOG_EMBED_COUNT;
    }
    arg = node->as.builtin.args.items[0];
    if (arg && arg->kind == JOG_NODE_ARG && arg->as.arg.name) {
        return JOG_EMBED_COUNT;
    }
    expr = arg && arg->kind == JOG_NODE_ARG ? arg->as.arg.value : arg;
    if (!expr) {
        return JOG_EMBED_COUNT;
    }
    if (expr->kind != JOG_NODE_STRING_LIT || !expr->as.literal.text) {
        return JOG_EMBED_LITERAL;
    }
    if (text) {
        *text = expr->as.literal.text;
    }
    if (len) {
        *len = expr->as.literal.len;
    }
    return JOG_EMBED_OK;
}

/* The root of the module whose package holds source_file. */
static const char *owning_root(const JogProgram *program, const char *source_file) {
    size_t p;
    size_t f;
    for (p = 0; p < program->package_count; p++) {
        const JogPackage *pkg = program->packages ? program->packages[p] : NULL;
        if (!pkg || !pkg->module_root || pkg->module_root[0] == '\0') {
            continue;
        }
        for (f = 0; f < pkg->file_count; f++) {
            const JogSourceFile *file = pkg->files ? pkg->files[f] : NULL;
            if (file && file->file.path &&
                (file->file.path == source_file || strcmp(file->file.path, source_file) == 0)) {
                return pkg->module_root;
            }
        }
    }
    return NULL;
}

JogEmbedStatus jog_embed_resolve(JogProgram *program, const char *source_file, const char *literal,
                                 size_t literal_len, const char **path_out) {
    CtArena *arena;
    char *parent;
    char *joined;
    char *full;
    char *bound;
    const char *boundary;
    int oom = 0;
    if (path_out) {
        *path_out = NULL;
    }
    if (!program || !program->arena || !path_out) {
        return JOG_EMBED_OOM;
    }
    arena = program->arena;
    if (!literal || literal_len == 0 || memchr(literal, '\0', literal_len) != NULL || is_sep(literal[0]) ||
        is_drive(literal)) {
        return JOG_EMBED_RELATIVE;
    }
    if (!source_file || source_file[0] == '\0') {
        return JOG_EMBED_ESCAPE;
    }
    parent = parent_dir(arena, source_file);
    joined = parent ? ct_path_join(arena, parent, literal) : NULL;
    if (!parent || !joined) {
        return JOG_EMBED_OOM;
    }
    full = norm_path(arena, joined, &oom);
    if (oom || !full) {
        return JOG_EMBED_OOM;
    }
    boundary = owning_root(program, source_file);
    if (!boundary && program->module_root && program->module_root[0] != '\0') {
        boundary = program->module_root;
    }
    if (!boundary) {
        boundary = parent;
    }
    bound = norm_path(arena, boundary, &oom);
    if (oom || !bound) {
        return JOG_EMBED_OOM;
    }
    if (!path_inside(bound, full)) {
        return JOG_EMBED_ESCAPE;
    }
    *path_out = full;
    return JOG_EMBED_OK;
}

JogEmbedStatus jog_embed_stat(const char *path) {
    if (!path || path[0] == '\0') {
        return JOG_EMBED_MISSING;
    }
    if (ct_fs_is_dir(path)) {
        return JOG_EMBED_DIR;
    }
    if (!ct_fs_is_file(path)) {
        return JOG_EMBED_MISSING;
    }
    return JOG_EMBED_OK;
}

static void fail_embed(Check *ck, JogPos pos, JogEmbedStatus status, const char *literal) {
    const char *text = literal ? literal : "";
    switch (status) {
    case JOG_EMBED_OK:
        return;
    case JOG_EMBED_OOM:
        jog_check_oom(ck);
        return;
    case JOG_EMBED_COUNT:
        jog_check_fail(ck, pos, "@embed expects one argument");
        return;
    case JOG_EMBED_LITERAL:
        jog_check_fail(ck, pos, "@embed expects a string literal");
        return;
    case JOG_EMBED_RELATIVE:
        jog_check_fail(ck, pos, "@embed path must be a relative file path");
        return;
    case JOG_EMBED_ESCAPE:
        jog_check_fail(ck, pos, "@embed path escapes the module");
        return;
    case JOG_EMBED_DIR:
        jog_check_fail(ck, pos, "cannot embed '%s': path is a directory", text);
        return;
    case JOG_EMBED_MISSING:
        jog_check_fail(ck, pos, "cannot embed '%s': file is missing", text);
        return;
    }
}

static JogType *check_embed(Check *ck, JogNode *node) {
    const char *literal = NULL;
    size_t literal_len = 0;
    const char *full = NULL;
    JogEmbedStatus status;
    JogType *got;
    int arg;
    arg = jog_embed_literal(node, &literal, &literal_len);
    if (arg != JOG_EMBED_OK) {
        fail_embed(ck, node->pos, (JogEmbedStatus)arg, NULL);
        got = jog_check_error_type(ck);
        node->type = got;
        return got;
    }
    status = jog_embed_resolve(ck->program, node->pos.file, literal, literal_len, &full);
    if (status == JOG_EMBED_OK) {
        status = jog_embed_stat(full);
    }
    if (status != JOG_EMBED_OK) {
        fail_embed(ck, node->pos, status, literal);
        got = jog_check_error_type(ck);
        node->type = got;
        return got;
    }
    got = jog_type_builtin(ck->program->types, JOG_BUILTIN_CONST_BYTES);
    if (!got) {
        jog_check_oom(ck);
        return NULL;
    }
    node->type = got;
    return got;
}

JogType *jog_check_comptime(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *node, JogType *target) {
    const char *name;
    JogType *got;
    JogNode *arg;
    JogNode *expr;
    int named = 0;
    if (!ck || !node) {
        return NULL;
    }
    name = node->as.builtin.name ? node->as.builtin.name : "";
    if (strcmp(name, "embed") == 0) {
        return check_embed(ck, node);
    }
    if (strcmp(name, "comptime") != 0) {
        jog_check_fail(ck, node->pos, "unknown compile-time builtin @%s", name);
        got = jog_check_error_type(ck);
        node->type = got;
        return got;
    }
    if (node->as.builtin.args.count != 1 || !node->as.builtin.args.items) {
        jog_check_fail(ck, node->pos, "@comptime expects one argument");
        got = jog_check_error_type(ck);
        node->type = got;
        return got;
    }
    arg = node->as.builtin.args.items[0];
    expr = arg_value(arg, &named);
    if (named || !expr) {
        jog_check_fail(ck, node->pos, "@comptime expects one argument");
        got = jog_check_error_type(ck);
        node->type = got;
        return got;
    }
    got = jog_check_expr(ck, job, scope, expr, target);
    if (got) {
        node->type = got;
    }
    return got;
}
