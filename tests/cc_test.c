#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "phases.h"

#include "fs.h"
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
#define CC_EXE_SUFFIX ".exe"
#else
#define CC_EXE_SUFFIX ""
#endif

static int g_failures = 0;
static unsigned g_seq = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

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
    return _putenv_s("JOG_CC", value) == 0;
#else
    if (!value || value[0] == '\0') {
        unsetenv("JOG_CC");
        return 1;
    }
    return setenv("JOG_CC", value, 1) == 0;
#endif
}

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    long length = 0;
    char *buf = NULL;
    size_t got = 0;
    size_t written = 0;
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = malloc((size_t)length + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    got = fread(buf, 1, (size_t)length, fp);
    fclose(fp);
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\r') {
            buf[written++] = buf[i];
        }
    }
    buf[written] = '\0';
    return buf;
}

static const char *temp_dir(CtArena *arena) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(buf), buf);
    if (n == 0 || n >= sizeof(buf)) {
        return ct_arena_strdup(arena, ".");
    }
    if (buf[n - 1] == '\\' || buf[n - 1] == '/') {
        buf[n - 1] = '\0';
    }
    return ct_arena_strdup(arena, buf);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || dir[0] == '\0') {
        dir = "/tmp";
    }
    (void)arena;
    return dir;
#endif
}

static char *temp_path(CtArena *arena, const char *suffix) {
    char name[96];
    const char *dir = temp_dir(arena);
    unsigned pid = 0;
    if (!dir) {
        return NULL;
    }
#if defined(_WIN32)
    pid = GetCurrentProcessId();
#else
    pid = (unsigned)getpid();
#endif
    snprintf(name, sizeof(name), "jog-cc-%u-%u%s", pid, g_seq++, suffix);
    return ct_path_join(arena, dir, name);
}

static int capture_stderr(const char *path, int *saved) {
    FILE *file = NULL;
#if defined(_WIN32)
    int fd = _dup(_fileno(stderr));
#else
    int fd = dup(fileno(stderr));
#endif
    if (fd < 0) {
        return 0;
    }
    file = freopen(path, "wb", stderr);
    if (!file) {
#if defined(_WIN32)
        _close(fd);
#else
        close(fd);
#endif
        return 0;
    }
    *saved = fd;
    return 1;
}

static void restore_stderr(int saved) {
    fflush(stderr);
#if defined(_WIN32)
    _dup2(saved, _fileno(stderr));
    _close(saved);
#else
    dup2(saved, fileno(stderr));
    close(saved);
#endif
    clearerr(stderr);
}

static void expect_has(const char *text, const char *needle, const char *file, int line) {
    if (!text || !strstr(text, needle)) {
        fprintf(stderr, "FAIL %s:%d: missing '%s' in:\n%s\n", file, line, needle, text ? text : "(null)");
        g_failures++;
    }
}

static void expect_lacks(const char *text, const char *needle, const char *file, int line) {
    if (text && needle && strstr(text, needle)) {
        fprintf(stderr, "FAIL %s:%d: unexpected '%s' in:\n%s\n", file, line, needle, text);
        g_failures++;
    }
}

static void fill_request(JogCcRequest *request, const char *exe, JogBuildMode mode, const char *const *libs,
    size_t lib_count) {
    memset(request, 0, sizeof(*request));
    request->c_path = CT_SOURCE_DIR "/tests/cc/use_jogrt.c";
    request->exe_path = exe;
    request->runtime_root = CT_SOURCE_DIR "/runtime";
    request->mode = mode;
    request->extra_libs = libs;
    request->extra_lib_count = lib_count;
    request->verbose = true;
}

/* Compiles use_jogrt.c. On success, *stderr_text is the compiler command
   plus any compiler diagnostics. Caller frees *stderr_text. */
static int compile_program(CtArena *arena, const char *exe, JogBuildMode mode, const char *const *libs, size_t lib_count,
    char **stderr_text, char **diag_text) {
    CtDiag *diag = ct_diag_create(arena);
    JogCcRequest request;
    char *err_path = temp_path(arena, ".err");
    char *diag_path = temp_path(arena, ".diag");
    int saved = -1;
    int captured = 0;
    int built = 0;
    FILE *diag_file = NULL;
    fill_request(&request, exe, mode, libs, lib_count);
    CHECK(diag && err_path && diag_path);
    if (!diag || !err_path || !diag_path) {
        return 0;
    }
    captured = capture_stderr(err_path, &saved);
    if (captured) {
        built = jog_cc_build(&request, diag);
        restore_stderr(saved);
    }
    CHECK(captured);
    if (stderr_text) {
        *stderr_text = read_stripped(err_path);
    }
    diag_file = fopen(diag_path, "wb");
    if (diag_file) {
        ct_diag_print(diag, diag_file);
        fclose(diag_file);
    }
    if (diag_text) {
        *diag_text = read_stripped(diag_path);
    }
    remove(err_path);
    remove(diag_path);
    return built;
}

static void expect_run(const char *exe, const char *expected, const char *file, int line) {
    CtArena *arena = ct_arena_create();
    char *out_path = arena ? temp_path(arena, ".out") : NULL;
    const char *argv[2];
    int code = -1;
    int ran = 0;
    char *text = NULL;
    argv[0] = exe;
    argv[1] = NULL;
    if (!out_path) {
        fprintf(stderr, "FAIL %s:%d: no temp path\n", file, line);
        g_failures++;
        ct_arena_destroy(arena);
        return;
    }
    ran = ct_proc_run(argv, out_path, NULL, &code);
    text = read_stripped(out_path);
    if (!ran || code != 0 || !text || strcmp(text, expected) != 0) {
        fprintf(stderr, "FAIL %s:%d: running %s\n  ran %d code %d\n  got '%s'\n  expected '%s'\n", file, line, exe, ran, code,
            text ? text : "(null)", expected);
        g_failures++;
    }
    free(text);
    remove(out_path);
    ct_arena_destroy(arena);
}

#define EXPECT_RUN(exe, expected) expect_run((exe), (expected), __FILE__, __LINE__)

static void expect_clean(const char *stderr_text, const char *file, int line) {
    expect_lacks(stderr_text, "warning:", file, line);
    expect_lacks(stderr_text, "error:", file, line);
}

static void test_rejects(void) {
    CtArena *arena = ct_arena_create();
    CtDiag *diag = arena ? ct_diag_create(arena) : NULL;
    JogCcRequest request;
    CHECK(arena && diag);
    CHECK(!jog_cc_build(NULL, diag));
    CHECK(diag && ct_diag_error_count(diag) == 1);

    memset(&request, 0, sizeof(request));
    request.c_path = CT_SOURCE_DIR "/tests/cc/use_jogrt.c";
    request.exe_path = "";
    request.runtime_root = CT_SOURCE_DIR "/runtime";
    request.mode = JOG_BUILD_DEBUG;
    CHECK(!jog_cc_build(&request, diag));

    request.exe_path = "jog-cc-unused.exe";
    request.mode = (JogBuildMode)99;
    CHECK(!jog_cc_build(&request, diag));

    request.mode = JOG_BUILD_DEBUG;
    request.c_path = CT_SOURCE_DIR "/tests/cc/missing-use-jogrt.c";
    CHECK(!jog_cc_build(&request, diag));
    ct_arena_destroy(arena);
}

static void test_mode(JogBuildMode mode, const char *stdout_text, const char *flag, const char *absent) {
    CtArena *arena = ct_arena_create();
    char *exe = arena ? temp_path(arena, CC_EXE_SUFFIX) : NULL;
    char *stderr_text = NULL;
    char *diag_text = NULL;
    int built = 0;
    CHECK(exe != NULL);
    if (!exe) {
        ct_arena_destroy(arena);
        return;
    }
    built = compile_program(arena, exe, mode, NULL, 0, &stderr_text, &diag_text);
    if (!built) {
        fprintf(stderr, "build failed for mode %d\n%s\n%s\n", (int)mode, diag_text ? diag_text : "",
            stderr_text ? stderr_text : "");
        g_failures++;
    } else {
        expect_has(stderr_text, " -std=gnu17 ", __FILE__, __LINE__);
        expect_has(stderr_text, flag, __FILE__, __LINE__);
        expect_has(stderr_text, "jogrt.c", __FILE__, __LINE__);
        expect_has(stderr_text, "jogrt_main.c", __FILE__, __LINE__);
        expect_has(stderr_text, "-Wno-incompatible-library-redeclaration", __FILE__, __LINE__);
        expect_has(stderr_text, "-Wno-builtin-requires-header", __FILE__, __LINE__);
        if (mode == JOG_BUILD_DEBUG) {
            expect_has(stderr_text, " -g ", __FILE__, __LINE__);
        } else {
            expect_lacks(stderr_text, " -g ", __FILE__, __LINE__);
        }
        expect_lacks(stderr_text, absent, __FILE__, __LINE__);
        expect_clean(stderr_text, __FILE__, __LINE__);
        EXPECT_RUN(exe, stdout_text);
    }
    free(stderr_text);
    free(diag_text);
    remove(exe);
    ct_arena_destroy(arena);
}

static void test_gcc_override(void) {
    CtArena *arena = ct_arena_create();
    char *exe = arena ? temp_path(arena, CC_EXE_SUFFIX) : NULL;
    char *stderr_text = NULL;
    char *diag_text = NULL;
    int built = 0;
    CHECK(env_set("gcc"));
    CHECK(exe != NULL);
    if (exe) {
        built = compile_program(arena, exe, JOG_BUILD_DEBUG, NULL, 0, &stderr_text, &diag_text);
        if (!built) {
            fprintf(stderr, "JOG_CC=gcc build failed\n%s\n%s\n", diag_text ? diag_text : "", stderr_text ? stderr_text : "");
            g_failures++;
        } else {
            expect_has(stderr_text, " -O0 ", __FILE__, __LINE__);
            expect_has(stderr_text, " -g ", __FILE__, __LINE__);
            expect_has(stderr_text, "-Wno-builtin-declaration-mismatch", __FILE__, __LINE__);
            expect_lacks(stderr_text, "-Wno-incompatible-library-redeclaration", __FILE__, __LINE__);
            expect_lacks(stderr_text, "-Wno-builtin-requires-header", __FILE__, __LINE__);
            expect_clean(stderr_text, __FILE__, __LINE__);
            EXPECT_RUN(exe, "jogrt gcc dbg\n");
        }
        remove(exe);
    }
    free(stderr_text);
    free(diag_text);
    ct_arena_destroy(arena);
    CHECK(env_set(""));
}

static void test_missing_compiler(void) {
    CtArena *arena = ct_arena_create();
    char *exe = arena ? temp_path(arena, CC_EXE_SUFFIX) : NULL;
    char *diag_text = NULL;
    char *stderr_text = NULL;
    int built = 0;
    CHECK(env_set("jog-no-such-cc"));
    if (exe) {
        built = compile_program(arena, exe, JOG_BUILD_DEBUG, NULL, 0, &stderr_text, &diag_text);
        CHECK(!built);
        expect_has(diag_text, "JOG_CC", __FILE__, __LINE__);
        expect_lacks(stderr_text, "jog cc:", __FILE__, __LINE__);
        remove(exe);
    }
    free(stderr_text);
    free(diag_text);
    ct_arena_destroy(arena);

    arena = ct_arena_create();
    exe = arena ? temp_path(arena, CC_EXE_SUFFIX) : NULL;
    diag_text = NULL;
    stderr_text = NULL;
    CHECK(env_set("no/such/jog-cc.exe"));
    if (exe) {
        built = compile_program(arena, exe, JOG_BUILD_DEBUG, NULL, 0, &stderr_text, &diag_text);
        CHECK(!built);
        expect_has(diag_text, "JOG_CC", __FILE__, __LINE__);
        remove(exe);
    }
    free(stderr_text);
    free(diag_text);
    ct_arena_destroy(arena);
    CHECK(env_set(""));
}

static void test_missing_lib(void) {
    CtArena *arena = ct_arena_create();
    char *exe = arena ? temp_path(arena, CC_EXE_SUFFIX) : NULL;
    char *stderr_text = NULL;
    char *diag_text = NULL;
    const char *libs[] = {"jog_cc_missing"};
    int built = 0;
    CHECK(exe != NULL);
    if (exe) {
        built = compile_program(arena, exe, JOG_BUILD_RELEASE, libs, 1, &stderr_text, &diag_text);
        CHECK(!built);
        expect_has(stderr_text, "-ljog_cc_missing", __FILE__, __LINE__);
        expect_has(stderr_text, " -O2 ", __FILE__, __LINE__);
        expect_has(diag_text, "exited with status", __FILE__, __LINE__);
        remove(exe);
    }
    free(stderr_text);
    free(diag_text);
    ct_arena_destroy(arena);
}

int main(void) {
    const char *existing = getenv("JOG_CC");
    char *saved = NULL;
    if (existing && existing[0] != '\0') {
        saved = dup_cstr(existing);
        CHECK(saved != NULL);
    }
    CHECK(env_set(""));

    test_rejects();
    test_mode(JOG_BUILD_DEBUG, "jogrt clang dbg\n", " -O0 ", " -O2 ");
    test_mode(JOG_BUILD_RELEASE, "jogrt clang opt\n", " -O2 ", " -O0 ");
    test_mode(JOG_BUILD_RELEASE_FAST, "jogrt clang opt\n", " -O2 ", " -O0 ");
    test_gcc_override();
    test_missing_compiler();
    test_missing_lib();

    if (saved) {
        env_set(saved);
        free(saved);
    } else {
        env_set("");
    }
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
