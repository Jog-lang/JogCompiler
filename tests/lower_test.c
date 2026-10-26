#include "lexer.h"
#include "phases.h"

#include "file.h"
#include "fs.h"
#include "str.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static const char *base_name(const char *path) {
    const char *base = path ? path : "";
    const char *p;
    for (p = base; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static char *copy_span(CtArena *arena, const char *s, size_t n) {
    char *out = ct_arena_alloc(arena, n + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* // module: name on its own line. A dotted name is a package path.
   A single segment is the root module name. */
static void module_of(CtArena *arena, const char *src, const char **path, const char **name) {
    const char *p = src ? src : "";
    *path = "";
    *name = "app";
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *s = p;
        size_t rest;
        if (len > 0 && s[len - 1] == '\r') {
            len--;
        }
        while (len > 0 && (*s == ' ' || *s == '\t')) {
            s++;
            len--;
        }
        rest = len;
        if (rest >= 11 && s[0] == '/' && s[1] == '/' && strncmp(s + 2, " module:", 8) == 0) {
            const char *mod = s + 10;
            size_t n = rest - 10;
            const char *dot;
            char *copied;
            while (n > 0 && (*mod == ' ' || *mod == '\t')) {
                mod++;
                n--;
            }
            while (n > 0 && (mod[n - 1] == ' ' || mod[n - 1] == '\t')) {
                n--;
            }
            copied = n > 0 ? copy_span(arena, mod, n) : NULL;
            if (copied) {
                *name = copied;
                dot = strrchr(copied, '.');
                if (dot) {
                    *path = copied;
                    *name = dot + 1;
                }
            }
            return;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static int ends_with(const char *s, const char *suffix) {
    size_t n;
    size_t m;
    if (!s || !suffix) {
        return 0;
    }
    n = strlen(s);
    m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static char *swap_suffix(CtArena *arena, const char *path, const char *from, const char *to) {
    size_t n;
    size_t fn;
    size_t tn;
    char *out;
    if (!ends_with(path, from)) {
        return NULL;
    }
    n = strlen(path);
    fn = strlen(from);
    tn = strlen(to);
    out = ct_arena_alloc(arena, n - fn + tn + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, n - fn);
    memcpy(out + n - fn, to, tn + 1);
    return out;
}

static void strip_cr(char *s) {
    char *r;
    char *w;
    if (!s) {
        return;
    }
    r = s;
    w = s;
    while (*r != '\0') {
        if (*r != '\r') {
            *w++ = *r;
        }
        r++;
    }
    *w = '\0';
}

static int lower_source(const char *path, int expect_golden) {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    CtFile src;
    CtDa tokens;
    JogNode *root;
    JogSourceFile file;
    JogSourceFile *files[1];
    JogPackage pkg;
    JogPackage *pkgs[1];
    JogProgram program;
    JirModule *module;
    const char *mod_path;
    const char *mod_name;
    CtSb dump;
    char *golden_path = NULL;
    CtFile golden;
    int ok = 0;

    arena = ct_arena_create();
    if (!arena) {
        CHECK(0);
        return 0;
    }
    diag = ct_diag_create(arena);
    intern = ct_intern_create(arena);
    types = jog_types_create(arena, 8);
    CHECK(diag && intern && types);
    if (!diag || !intern || !types) {
        jog_types_release(types);
        ct_arena_destroy(arena);
        return 0;
    }
    memset(&src, 0, sizeof(src));
    memset(&tokens, 0, sizeof(tokens));
    if (!ct_da_init(&tokens, sizeof(JogToken)) || !ct_file_read(arena, diag, path, &src) || !src.data) {
        fprintf(stderr, "cannot read %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        ct_da_free(&tokens);
        jog_types_release(types);
        ct_arena_destroy(arena);
        return 0;
    }
    src.path = base_name(src.path);
    if (!jog_lex(arena, diag, intern, &src, &tokens) || ct_diag_error_count(diag) != 0) {
        fprintf(stderr, "lex errors in %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        goto done;
    }
    root = jog_parse(arena, diag, intern, &src, &tokens);
    if (!root || ct_diag_error_count(diag) != 0) {
        fprintf(stderr, "parse errors in %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        goto done;
    }
    module_of(arena, src.data, &mod_path, &mod_name);
    memset(&file, 0, sizeof(file));
    file.ast = root;
    files[0] = &file;
    memset(&pkg, 0, sizeof(pkg));
    pkg.path = mod_path;
    pkg.name = mod_name;
    pkg.files = files;
    pkg.file_count = 1;
    memset(&program, 0, sizeof(program));
    program.arena = arena;
    program.diag = diag;
    program.intern = intern;
    program.types = types;
    program.module_name = mod_name;
    program.packages = pkgs;
    program.package_count = 1;
    program.root = &pkg;
    pkgs[0] = &pkg;
    program.options.ptr_size = 8;
    if (!jog_check(&program) || ct_diag_error_count(diag) != 0) {
        fprintf(stderr, "check errors in %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        goto done;
    }
    module = jog_lower(&program);
    if (!module || ct_diag_error_count(diag) != 0) {
        fprintf(stderr, "lower errors in %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        goto done;
    }
    if (!expect_golden) {
        size_t user_globals = 0;
        size_t gi;
        CHECK(module->init_func == -1);
        for (gi = 0; gi < module->globals.count; gi++) {
            JirGlobal *global = jir_global(module, (uint32_t)gi);
            const char *name = global && global->name ? global->name : "";
            if (strncmp(name, "jog_tid_", 8) != 0 && strncmp(name, "jog_vt_", 7) != 0) {
                user_globals++;
            }
        }
        CHECK(user_globals == 0);
    } else if (module->globals.count == 0) {
        CHECK(module->init_func == -1);
    }
    CHECK(jir_verify(module, diag));
    if (ct_diag_error_count(diag) != 0) {
        fprintf(stderr, "verify errors in %s\n", path);
        ct_diag_print(diag, stderr);
        CHECK(0);
        jir_module_destroy(module);
        goto done;
    }
    ct_sb_init(&dump);
    jir_dump(module, &dump);
    if (dump.failed) {
        CHECK(0);
        ct_sb_free(&dump);
        jir_module_destroy(module);
        goto done;
    }
    if (expect_golden) {
        golden_path = swap_suffix(arena, path, ".jog", ".ir");
        CHECK(golden_path != NULL);
        memset(&golden, 0, sizeof(golden));
        if (!golden_path || !ct_file_read(arena, diag, golden_path, &golden) || !golden.data) {
            fprintf(stderr, "missing golden for %s\nGOT:\n%s", path, ct_sb_cstr(&dump));
            CHECK(0);
        } else {
            strip_cr((char *)golden.data);
            if (strcmp(golden.data, ct_sb_cstr(&dump)) != 0) {
                fprintf(stderr, "IR mismatch %s\nGOT:\n%sEXPECTED:\n%s", path, ct_sb_cstr(&dump), golden.data);
                CHECK(0);
            }
        }
    }
    if (strstr(base_name(path), "hello.jog")) {
        JirFunc *puts_fn = NULL;
        size_t i;
        CHECK(module->main_func >= 0);
        for (i = 0; i < module->funcs.count; i++) {
            JirFunc *func = jir_func(module, (uint32_t)i);
            if (func && func->linkage == JIR_LINK_EXTERN && func->name && strcmp(func->name, "puts") == 0) {
                puts_fn = func;
            }
        }
        CHECK(puts_fn != NULL);
        CHECK(puts_fn && puts_fn->type && puts_fn->type->as.fn.variadic == false);
        CHECK(puts_fn && puts_fn->blocks.count == 0);
    }
    ct_sb_free(&dump);
    jir_module_destroy(module);
    ok = g_failures == 0;
done:
    ct_da_free(&tokens);
    jog_types_release(types);
    ct_arena_destroy(arena);
    return ok;
}

static void lower_dir(const char *dir, const char *suffix, int expect_golden, int only_ok) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag;
    CtDa names;
    size_t i;
    CHECK(arena != NULL);
    if (!arena) {
        return;
    }
    diag = ct_diag_create(arena);
    if (!diag || !ct_da_init(&names, sizeof(const char *))) {
        CHECK(0);
        ct_arena_destroy(arena);
        return;
    }
    if (!ct_fs_list(arena, diag, dir, CT_FS_FILES, suffix, &names)) {
        fprintf(stderr, "cannot list %s\n", dir);
        ct_diag_print(diag, stderr);
        CHECK(0);
        ct_da_free(&names);
        ct_arena_destroy(arena);
        return;
    }
    CHECK(names.count > 0);
    for (i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        const char *path;
        if (!name) {
            CHECK(0);
            continue;
        }
        if (only_ok && strncmp(name, "ok_", 3) != 0) {
            continue;
        }
        path = ct_path_join(arena, dir, name);
        CHECK(path != NULL);
        if (path) {
            int before = g_failures;
            lower_source(path, expect_golden);
            if (g_failures != before) {
                fprintf(stderr, "while lowering %s\n", path);
            }
        }
    }
    ct_da_free(&names);
    ct_arena_destroy(arena);
}

int main(void) {
    CtArena *arena = ct_arena_create();
    const char *root;
    const char *lower_dir_path;
    const char *check_dir;
    CHECK(arena != NULL);
    if (!arena) {
        return 1;
    }
    root = CT_SOURCE_DIR;
    lower_dir_path = ct_path_join(arena, root, "tests");
    lower_dir_path = lower_dir_path ? ct_path_join(arena, lower_dir_path, "lower") : NULL;
    check_dir = ct_path_join(arena, root, "tests");
    check_dir = check_dir ? ct_path_join(arena, check_dir, "check") : NULL;
    CHECK(lower_dir_path && check_dir);
    if (lower_dir_path) {
        lower_dir(lower_dir_path, ".jog", 1, 0);
    }
    if (check_dir) {
        lower_dir(check_dir, ".jog", 0, 1);
    }
    ct_arena_destroy(arena);
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
