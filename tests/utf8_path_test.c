#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "diag.h"
#include "fs.h"
#include "proc.h"

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

/* Paths and arguments outside ASCII. A module folder named nonascii_é
   (UTF-8 in this file) with a file named ü.jog is checked by jog.exe, and
   a program prints an argument outside ASCII. On Windows that needs the
   UTF-16 command line in jog and in the runtime, and the W file APIs. */

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

#define E_ACUTE "\xc3\xa9"
#define U_UMLAUT "\xc3\xbc"

static const char *jog_executable(CtArena *arena) {
    const char *self = NULL;
    size_t len = 0;
    char *dir = NULL;
    const char *jog = NULL;
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD got = GetModuleFileNameW(NULL, buf, 32768);
    if (got == 0 || got >= 32768) {
        return NULL;
    }
    self = ct_fs_narrow(arena, buf);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    self = ct_arena_strdup(arena, buf);
#else
    char buf[4096];
    ssize_t got = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (got < 0) {
        return NULL;
    }
    buf[got] = '\0';
    self = ct_arena_strdup(arena, buf);
#endif
    if (!self) {
        return NULL;
    }
    len = strlen(self);
    while (len > 0 && self[len - 1] != '/' && self[len - 1] != '\\') {
        len--;
    }
    if (len == 0) {
        return NULL;
    }
    dir = ct_arena_alloc(arena, len, 1);
    if (!dir) {
        return NULL;
    }
    memcpy(dir, self, len - 1);
    dir[len - 1] = '\0';
#if defined(_WIN32)
    jog = ct_path_join(arena, dir, "jog.exe");
#else
    jog = ct_path_join(arena, dir, "jog");
#endif
    return jog && ct_fs_is_file(jog) ? jog : NULL;
}

static const char *temp_root(CtArena *arena) {
    char name[64];
    const char *base = NULL;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (n == 0 || n > MAX_PATH) {
        return NULL;
    }
    base = ct_fs_narrow(arena, buf);
    snprintf(name, sizeof name, "jog-utf8-%lu", (unsigned long)GetCurrentProcessId());
#else
    base = getenv("TMPDIR");
    if (!base || base[0] == '\0') {
        base = "/tmp";
    }
    snprintf(name, sizeof name, "jog-utf8-%lu", (unsigned long)getpid());
#endif
    return base ? ct_path_join(arena, base, name) : NULL;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = ct_fs_fopen(path, "wb");
    size_t len = strlen(text);
    if (!fp) {
        return 0;
    }
    if (fwrite(text, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    return fclose(fp) == 0;
}

static char *read_text(const char *path) {
    FILE *fp = ct_fs_fopen(path, "rb");
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int ch = 0;
    if (!fp) {
        return NULL;
    }
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == '\r') {
            continue;
        }
        if (len + 2 > cap) {
            size_t next = cap == 0 ? 256 : cap * 2;
            char *grown = realloc(buf, next);
            if (!grown) {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = grown;
            cap = next;
        }
        buf[len++] = (char)ch;
    }
    fclose(fp);
    if (!buf) {
        buf = malloc(1);
    }
    if (buf) {
        buf[len] = '\0';
    }
    return buf;
}

/* Every file in dir, then dir. One level of folders below it. */
static void remove_tree(CtArena *arena, const char *dir) {
    CtDa names;
    size_t i = 0;
    for (int kind = 0; kind < 2; kind++) {
        if (!ct_da_init(&names, sizeof(const char *))) {
            return;
        }
        if (ct_fs_list(arena, NULL, dir, kind == 0 ? CT_FS_FILES : CT_FS_DIRS, NULL, &names)) {
            for (i = 0; i < names.count; i++) {
                const char *path = ct_path_join(arena, dir, *(const char **)ct_da_at(&names, i));
                if (kind == 0) {
                    ct_fs_remove(path);
                } else {
                    remove_tree(arena, path);
                }
            }
        }
        ct_da_free(&names);
    }
#if defined(_WIN32)
    {
        wchar_t *wide = ct_fs_widen(dir);
        if (wide) {
            RemoveDirectoryW(wide);
            free(wide);
        }
    }
#else
    rmdir(dir);
#endif
}

static int run(const char *const *argv, const char *out_path, const char *err_path, int *code) {
    return ct_proc_run(argv, out_path, err_path, code) ? 1 : 0;
}

static void test_check(CtArena *arena, const char *jog, const char *work) {
    const char *good = ct_path_join(arena, work, "nonascii_" E_ACUTE);
    const char *bad = ct_path_join(arena, work, "bad_" E_ACUTE);
    const char *out_path = ct_path_join(arena, work, "check.out");
    const char *err_path = ct_path_join(arena, work, "check.err");
    const char *good_file = good ? ct_path_join(arena, good, U_UMLAUT ".jog") : NULL;
    char *err_text = NULL;
    int code = -1;
    CHECK(good && bad && out_path && err_path && good_file);
    if (!good || !bad || !out_path || !err_path || !good_file) {
        return;
    }
    CHECK(ct_fs_mkdir(good));
    CHECK(ct_fs_is_dir(good));
    CHECK(write_text(ct_path_join(arena, good, "jog.mod"), "module nonascii\n"));
    CHECK(write_text(ct_path_join(arena, good, "a.jog"), "fn main() {\n    helper()\n}\n"));
    CHECK(write_text(good_file, "fn helper() {\n}\n"));
    CHECK(ct_fs_is_file(good_file));

    {
        const char *argv[] = {jog, "check", good, NULL};
        CHECK(run(argv, out_path, err_path, &code));
        err_text = read_text(err_path);
        if (code != 0) {
            fprintf(stderr, "FAIL jog check %s exited %d\n%s\n", good, code, err_text ? err_text : "");
        }
        CHECK(code == 0);
        free(err_text);
    }
    {
        const char *argv[] = {jog, "check", good_file, NULL};
        code = -1;
        CHECK(run(argv, out_path, err_path, &code));
        CHECK(code == 0);
    }

    CHECK(ct_fs_mkdir(bad));
    CHECK(write_text(ct_path_join(arena, bad, "jog.mod"), "module bad\n"));
    CHECK(write_text(ct_path_join(arena, bad, "main.jog"), "fn main() {\n    val n = missing\n}\n"));
    {
        const char *argv[] = {jog, "check", bad, NULL};
        code = -1;
        CHECK(run(argv, out_path, err_path, &code));
        err_text = read_text(err_path);
        CHECK(code != 0);
        CHECK(err_text && strstr(err_text, "bad_" E_ACUTE) != NULL);
        CHECK(err_text && strstr(err_text, "undefined name 'missing'") != NULL);
        free(err_text);
    }
    ct_fs_remove(out_path);
    ct_fs_remove(err_path);
}

/* The runtime's argv is UTF-8 too. The module folder is ASCII so the C
   compiler does not need to accept a non-ASCII path. */
static void test_run_argv(CtArena *arena, const char *jog, const char *work) {
    const char *mod = ct_path_join(arena, work, "argv");
    const char *out_path = ct_path_join(arena, work, "run.out");
    const char *err_path = ct_path_join(arena, work, "run.err");
    char *out_text = NULL;
    char *err_text = NULL;
    int code = -1;
    if (!mod || !out_path || !err_path) {
        CHECK(0);
        return;
    }
    CHECK(ct_fs_mkdir(mod));
    CHECK(write_text(ct_path_join(arena, mod, "jog.mod"), "module argv\n"));
    CHECK(write_text(ct_path_join(arena, mod, "main.jog"),
                     "import std.io\nimport std.os\n\nfn main() {\n    io.println(os.argv(1))\n}\n"));
    {
        const char *argv[] = {jog, "run", mod, "--", "caf" E_ACUTE " " U_UMLAUT, NULL};
        CHECK(run(argv, out_path, err_path, &code));
    }
    out_text = read_text(out_path);
    err_text = read_text(err_path);
    if (code != 0 || !out_text || strcmp(out_text, "caf" E_ACUTE " " U_UMLAUT "\n") != 0) {
        fprintf(stderr, "FAIL jog run argv exited %d\nstdout: %s\nstderr: %s\n", code, out_text ? out_text : "",
                err_text ? err_text : "");
        CHECK(0);
    }
    free(out_text);
    free(err_text);
    ct_fs_remove(out_path);
    ct_fs_remove(err_path);
}

static int set_jog_root(const char *value) {
#if defined(_WIN32)
    return _putenv_s("JOG_ROOT", value) == 0;
#else
    return setenv("JOG_ROOT", value, 1) == 0;
#endif
}

int main(void) {
    CtArena *arena = ct_arena_create();
    const char *jog = arena ? jog_executable(arena) : NULL;
    const char *work = arena ? temp_root(arena) : NULL;
    CHECK(arena && jog && work);
    if (!arena || !jog || !work) {
        fprintf(stderr, "FAIL: cannot find jog or a temp folder\n");
        ct_arena_destroy(arena);
        return 1;
    }
    CHECK(set_jog_root(CT_SOURCE_DIR));
    remove_tree(arena, work);
    CHECK(ct_fs_mkdir(work));
    test_check(arena, jog, work);
    test_run_argv(arena, jog, work);
    remove_tree(arena, work);
    CHECK(!ct_fs_is_dir(work));
    ct_arena_destroy(arena);
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
