#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "phases.h"

#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

typedef struct Harness {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    JogProgram program;
    char *saved_root;
    int had_root;
} Harness;

static char *dup_cstr(const char *text) {
    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

static int env_set(const char *value) {
#if defined(_WIN32)
    return _putenv_s("JOG_ROOT", value ? value : "") == 0;
#else
    if (!value || value[0] == '\0') {
        unsetenv("JOG_ROOT");
        return 1;
    }
    return setenv("JOG_ROOT", value, 1) == 0;
#endif
}

static void free_package_tokens(JogPackage *pkg) {
    size_t i = 0;
    if (!pkg) {
        return;
    }
    for (i = 0; i < pkg->file_count; i++) {
        if (pkg->files && pkg->files[i]) {
            ct_da_free(&pkg->files[i]->tokens);
        }
    }
    for (i = 0; i < pkg->platform_file_count; i++) {
        if (pkg->platform_files && pkg->platform_files[i]) {
            ct_da_free(&pkg->platform_files[i]->tokens);
        }
    }
}

static void close_harness(Harness *h) {
    size_t i = 0;
    if (!h) {
        return;
    }
    for (i = 0; i < h->program.package_count; i++) {
        free_package_tokens(h->program.packages[i]);
    }
    free_package_tokens(h->program.root);
    if (h->had_root) {
        env_set(h->saved_root);
    } else {
        env_set(NULL);
    }
    free(h->saved_root);
    ct_arena_destroy(h->arena);
    memset(h, 0, sizeof *h);
}

static int open_harness(Harness *h) {
    const char *env = NULL;
    memset(h, 0, sizeof *h);
    h->arena = ct_arena_create();
    h->diag = h->arena ? ct_diag_create(h->arena) : NULL;
    h->intern = h->arena ? ct_intern_create(h->arena) : NULL;
    if (!h->arena || !h->diag || !h->intern) {
        ct_arena_destroy(h->arena);
        memset(h, 0, sizeof *h);
        return 0;
    }
    h->program.arena = h->arena;
    h->program.diag = h->diag;
    h->program.intern = h->intern;
    env = getenv("JOG_ROOT");
    if (env && env[0] != '\0') {
        h->had_root = 1;
        h->saved_root = dup_cstr(env);
        if (!h->saved_root) {
            close_harness(h);
            return 0;
        }
    }
    if (!env_set(NULL)) {
        close_harness(h);
        return 0;
    }
    return 1;
}

static const char *fixture(Harness *h, const char *rel) {
    const char *root = ct_path_join(h->arena, CT_SOURCE_DIR, "tests/jog/loader");
    if (!root) {
        return NULL;
    }
    return ct_path_join(h->arena, root, rel);
}

static int load_rel(Harness *h, const char *rel) {
    const char *path = fixture(h, rel);
    if (!path) {
        return 0;
    }
    return jog_load(&h->program, path);
}

static char *diag_text(CtDiag *diag) {
    FILE *fp = tmpfile();
    long n = 0;
    char *buf = NULL;
    size_t got = 0;
    if (!fp) {
        return NULL;
    }
    ct_diag_print(diag, fp);
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    n = ftell(fp);
    if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void expect_fragment(CtDiag *diag, const char *fragment) {
    char *text = diag_text(diag);
    if (!text || !strstr(text, fragment)) {
        fprintf(stderr, "diagnostic missing '%s':\n%s\n", fragment, text ? text : "(null)");
    }
    CHECK(text && strstr(text, fragment));
    free(text);
}

static char norm_char(char c) {
    if (c == '\\') {
        return '/';
    }
#if defined(_WIN32)
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
#endif
    return c;
}

static int path_eq(const char *left, const char *right) {
    if (!left || !right) {
        return 0;
    }
    while (*left && *right) {
        if (norm_char(*left) != norm_char(*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int path_ends(const char *path, const char *suffix) {
    size_t path_len = 0;
    size_t suffix_len = 0;
    size_t i = 0;
    if (!path || !suffix) {
        return 0;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (suffix_len > path_len) {
        return 0;
    }
    for (i = 0; i < suffix_len; i++) {
        if (norm_char(path[path_len - suffix_len + i]) != norm_char(suffix[i])) {
            return 0;
        }
    }
    if (path_len > suffix_len && norm_char(path[path_len - suffix_len - 1]) != '/') {
        return 0;
    }
    return 1;
}

static int deps_first(const JogProgram *program) {
    size_t i = 0;
    for (i = 0; i < program->package_count; i++) {
        JogPackage *pkg = program->packages[i];
        size_t j = 0;
        for (j = 0; j < pkg->import_count; j++) {
            size_t k = 0;
            int found = 0;
            for (k = 0; k < i; k++) {
                if (program->packages[k] == pkg->imports[j]) {
                    found = 1;
                }
            }
            if (!found) {
                fprintf(stderr, "package '%s' imports '%s' which is not earlier in the list\n", pkg->path,
                        pkg->imports[j] ? pkg->imports[j]->path : "(null)");
                return 0;
            }
        }
    }
    return 1;
}

static const char *fn_at(const JogSourceFile *file, size_t index) {
    JogNode *decl = NULL;
    if (!file || !file->ast || file->ast->kind != JOG_NODE_FILE) {
        return NULL;
    }
    if (index >= file->ast->as.file.decls.count || !file->ast->as.file.decls.items) {
        return NULL;
    }
    decl = file->ast->as.file.decls.items[index];
    if (!decl || decl->kind != JOG_NODE_FN) {
        return NULL;
    }
    return decl->as.fn.name;
}

static int file_named(const JogSourceFile *file, const char *name) {
    return file && file->file.path && path_ends(file->file.path, name);
}

static int paths_are(const JogProgram *program, const char *const *paths, size_t count) {
    size_t i = 0;
    if (program->package_count != count) {
        fprintf(stderr, "package count %zu, expected %zu\n", program->package_count, count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!program->packages[i] || strcmp(program->packages[i]->path, paths[i]) != 0) {
            fprintf(stderr, "package[%zu] is '%s', expected '%s'\n", i,
                    program->packages[i] ? program->packages[i]->path : "(null)", paths[i]);
            return 0;
        }
    }
    return program->root == program->packages[count - 1];
}

static void require_load(Harness *h, const char *rel) {
    int ok = load_rel(h, rel);
    if (!ok) {
        fprintf(stderr, "jog_load failed for %s\n", rel);
        ct_diag_print(h->diag, stderr);
    }
    CHECK(ok);
}

static void test_arguments(void) {
    Harness h;
    CHECK(!jog_load(NULL, "x"));
    CHECK(open_harness(&h));
    CHECK(!jog_load(&h.program, NULL));
    CHECK(!jog_load(&h.program, ""));
    expect_fragment(h.diag, "missing start path");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!jog_load(&h.program, "no/such/jog-loader-start"));
    expect_fragment(h.diag, "cannot find");
    close_harness(&h);
}

static void check_basic(Harness *h, int with_tests) {
    const char *root_only[] = {""};
    if (!h->program.root || !h->program.module_name) {
        CHECK(h->program.root != NULL);
        CHECK(h->program.module_name != NULL);
        return;
    }
    CHECK(strcmp(h->program.module_name, "basic") == 0);
    CHECK(h->program.link_lib_count == 0);
    CHECK(h->program.link_libs == NULL);
    CHECK(path_ends(h->program.module_root, "tests/jog/loader/basic"));
    CHECK(ct_fs_is_file(ct_path_join(h->arena, h->program.module_root, "jog.mod")));
    CHECK(paths_are(&h->program, root_only, 1));
    CHECK(deps_first(&h->program));
    CHECK(h->program.root->import_count == 0);
    CHECK(!h->program.root->is_std);
    CHECK(h->program.root->name && strcmp(h->program.root->name, "basic") == 0);
    if (with_tests) {
        CHECK(h->program.root->file_count == 3);
        CHECK(file_named(h->program.root->files[0], "a.jog"));
        CHECK(!h->program.root->files[0]->is_test);
        CHECK(fn_at(h->program.root->files[0], 0) && strcmp(fn_at(h->program.root->files[0], 0), "alpha") == 0);
        CHECK(file_named(h->program.root->files[1], "a.test.jog"));
        CHECK(h->program.root->files[1]->is_test);
        CHECK(fn_at(h->program.root->files[1], 0) && strcmp(fn_at(h->program.root->files[1], 0), "alpha_test") == 0);
        CHECK(file_named(h->program.root->files[2], "b.jog"));
        CHECK(!h->program.root->files[2]->is_test);
        CHECK(fn_at(h->program.root->files[2], 0) && strcmp(fn_at(h->program.root->files[2], 0), "beta") == 0);
    } else {
        CHECK(h->program.root->file_count == 2);
        CHECK(file_named(h->program.root->files[0], "a.jog"));
        CHECK(file_named(h->program.root->files[1], "b.jog"));
        CHECK(h->program.root->files[0]->tokens.count > 2);
        CHECK(h->program.root->files[0]->ast->kind == JOG_NODE_FILE);
    }
    CHECK(ct_diag_error_count(h->diag) == 0);
}

static void test_basic_entries(void) {
    Harness h;
    CHECK(open_harness(&h));
    require_load(&h, "basic");
    check_basic(&h, 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "basic/a.jog");
    check_basic(&h, 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "basic/jog.mod");
    check_basic(&h, 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    require_load(&h, "basic");
    check_basic(&h, 1);
    close_harness(&h);
}

static void test_order(void) {
    Harness h;
    const char *chain[] = {"leaf", "mid", ""};
    const char *diamond[] = {"c", "a", "b", ""};
    CHECK(open_harness(&h));
    require_load(&h, "order");
    CHECK(paths_are(&h.program, chain, 3));
    CHECK(deps_first(&h.program));
    CHECK(h.program.packages[1]->import_count == 1);
    CHECK(h.program.packages[1]->imports[0] == h.program.packages[0]);
    CHECK(h.program.root->import_count == 1);
    CHECK(strcmp(h.program.root->imports[0]->path, "mid") == 0);
    CHECK(fn_at(h.program.packages[0]->files[0], 0) && strcmp(fn_at(h.program.packages[0]->files[0], 0), "leaf") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "diamond");
    CHECK(paths_are(&h.program, diamond, 4));
    CHECK(deps_first(&h.program));
    CHECK(h.program.root->import_count == 2);
    CHECK(strcmp(h.program.root->imports[0]->path, "a") == 0);
    CHECK(strcmp(h.program.root->imports[1]->path, "b") == 0);
    CHECK(h.program.packages[1]->import_count == 1);
    CHECK(h.program.packages[2]->import_count == 1);
    CHECK(h.program.packages[1]->imports[0] == h.program.packages[0]);
    CHECK(h.program.packages[2]->imports[0] == h.program.packages[0]);
    close_harness(&h);
}

static void test_cycle(void) {
    Harness h;
    const char *order[] = {"right", "mid", "left", ""};
    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "cycle"));
    expect_fragment(h.diag, "import cycle: left -> mid -> right -> left");
    CHECK(paths_are(&h.program, order, 4));
    CHECK(deps_first(&h.program));
    CHECK(h.program.packages[0]->import_count == 0);
    CHECK(h.program.packages[1]->import_count == 1);
    CHECK(strcmp(h.program.packages[1]->imports[0]->path, "right") == 0);
    CHECK(h.program.packages[2]->import_count == 1);
    CHECK(strcmp(h.program.packages[2]->imports[0]->path, "mid") == 0);
    close_harness(&h);
}

static void test_dedup(void) {
    Harness h;
    const char *order[] = {"store", ""};
    CHECK(open_harness(&h));
    require_load(&h, "dedup");
    CHECK(paths_are(&h.program, order, 2));
    CHECK(h.program.root->import_count == 1);
    CHECK(strcmp(h.program.root->imports[0]->name, "store") == 0);
    CHECK(h.program.root->files[0]->ast->as.file.imports.count == 2);
    close_harness(&h);
}

static int set_root(Harness *h, const char *rel) {
    const char *path = fixture(h, rel);
    return path && env_set(path);
}

static void test_std_and_links(void) {
    Harness h;
    const char *order[] = {"std.io", "store", ""};
    JogPackage *io = NULL;
    CHECK(open_harness(&h));
    CHECK(set_root(&h, "stdroot"));
    require_load(&h, "stduse");
    CHECK(paths_are(&h.program, order, 3));
    CHECK(deps_first(&h.program));
    io = h.program.packages[0];
    CHECK(io->is_std);
    CHECK(strcmp(io->name, "io") == 0);
    CHECK(path_eq(io->dir, ct_path_join(h.arena, h.program.std_root, "io")));
    CHECK(fn_at(io->files[0], 0) && strcmp(fn_at(io->files[0], 0), "write") == 0);
    CHECK(!h.program.packages[1]->is_std);
    CHECK(path_ends(h.program.std_root, "tests/jog/loader/stdroot/std"));
    CHECK(path_ends(h.program.runtime_root, "tests/jog/loader/stdroot/runtime"));
    CHECK(h.program.root->import_count == 2);
    CHECK(h.program.root->imports[0] == io);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "links");
    CHECK(h.program.module_name && strcmp(h.program.module_name, "demo") == 0);
    CHECK(h.program.link_lib_count == 2);
    CHECK(h.program.link_libs != NULL);
    CHECK(strcmp(h.program.link_libs[0], "SDL3") == 0);
    CHECK(strcmp(h.program.link_libs[1], "m") == 0);
    close_harness(&h);
}

static void test_failures(void) {
    Harness h;
    const char *partial[] = {"store", ""};
    const char *secret[] = {"secret", ""};
    const char *helper[] = {"helper", ""};

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "badparse"));
    expect_fragment(h.diag, "main.jog");
    CHECK(h.program.root != NULL);
    CHECK(h.program.root->file_count == 1);
    CHECK(h.program.root->files[0]->ast != NULL);
    CHECK(ct_diag_error_count(h.diag) > 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "badtest");
    CHECK(h.program.root->file_count == 1);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    CHECK(!load_rel(&h, "badtest"));
    expect_fragment(h.diag, "main.test.jog");
    CHECK(h.program.root->file_count == 2);
    CHECK(h.program.root->files[1]->is_test);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "tests_import");
    CHECK(h.program.package_count == 1);
    CHECK(h.program.root->import_count == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    require_load(&h, "tests_import");
    CHECK(paths_are(&h.program, helper, 2));
    CHECK(h.program.root->import_count == 1);
    CHECK(strcmp(h.program.root->imports[0]->path, "helper") == 0);
    CHECK(fn_at(h.program.root->files[1], 0) && strcmp(fn_at(h.program.root->files[1], 0), "check") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(set_root(&h, "nostd"));
    CHECK(!load_rel(&h, "nostd"));
    expect_fragment(h.diag, "standard library was not found");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(set_root(&h, "stdempty"));
    CHECK(!load_rel(&h, "needio"));
    expect_fragment(h.diag, "cannot find package 'std.missing'");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "shadow"));
    expect_fragment(h.diag, "folder 'std' shadows the standard library");
    CHECK(ct_diag_error_count(h.diag) == 1);
    CHECK(h.program.package_count == 1);
    CHECK(h.program.root->file_count == 1);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "partial"));
    expect_fragment(h.diag, "cannot find package 'missing'");
    CHECK(paths_are(&h.program, partial, 2));
    CHECK(deps_first(&h.program));
    CHECK(h.program.root->import_count == 1);
    CHECK(strcmp(h.program.root->imports[0]->path, "store") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "emptykg"));
    expect_fragment(h.diag, "package 'bare' has no source files");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "onlytest"));
    expect_fragment(h.diag, "package 'secret' has no source files");
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    require_load(&h, "onlytest");
    CHECK(paths_are(&h.program, secret, 2));
    CHECK(h.program.packages[0]->files[0]->is_test);
    CHECK(fn_at(h.program.packages[0]->files[0], 0) && strcmp(fn_at(h.program.packages[0]->files[0], 0), "hidden") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "upward/nested/deep/keep.txt");
    CHECK(h.program.module_name && strcmp(h.program.module_name, "upward") == 0);
    CHECK(h.program.package_count == 1);
    CHECK(fn_at(h.program.root->files[0], 0) && strcmp(fn_at(h.program.root->files[0], 0), "main") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    require_load(&h, "upward/nested/deep");
    CHECK(strcmp(h.program.module_name, "upward") == 0);
    close_harness(&h);
}

static void test_manifest_errors(void) {
    Harness h;
    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "badmod"));
    expect_fragment(h.diag, "first declaration");
    CHECK(h.program.module_name == NULL);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "emptymod"));
    expect_fragment(h.diag, "missing module name");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "badname"));
    expect_fragment(h.diag, "not an identifier");
    CHECK(h.program.module_name == NULL);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "dupmod"));
    expect_fragment(h.diag, "duplicate module declaration");
    CHECK(h.program.module_name == NULL);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "unknown"));
    expect_fragment(h.diag, "unknown declaration 'use'");
    CHECK(h.program.module_name == NULL);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_rel(&h, "extramod"));
    expect_fragment(h.diag, "extra tokens after the module name");
    CHECK(h.program.module_name == NULL);
    close_harness(&h);
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

/* Parent of a normal absolute path, matching <exe>/../std. */
static char *parent_path(CtArena *arena, const char *path) {
    size_t len = 0;
    size_t slash = 0;
    if (!path) {
        return NULL;
    }
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        if (len == 3 && path[1] == ':') {
            break;
        }
        len--;
    }
    slash = len;
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\') {
        slash--;
    }
    if (slash == 0) {
        return NULL;
    }
    if (slash == 3 && path[1] == ':') {
        char *root = ct_arena_alloc(arena, 4, 1);
        if (!root) {
            return NULL;
        }
        root[0] = path[0];
        root[1] = ':';
        root[2] = path[2];
        root[3] = '\0';
        return root;
    }
    if (slash == 1) {
        return dup_range(arena, path, 1);
    }
    return dup_range(arena, path, slash - 1);
}

static char *current_exe(CtArena *arena) {
#if defined(_WIN32)
    DWORD size = 32768;
    char *buf = ct_arena_alloc(arena, size, 1);
    DWORD got = 0;
    if (!buf) {
        return NULL;
    }
    got = GetModuleFileNameA(NULL, buf, size);
    if (got == 0 || got >= size) {
        return NULL;
    }
    return buf;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    return ct_arena_strdup(arena, buf);
#else
    char buf[4096];
    ssize_t got = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (got < 0) {
        return NULL;
    }
    buf[got] = '\0';
    return ct_arena_strdup(arena, buf);
#endif
}

static void test_exe_relative_roots(void) {
    Harness h;
    char *exe = NULL;
    char *exe_dir = NULL;
    char *base = NULL;
    const char *std_dir = NULL;
    const char *runtime_dir = NULL;
    CHECK(open_harness(&h));
    require_load(&h, "basic");
    exe = current_exe(h.arena);
    exe_dir = parent_path(h.arena, exe);
    base = parent_path(h.arena, exe_dir);
    std_dir = base ? ct_path_join(h.arena, base, "std") : NULL;
    runtime_dir = base ? ct_path_join(h.arena, base, "runtime") : NULL;
    CHECK(exe && exe_dir && base && std_dir && runtime_dir);
    if (std_dir && ct_fs_is_dir(std_dir)) {
        CHECK(path_eq(h.program.std_root, std_dir));
    } else {
        CHECK(h.program.std_root == NULL);
    }
    if (runtime_dir && ct_fs_is_dir(runtime_dir)) {
        CHECK(path_eq(h.program.runtime_root, runtime_dir));
    } else {
        CHECK(h.program.runtime_root == NULL);
    }
    close_harness(&h);
}

/* --- P2.5: jog.mod v2 and --module ------------------------------------ */

static const char *modules_dir(Harness *h, const char *rel) {
    const char *root = ct_path_join(h->arena, CT_SOURCE_DIR, "tests/jog/pkg/modules");
    return root ? ct_path_join(h->arena, root, rel) : NULL;
}

static JogModuleArg g_args[4];

/* Loads modules/<app> with each "name=folder" of maps as --module. */
static int load_mapped(Harness *h, const char *app, const char *const *maps, size_t count) {
    size_t i = 0;
    const char *path = modules_dir(h, app);
    for (i = 0; i < count && i < 4; i++) {
        const char *eq = strchr(maps[i], '=');
        g_args[i].name = eq ? dup_range(h->arena, maps[i], (size_t)(eq - maps[i])) : maps[i];
        g_args[i].dir = eq ? modules_dir(h, eq + 1) : NULL;
    }
    h->program.module_args = g_args;
    h->program.module_arg_count = count;
    return path && jog_load(&h->program, path);
}

static JogPackage *find_pkg(const JogProgram *program, const char *path) {
    size_t i = 0;
    for (i = 0; i < program->package_count; i++) {
        if (program->packages[i] && strcmp(program->packages[i]->path, path) == 0) {
            return program->packages[i];
        }
    }
    return NULL;
}

static int imports_path(const JogPackage *pkg, const char *path) {
    size_t i = 0;
    for (i = 0; pkg && i < pkg->import_count; i++) {
        if (pkg->imports[i] && strcmp(pkg->imports[i]->path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_module_mapping(void) {
    Harness h;
    const char *lib_map[] = {"lib=lib"};
    JogPackage *app_util = NULL;
    JogPackage *lib_util = NULL;
    JogPackage *lib_root = NULL;
    JogPackage *shapes = NULL;

    /* Canonical paths: app's util stays util, lib's util is lib.util. */
    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    CHECK(load_mapped(&h, "app", lib_map, 1));
    CHECK(deps_first(&h.program));
    CHECK(!h.program.is_library);
    app_util = find_pkg(&h.program, "util");
    lib_util = find_pkg(&h.program, "lib.util");
    lib_root = find_pkg(&h.program, "lib");
    shapes = find_pkg(&h.program, "lib.shapes");
    CHECK(app_util && lib_util && lib_root && shapes);
    CHECK(find_pkg(&h.program, "lib.internal") == NULL);
    if (app_util && lib_util && lib_root && shapes) {
        CHECK(strcmp(app_util->module_name, "app") == 0);
        CHECK(path_ends(app_util->module_root, "modules/app"));
        CHECK(strcmp(lib_util->module_name, "lib") == 0);
        CHECK(strcmp(lib_util->name, "util") == 0);
        CHECK(path_ends(lib_util->dir, "modules/lib/util"));
        CHECK(path_ends(lib_util->module_root, "modules/lib"));
        CHECK(strcmp(lib_root->name, "lib") == 0);
        CHECK(imports_path(lib_root, "lib.util"));
        CHECK(!imports_path(lib_root, "util"));
        /* The dependency's lib.test.jog is not loaded; the app's is. */
        CHECK(lib_root->file_count == 1);
        CHECK(file_named(lib_root->files[0], "lib.jog"));
        CHECK(h.program.root->file_count == 2);
        CHECK(h.program.root->files[1]->is_test);
    }
    close_harness(&h);

    /* A library as the root: its exports are loaded, others are not. */
    CHECK(open_harness(&h));
    CHECK(jog_load(&h.program, modules_dir(&h, "lib")));
    CHECK(h.program.is_library);
    CHECK(find_pkg(&h.program, "shapes") != NULL);
    CHECK(find_pkg(&h.program, "util") != NULL);
    CHECK(find_pkg(&h.program, "internal") == NULL);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", NULL, 0));
    expect_fragment(h.diag, "main.jog:2:1: error: module 'lib' is required by jog.mod but was not given with "
                            "--module; build with jog-build, or pass --module lib=<dir>");
    close_harness(&h);
}

static void test_module_errors(void) {
    Harness h;
    const char *lib_map[] = {"lib=lib"};
    const char *other_map[] = {"lib=other"};
    const char *plain_map[] = {"lib=lib", "plain=plain"};
    const char *twice_map[] = {"lib=lib", "lib=lib"};
    const char *std_map[] = {"std=lib"};
    const char *self_map[] = {"app=lib"};

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "noexport", lib_map, 1));
    expect_fragment(h.diag, "main.jog:1:1: error: package 'lib.internal' is not exported by module 'lib'");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "shadow", lib_map, 1));
    expect_fragment(h.diag, "jog.mod:2:9: error: folder 'lib' shadows module 'lib'");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "plain", lib_map, 1));
    expect_fragment(h.diag, "--module lib: no loaded jog.mod requires module 'lib'");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", other_map, 1));
    expect_fragment(h.diag, "jog.mod:1:8: error: jog.mod: module 'other' was given as --module lib=");
    CHECK(ct_diag_error_count(h.diag) == 1);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", plain_map, 2));
    expect_fragment(h.diag, "module 'plain' is not a library");
    CHECK(ct_diag_error_count(h.diag) == 1);
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", twice_map, 2));
    expect_fragment(h.diag, "--module lib is given twice");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", std_map, 1));
    expect_fragment(h.diag, "--module cannot map std");
    close_harness(&h);

    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "app", self_map, 1));
    expect_fragment(h.diag, "--module app: 'app' is the module being built");
    close_harness(&h);

    /* lib.util and the app's own lib_util would share jog_lib_util__. */
    CHECK(open_harness(&h));
    CHECK(!load_mapped(&h, "clash", lib_map, 1));
    expect_fragment(h.diag, "package 'lib.util' of module 'lib' and package 'lib_util' of module 'clash' have the "
                            "same C name prefix 'jog_lib_util'");
    close_harness(&h);
}

static int links_are(const JogProgram *program, const char *const *libs, size_t count) {
    size_t i = 0;
    if (program->link_lib_count != count) {
        fprintf(stderr, "link count %zu, expected %zu\n", program->link_lib_count, count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(program->link_libs[i], libs[i]) != 0) {
            fprintf(stderr, "link[%zu] is '%s', expected '%s'\n", i, program->link_libs[i], libs[i]);
            return 0;
        }
    }
    return 1;
}

static void test_module_links(void) {
    Harness h;
    const char *map[] = {"linklib=linklib"};
#if defined(_WIN32)
    const char *host[] = {"z", "ws2_32", "m", "gdi32"};
#elif defined(__APPLE__)
    const char *host[] = {"z", "objc", "m", "iokit"};
#else
    const char *host[] = {"z", "dl", "m", "rt"};
#endif
    const char *wasm[] = {"z", "m"};

    /* The root's lines, then linklib's; m keeps its last position. */
    CHECK(open_harness(&h));
    h.program.options.ptr_size = 8;
    CHECK(load_mapped(&h, "linkapp", map, 1));
    CHECK(links_are(&h.program, host, 4));
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.ptr_size = 4;
    CHECK(load_mapped(&h, "linkapp", map, 1));
    CHECK(links_are(&h.program, wasm, 2));
    close_harness(&h);
}

static const char *temp_base(CtArena *arena, unsigned n) {
    char name[64];
    const char *base = NULL;
#if defined(_WIN32)
    base = ct_fs_getenv(arena, "TEMP");
    snprintf(name, sizeof name, "jog-loader-%lu-%u", (unsigned long)GetCurrentProcessId(), n);
#else
    base = getenv("TMPDIR");
    if (!base || base[0] == '\0') {
        base = "/tmp";
    }
    snprintf(name, sizeof name, "jog-loader-%lu-%u", (unsigned long)getpid(), n);
#endif
    return base ? ct_path_join(arena, base, name) : NULL;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = ct_fs_fopen(path, "wb");
    size_t len = strlen(text);
    int ok = 0;
    if (!fp) {
        return 0;
    }
    ok = fwrite(text, 1, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

static void remove_dir(const char *dir) {
#if defined(_WIN32)
    RemoveDirectoryA(dir);
#else
    rmdir(dir);
#endif
}

/* A temporary module with the given jog.mod, a main.jog, and, when
   with_exports is set, packages x and x/y. Returns jog_load's result. */
static int load_manifest(Harness *h, unsigned n, const char *manifest, int with_exports) {
    const char *dir = temp_base(h->arena, n);
    const char *x = dir ? ct_path_join(h->arena, dir, "x") : NULL;
    const char *y = x ? ct_path_join(h->arena, x, "y") : NULL;
    const char *files[4];
    int ok = 0;
    size_t i = 0;
    if (!dir || !x || !y) {
        return 0;
    }
    files[0] = ct_path_join(h->arena, dir, "jog.mod");
    files[1] = ct_path_join(h->arena, dir, "main.jog");
    files[2] = ct_path_join(h->arena, x, "x.jog");
    files[3] = ct_path_join(h->arena, y, "y.jog");
    ct_fs_mkdir(dir);
    CHECK(write_text(files[0], manifest));
    CHECK(write_text(files[1], "fn main() {\n}\n"));
    if (with_exports) {
        ct_fs_mkdir(x);
        ct_fs_mkdir(y);
        CHECK(write_text(files[2], "fn x() {\n}\n"));
        CHECK(write_text(files[3], "fn y() {\n}\n"));
    }
    ok = jog_load(&h->program, dir);
    for (i = 0; i < 4; i++) {
        ct_fs_remove(files[i]);
    }
    remove_dir(y);
    remove_dir(x);
    remove_dir(dir);
    return ok;
}

typedef struct BadManifest {
    const char *text;
    const char *fragment; /* "<line>:<col>: error: <message>" */
} BadManifest;

static void test_manifest_v2(void) {
    static const BadManifest bad[] = {
        {"module m\nlibdir lib\n", "jog.mod:2:1: error: jog.mod: 'libdir' is reserved for a later version"},
        {"module m\nlibdir.windows lib\n", "jog.mod:2:1: error: jog.mod: 'libdir.windows' is reserved"},
        {"module m\nsource x\n", "jog.mod:2:1: error: jog.mod: 'source' is reserved"},
        {"module m\nreplace a ../a\n", "jog.mod:2:1: error: jog.mod: 'replace' is reserved"},
        {"module m\nconfig debug\n", "jog.mod:2:1: error: jog.mod: 'config' is reserved"},
        {"module m\nrequire.debug a 1.0.0\n", "jog.mod:2:1: error: jog.mod: 'require.debug' is reserved"},
        {"module m\ninclude assets\n", "jog.mod:2:1: error: jog.mod: 'include' is reserved"},
        {"module m\nexclude out\n", "jog.mod:2:1: error: jog.mod: 'exclude' is reserved"},
        {"module m\nprovides log\n", "jog.mod:2:1: error: jog.mod: 'provides' is reserved"},
        {"module m\nneeds log\n", "jog.mod:2:1: error: jog.mod: 'needs' is reserved"},
        {"module m\nlink.wasm32 c\n", "jog.mod:2:6: error: jog.mod: unknown target 'wasm32'"},
        {"module m\nversion v1.0.0\n", "jog.mod:2:9: error: jog.mod: 'v1.0.0' is not a version"},
        {"module m\nversion 1.0.0+build.5\n", "jog.mod:2:9: error: jog.mod: '1.0.0+build.5' is not a version"},
        {"module m\nversion 01.0.0\n", "jog.mod:2:9: error: jog.mod: '01.0.0' is not a version"},
        {"module m\nversion 1.0.0\nversion 1.0.1\n", "jog.mod:3:1: error: jog.mod: duplicate version declaration"},
        {"module m\nkind tool\n", "jog.mod:2:6: error: jog.mod: unknown kind 'tool'"},
        {"module m\nkind library\nkind library\n", "jog.mod:3:1: error: jog.mod: duplicate kind declaration"},
        {"module m\njog 99.0.0\n", "jog.mod:2:5: error: jog.mod: module 'm' needs jog 99.0.0 or later"},
        {"module m\nexport x\n", "jog.mod:2:8: error: jog.mod: only a library can export packages"},
        {"module m\nkind library\nexport x\nexport x\n", "jog.mod:4:8: error: jog.mod: package 'x' is exported twice"},
        {"module m\nkind library\nexport x..y\n", "jog.mod:3:8: error: jog.mod: export 'x..y' is not a package path"},
        {"module m\nrequire std 1.0.0\n", "jog.mod:2:9: error: jog.mod: std is built in and cannot be required"},
        {"module m\nrequire m ../m\n", "jog.mod:2:9: error: jog.mod: module 'm' cannot require itself"},
        {"module m\nrequire a ../a\nrequire a ../b\n", "jog.mod:3:9: error: jog.mod: module 'a' is required twice"},
        {"module m\nrequire a latest\n", "jog.mod:2:11: error: jog.mod: 'latest' is neither a path nor a version"},
        {"module m\nrequire a\n", "jog.mod:2:9: error: jog.mod: missing the path or version of module 'a'"},
        {"module m\nlink \"my lib\"\n", "jog.mod:2:6: error: jog.mod: quoted words are reserved for a later version"},
        {"module std\n", "jog.mod:1:8: error: jog.mod: module name 'std' is reserved for the standard library"},
        {"module m\nuse x\n", "jog.mod:2:1: error: jog.mod: unknown declaration 'use'"},
        {"module m\ntargets haiku\n",
         "jog.mod:2:9: error: jog.mod: unknown target 'haiku'; expected windows, linux, macos, or wasm"},
        {"module m\ntargets posix\n",
         "jog.mod:2:9: error: jog.mod: unknown target 'posix'; expected windows, linux, macos, or wasm"},
        {"module m\ntargets\n", "jog.mod:2:1: error: jog.mod: missing target name"},
        {"module m\ntargets windows\ntargets linux\n", "jog.mod:3:1: error: jog.mod: duplicate targets declaration"},
        {"module m\ntargets windows windows\n", "jog.mod:2:17: error: jog.mod: target 'windows' is listed twice"},
    };
    Harness h;
    size_t i = 0;

    /* Every v2 directive at once. Requires that nothing imports need no
       --module; a prerelease of this compiler's version is accepted. */
    CHECK(open_harness(&h));
    CHECK(load_manifest(&h, 0,
                        "# v2\r\nmodule m # name\r\nversion 1.2.3-rc.1\r\nkind library\r\njog 0.0.1-alpha.2\r\n"
                        "require a ../a\r\nrequire b 1.4.0\r\nrequire c C:/vendor/c\r\nrequire d /opt/d.jogz\r\n"
                        "export x\r\nexport x.y\r\nlink m\r\nlink.windows ws2_32\r\nlink.linux dl\r\n"
                        "link.macos objc\r\n",
                        1));
    CHECK(h.program.module_name && strcmp(h.program.module_name, "m") == 0);
    CHECK(h.program.is_library);
    CHECK(h.program.package_count == 3);
    CHECK(find_pkg(&h.program, "x") != NULL);
    CHECK(find_pkg(&h.program, "x.y") != NULL);
    CHECK(h.program.link_lib_count == 2);
    CHECK(h.program.link_lib_count > 0 && strcmp(h.program.link_libs[0], "m") == 0);
    close_harness(&h);

    /* A V1 file means version 0.0.0, an executable, no requires. */
    CHECK(open_harness(&h));
    CHECK(load_manifest(&h, 1, "module m\nlink SDL3\nlink m\n", 0));
    CHECK(!h.program.is_library);
    CHECK(h.program.link_lib_count == 2);
    close_harness(&h);

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(open_harness(&h));
        CHECK(!load_manifest(&h, (unsigned)(i + 2), bad[i].text, 0));
        expect_fragment(h.diag, bad[i].fragment);
        close_harness(&h);
    }
}

static const char *pkg_fixture(Harness *h, const char *rel) {
    const char *root = ct_path_join(h->arena, CT_SOURCE_DIR, "tests/jog/pkg");
    if (!root) {
        return NULL;
    }
    return ct_path_join(h->arena, root, rel);
}

static int files_are(const JogPackage *pkg, const char *const *names, size_t count) {
    size_t i = 0;
    if (!pkg || pkg->file_count != count) {
        fprintf(stderr, "file count %zu, expected %zu\n", pkg ? pkg->file_count : 0, count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *path = pkg->files[i] && pkg->files[i]->file.path ? pkg->files[i]->file.path : "(null)";
        if (!file_named(pkg->files[i], names[i])) {
            fprintf(stderr, "file[%zu] is '%s', expected to end with '%s'\n", i, path, names[i]);
            return 0;
        }
    }
    return 1;
}

/* <name>.<os>.jog and <name>.<os>.test.jog load only for the target.
   a.b.jog and keep.windows.extra.jog stay ordinary files. ptr_size 4 is wasm. */
static void test_per_os_files(void) {
    Harness h;
    const char *wasm_src[] = {"a.b.jog", "host.wasm.jog", "keep.windows.extra.jog", "main.jog"};
    const char *wasm_tests[] = {"a.b.jog",           "a.b.test.jog", "extra.wasm.test.jog", "host.wasm.jog",
                                 "keep.windows.extra.jog", "main.jog", "plain.test.jog"};
#if defined(_WIN32)
    const char *host_src[] = {"a.b.jog", "host.windows.jog", "keep.windows.extra.jog", "main.jog"};
    const char *host_tests[] = {"a.b.jog",           "a.b.test.jog", "extra.windows.test.jog", "host.windows.jog",
                                 "keep.windows.extra.jog", "main.jog", "plain.test.jog"};
#elif defined(__APPLE__)
    const char *host_src[] = {"a.b.jog", "host.macos.jog", "host.posix.jog", "keep.windows.extra.jog", "main.jog"};
    const char *host_tests[] = {"a.b.jog",
                                 "a.b.test.jog",
                                 "extra.posix.test.jog",
                                 "host.macos.jog",
                                 "host.posix.jog",
                                 "keep.windows.extra.jog",
                                 "main.jog",
                                 "plain.test.jog"};
#else
    const char *host_src[] = {"a.b.jog", "host.linux.jog", "host.posix.jog", "keep.windows.extra.jog", "main.jog"};
    const char *host_tests[] = {"a.b.jog",
                                 "a.b.test.jog",
                                 "extra.linux.test.jog",
                                 "extra.posix.test.jog",
                                 "host.linux.jog",
                                 "host.posix.jog",
                                 "keep.windows.extra.jog",
                                 "main.jog",
                                 "plain.test.jog"};
#endif
    const char *path = NULL;

    CHECK(open_harness(&h));
    path = pkg_fixture(&h, "peros");
    CHECK(path && jog_load(&h.program, path));
    CHECK(files_are(h.program.root, host_src, sizeof host_src / sizeof host_src[0]));
    CHECK(h.program.root->target_count == 0 && !h.program.root->target_sources);
    CHECK(ct_diag_error_count(h.diag) == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.include_tests = true;
    path = pkg_fixture(&h, "peros");
    CHECK(path && jog_load(&h.program, path));
    CHECK(files_are(h.program.root, host_tests, sizeof host_tests / sizeof host_tests[0]));
#if defined(_WIN32)
    CHECK(h.program.root->files[2]->is_test);
    CHECK(fn_at(h.program.root->files[2], 0) && strcmp(fn_at(h.program.root->files[2], 0), "windows_test") == 0);
#elif defined(__APPLE__)
    CHECK(h.program.root->files[2]->is_test);
    CHECK(fn_at(h.program.root->files[2], 0) && strcmp(fn_at(h.program.root->files[2], 0), "posix_test") == 0);
#else
    CHECK(h.program.root->files[2]->is_test);
    CHECK(fn_at(h.program.root->files[2], 0) && strcmp(fn_at(h.program.root->files[2], 0), "linux_test") == 0);
#endif
    CHECK(ct_diag_error_count(h.diag) == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.ptr_size = 4;
    path = pkg_fixture(&h, "peros");
    CHECK(path && jog_load(&h.program, path));
    CHECK(files_are(h.program.root, wasm_src, sizeof wasm_src / sizeof wasm_src[0]));
    CHECK(fn_at(h.program.root->files[1], 0) && strcmp(fn_at(h.program.root->files[1], 0), "from_wasm") == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.ptr_size = 4;
    h.program.options.include_tests = true;
    path = pkg_fixture(&h, "peros");
    CHECK(path && jog_load(&h.program, path));
    CHECK(files_are(h.program.root, wasm_tests, sizeof wasm_tests / sizeof wasm_tests[0]));
    CHECK(h.program.root->files[2]->is_test);
    CHECK(fn_at(h.program.root->files[2], 0) && strcmp(fn_at(h.program.root->files[2], 0), "wasm_test") == 0);
    close_harness(&h);
}

static int group_is(const JogPackage *pkg, const char *os, const char *const *names, size_t count) {
    size_t i = 0;
    const JogTargetSources *group = NULL;
    if (!pkg) {
        return 0;
    }
    for (i = 0; i < pkg->target_count; i++) {
        if (pkg->target_sources && pkg->target_sources[i].os && strcmp(pkg->target_sources[i].os, os) == 0) {
            group = &pkg->target_sources[i];
            break;
        }
    }
    if (!group || group->file_count != count) {
        fprintf(stderr, "target %s file count %zu, expected %zu\n", os, group ? group->file_count : 0, count);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *path = group->files && group->files[i] && group->files[i]->file.path ? group->files[i]->file.path
                                                                                          : "(null)";
        if (!group->files || !file_named(group->files[i], names[i])) {
            fprintf(stderr, "target %s file[%zu] is '%s', expected to end with '%s'\n", os, i, path, names[i]);
            return 0;
        }
    }
    return 1;
}

static JogSourceFile *group_file(const JogPackage *pkg, const char *os, const char *name) {
    size_t i = 0;
    size_t f = 0;
    if (!pkg) {
        return NULL;
    }
    for (i = 0; i < pkg->target_count; i++) {
        const JogTargetSources *group = pkg->target_sources ? &pkg->target_sources[i] : NULL;
        if (!group || !group->os || strcmp(group->os, os) != 0) {
            continue;
        }
        for (f = 0; f < group->file_count; f++) {
            if (group->files && file_named(group->files[f], name)) {
                return group->files[f];
            }
        }
    }
    return NULL;
}

/* targets lists every OS file for each name, on any host. The current
   target's files stay in pkg->files. posix is one parse shared by linux
   and macos. */
static void test_target_lists(void) {
    Harness h;
    const char *windows_files[] = {"a.windows.jog"};
    const char *linux_files[] = {"a.linux.jog", "a.posix.jog"};
    const char *macos_files[] = {"a.macos.jog", "a.posix.jog"};
    const char *wasm_files[] = {"a.wasm.jog"};
#if defined(_WIN32)
    const char *host_src[] = {"a.windows.jog", "main.jog", "note.b.jog"};
#elif defined(__APPLE__)
    const char *host_src[] = {"a.macos.jog", "a.posix.jog", "main.jog", "note.b.jog"};
#else
    const char *host_src[] = {"a.linux.jog", "a.posix.jog", "main.jog", "note.b.jog"};
#endif
    const char *path = NULL;
    JogSourceFile *posix_linux = NULL;
    JogSourceFile *posix_macos = NULL;

    CHECK(open_harness(&h));
    CHECK(load_manifest(&h, 200, "module m\ntargets windows linux macos wasm\n", 0));
    CHECK(h.program.root && h.program.root->target_count == 4);
    CHECK(h.program.root->targets && strcmp(h.program.root->targets[0], "windows") == 0);
    CHECK(strcmp(h.program.root->targets[3], "wasm") == 0);
    CHECK(h.program.root->target_sources && h.program.root->target_sources[0].file_count == 0);
    close_harness(&h);

    CHECK(open_harness(&h));
    h.program.options.ptr_size = 4;
    CHECK(!load_manifest(&h, 201, "module m\ntargets windows linux macos\n", 0));
    expect_fragment(h.diag, "module 'm' does not support target 'wasm'");
    close_harness(&h);

    CHECK(open_harness(&h));
    path = pkg_fixture(&h, "targets");
    CHECK(path && jog_load(&h.program, path));
    CHECK(ct_diag_error_count(h.diag) == 0);
    CHECK(files_are(h.program.root, host_src, sizeof host_src / sizeof host_src[0]));
    CHECK(h.program.root->target_count == 4);
    CHECK(group_is(h.program.root, "windows", windows_files, 1));
    CHECK(group_is(h.program.root, "linux", linux_files, 2));
    CHECK(group_is(h.program.root, "macos", macos_files, 2));
    CHECK(group_is(h.program.root, "wasm", wasm_files, 1));
    posix_linux = group_file(h.program.root, "linux", "a.posix.jog");
    posix_macos = group_file(h.program.root, "macos", "a.posix.jog");
    CHECK(posix_linux && posix_linux == posix_macos);
    close_harness(&h);
}

int main(void) {
    test_module_mapping();
    test_module_errors();
    test_module_links();
    test_manifest_v2();
    test_arguments();
    test_basic_entries();
    test_order();
    test_cycle();
    test_dedup();
    test_std_and_links();
    test_failures();
    test_manifest_errors();
    test_exe_relative_roots();
    test_per_os_files();
    test_target_lists();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
