#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "jogrt.h"
#include "jogrt_switch.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#else

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#endif

static int g_failures = 0;
static const char *g_self;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

/* UCRT abort() fast-fails. Elsewhere the child dies on SIGABRT, reported
   as 128 plus the signal, matching tools/proc.h. */
static int is_abort_status(int code) {
#if defined(_WIN32)
    return (unsigned)code == 0xC0000409u;
#else
    return code == 128 + SIGABRT;
#endif
}

static char *read_stripped(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = malloc((size_t)length + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)length, fp);
    fclose(fp);
    size_t written = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != '\r') {
            buf[written++] = buf[i];
        }
    }
    buf[written] = '\0';
    return buf;
}

#if defined(_WIN32)

static int append_chars(char *dst, size_t cap, size_t *used, char c, size_t count) {
    if (count > cap || *used > cap - count) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        dst[(*used)++] = c;
    }
    return 1;
}

static int append_char(char *dst, size_t cap, size_t *used, char c) {
    return append_chars(dst, cap, used, c, 1);
}

/* Quote one argument the way CommandLineToArgvW unquotes it. */
static int quote_arg(char *dst, size_t cap, size_t *used, const char *arg) {
    if (!append_char(dst, cap, used, '"')) {
        return 0;
    }
    size_t slashes = 0;
    for (const char *p = arg; *p; p++) {
        char c = *p;
        if (c == '\\') {
            slashes++;
            continue;
        }
        if (c == '"') {
            if (slashes > (SIZE_MAX - 1) / 2 || !append_chars(dst, cap, used, '\\', slashes * 2 + 1) ||
                !append_char(dst, cap, used, '"')) {
                return 0;
            }
            slashes = 0;
            continue;
        }
        if (!append_chars(dst, cap, used, '\\', slashes) || !append_char(dst, cap, used, c)) {
            return 0;
        }
        slashes = 0;
    }
    if (slashes > SIZE_MAX / 2 || !append_chars(dst, cap, used, '\\', slashes * 2) ||
        !append_char(dst, cap, used, '"')) {
        return 0;
    }
    return 1;
}

static char *build_command(const char *const *argv) {
    size_t cap = 32768;
    char *dst = malloc(cap);
    if (!dst) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; argv[i]; i++) {
        if (i > 0 && !append_char(dst, cap, &used, ' ')) {
            free(dst);
            return NULL;
        }
        if (!quote_arg(dst, cap, &used, argv[i])) {
            free(dst);
            return NULL;
        }
    }
    if (used > 32766 || used >= cap) {
        free(dst);
        return NULL;
    }
    dst[used] = '\0';
    return dst;
}

static HANDLE open_capture(const char *path) {
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE open_nul(void) {
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE inherit_std(DWORD id) {
    HANDLE handle = GetStdHandle(id);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        return handle;
    }
    return handle;
}

static void close_optional(HANDLE handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
}

static int run_child(const char *const *argv, const char *stderr_path, int *exit_code) {
    char *command = build_command(argv);
    if (!command) {
        return 0;
    }

    HANDLE err_h = open_capture(stderr_path);
    HANDLE in_owned = NULL;
    HANDLE out_owned = NULL;
    HANDLE in_h = inherit_std(STD_INPUT_HANDLE);
    HANDLE out_h = inherit_std(STD_OUTPUT_HANDLE);
    if (!in_h) {
        in_owned = open_nul();
        in_h = in_owned;
    }
    if (!out_h) {
        out_owned = open_nul();
        out_h = out_owned;
    }
    if (!in_h || !out_h || err_h == INVALID_HANDLE_VALUE) {
        close_optional(in_owned);
        close_optional(out_owned);
        close_optional(err_h);
        free(command);
        return 0;
    }

    STARTUPINFOA startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in_h;
    startup.hStdOutput = out_h;
    startup.hStdError = err_h;

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    BOOL created = CreateProcessA(argv[0], command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    free(command);
    close_optional(in_owned);
    close_optional(out_owned);
    CloseHandle(err_h);
    if (!created) {
        return 0;
    }

    DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    BOOL got = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!got) {
        return 0;
    }
    *exit_code = (int)status;
    return 1;
}

#else

static int move_high(int fd) {
    if (fd > STDERR_FILENO) {
        return fd;
    }
    int high = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int saved = errno;
    close(fd);
    if (high < 0) {
        errno = saved;
        return -1;
    }
    return high;
}

static int run_child(const char *const *argv, const char *stderr_path, int *exit_code) {
    int err_fd = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (err_fd < 0) {
        return 0;
    }
    err_fd = move_high(err_fd);
    if (err_fd < 0) {
        return 0;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(err_fd);
        return 0;
    }
    int failed = posix_spawn_file_actions_adddup2(&actions, err_fd, STDERR_FILENO) != 0 ||
                 posix_spawn_file_actions_addclose(&actions, err_fd) != 0;
    pid_t pid = 0;
    int spawned = -1;
    if (!failed) {
        spawned = posix_spawn(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    close(err_fd);
    if (failed || spawned != 0) {
        return 0;
    }

    int status = 0;
    for (;;) {
        pid_t got = waitpid(pid, &status, 0);
        if (got == pid) {
            break;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return 0;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
        return 1;
    }
    if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
        return 1;
    }
    return 0;
}

#endif

static int run_mode(const char *mode, const char *extra, int *code, char **text) {
    const char *err_path = "runtime_child_err.txt";
    remove(err_path);
    const char *argv[5];
    int n = 0;
    argv[n++] = g_self;
    argv[n++] = "--jogrt-child";
    argv[n++] = mode;
    if (extra) {
        argv[n++] = extra;
    }
    argv[n] = NULL;
    *text = NULL;
    if (!g_self || g_self[0] == '\0') {
        return 0;
    }
    int ok = run_child(argv, err_path, code);
    if (ok) {
        *text = read_stripped(err_path);
        if (!*text) {
            ok = 0;
        }
    }
    remove(err_path);
    return ok;
}

static void quiet_fn(void *env) {
    (void)env;
}

/* Allocates a coroutine stack and stores into its guard page. The handler
   panics with "stack overflow". The store runs on this thread's stack so
   the handler has room to print. */
static void touch_guard(void) {
    void *base = NULL;
    void *top = NULL;
    volatile char *guard;
    if (!jogrt_stack_alloc(4096, &base, &top) || !base) {
        fputs("no-stack\n", stderr);
        return;
    }
    guard = (volatile char *)base;
    guard[0] = 1;
}

static void boom_fn(void *env) {
    (void)env;
    jogrt_panic("boom", 4, "rt.c", 12);
}

static void nested_fn(void *env) {
    jogrt_closure inner;
    (void)env;
    inner.code = boom_fn;
    inner.env = NULL;
    if (jogrt_panic_call(&inner) != 1) {
        fputs("nested-miss\n", stderr);
        return;
    }
    jogrt_panic("outer", 5, "rt.c", 9);
}

static int child_main(int argc, char **argv) {
    if (argc < 3 || !argv[2]) {
        return 2;
    }
    const char *mode = argv[2];
    if (strcmp(mode, "ok") == 0) {
        if (fputs("ready\n", stderr) == EOF || fflush(stderr) != 0) {
            return 2;
        }
        return 0;
    }
    if (strcmp(mode, "exit") == 0) {
        if (argc < 4 || !argv[3]) {
            return 2;
        }
        char *end = NULL;
        long value = strtol(argv[3], &end, 10);
        if (!end || end == argv[3] || *end != '\0' || value < 0 || value > 255) {
            return 2;
        }
        jogrt_exit((int)value);
    }
    if (strcmp(mode, "panic") == 0) {
        jogrt_panic("boom", 4, "rt.c", 12);
    }
    if (strcmp(mode, "panic-empty") == 0) {
        jogrt_panic("ignored", 0, "rt.c", 4);
    }
    if (strcmp(mode, "panic-null") == 0) {
        jogrt_panic(NULL, 4, "rt.c", 9);
    }
    if (strcmp(mode, "panic-prefix") == 0) {
        jogrt_panic("abcdef", 3, "rt.c", 8);
    }
    if (strcmp(mode, "panic-nl") == 0) {
        jogrt_panic("a\nb", 3, "rt.c", 1);
    }
    if (strcmp(mode, "panic-nofile") == 0) {
        jogrt_panic("m", 1, NULL, 3);
    }
    if (strcmp(mode, "bounds") == 0) {
        jogrt_panic_bounds(3, 2, "rt.c", 15);
    }
    if (strcmp(mode, "bounds-big") == 0) {
        jogrt_panic_bounds(1000, 20, "main.jog", 40);
    }
    if (strcmp(mode, "bounds-zero") == 0) {
        jogrt_panic_bounds(0, 0, "rt.c", 0);
    }
    if (strcmp(mode, "overflow") == 0) {
        jogrt_panic_overflow("rt.c", 21);
    }
    if (strcmp(mode, "divzero") == 0) {
        jogrt_panic_divide_by_zero("rt.c", 22);
    }
    if (strcmp(mode, "null") == 0) {
        jogrt_panic_null("rt.c", 23);
    }
    if (strcmp(mode, "convert") == 0) {
        jogrt_panic_convert("rt.c", 24);
    }
    if (strcmp(mode, "unreachable") == 0) {
        jogrt_panic_unreachable("rt.c", 25);
    }
    if (strcmp(mode, "catch") == 0) {
        jogrt_closure body;
        body.code = boom_fn;
        body.env = NULL;
        if (jogrt_panic_call(&body) != 1) {
            return 2;
        }
        body.code = quiet_fn;
        if (jogrt_panic_call(&body) != 0) {
            return 2;
        }
        body.code = nested_fn;
        if (jogrt_panic_call(&body) != 1) {
            return 2;
        }
        if (fputs("caught\n", stderr) == EOF) {
            return 2;
        }
        return 0;
    }
    if (strcmp(mode, "stack") == 0) {
        touch_guard();
        fputs("stack-returned\n", stderr);
        return 2;
    }
    if (strcmp(mode, "uncatch") == 0) {
        jogrt_closure body;
        body.code = quiet_fn;
        body.env = NULL;
        if (jogrt_panic_call(&body) != 0) {
            return 2;
        }
        jogrt_panic("after", 5, "rt.c", 7);
    }
    return 2;
}

static void expect_panic(const char *mode, const char *expected) {
    int code = -1;
    char *got = NULL;
    int ran = run_mode(mode, NULL, &code, &got);
    CHECK(ran);
    if (!ran) {
        fprintf(stderr, "  %s did not start\n", mode);
        free(got);
        return;
    }
    if (!is_abort_status(code)) {
        fprintf(stderr, "  %s status %d (0x%x)\n", mode, code, (unsigned)code);
    }
    CHECK(is_abort_status(code));
    int same = got && strcmp(got, expected) == 0;
    if (!same) {
        fprintf(stderr, "  %s stderr mismatch\n got:\n%s\n want:\n%s\n", mode, got ? got : "(null)", expected);
    }
    CHECK(same);
    free(got);
}

static void expect_exit(const char *extra, int expected) {
    int code = -1;
    char *got = NULL;
    int ran = run_mode("exit", extra, &code, &got);
    CHECK(ran);
    if (!ran) {
        fprintf(stderr, "  exit %s did not start\n", extra);
        free(got);
        return;
    }
    if (code != expected) {
        fprintf(stderr, "  exit %s status %d (0x%x)\n", extra, code, (unsigned)code);
    }
    CHECK(code == expected);
    int same = got && got[0] == '\0';
    if (!same) {
        fprintf(stderr, "  exit %s stderr:\n%s\n", extra, got ? got : "(null)");
    }
    CHECK(same);
    free(got);
}

static void test_capture(void) {
    int code = -1;
    char *got = NULL;
    int ran = run_mode("ok", NULL, &code, &got);
    CHECK(ran);
    CHECK(ran && code == 0);
    int same = got && strcmp(got, "ready\n") == 0;
    if (!same) {
        fprintf(stderr, "  capture stderr:\n%s\n", got ? got : "(null)");
    }
    CHECK(same);
    free(got);
}

static void expect_catch(void) {
    int code = -1;
    char *got = NULL;
    int ran = run_mode("catch", NULL, &code, &got);
    CHECK(ran);
    if (!ran) {
        fprintf(stderr, "  catch did not start\n");
        free(got);
        return;
    }
    if (code != 0) {
        fprintf(stderr, "  catch status %d (0x%x)\n%s\n", code, (unsigned)code, got ? got : "");
    }
    CHECK(code == 0);
    int saw = got && strstr(got, "panic: boom\n") && strstr(got, "panic: outer\n") && strstr(got, "caught\n");
    if (!saw) {
        fprintf(stderr, "  catch stderr:\n%s\n", got ? got : "(null)");
    }
    CHECK(saw);
    free(got);
}

static void test_panics(void) {
    expect_panic("panic", "panic: boom\n  at rt.c:12\n");
    expect_panic("panic-empty", "panic: \n  at rt.c:4\n");
    expect_panic("panic-null", "panic: \n  at rt.c:9\n");
    expect_panic("panic-prefix", "panic: abc\n  at rt.c:8\n");
    expect_panic("panic-nl", "panic: a\nb\n  at rt.c:1\n");
    expect_panic("panic-nofile", "panic: m\n  at :3\n");
    expect_panic("bounds", "panic: index 3 out of bounds for length 2\n  at rt.c:15\n");
    expect_panic("bounds-big", "panic: index 1000 out of bounds for length 20\n  at main.jog:40\n");
    expect_panic("bounds-zero", "panic: index 0 out of bounds for length 0\n  at rt.c:0\n");
    expect_panic("overflow", "panic: integer overflow\n  at rt.c:21\n");
    expect_panic("divzero", "panic: division by zero\n  at rt.c:22\n");
    expect_panic("null", "panic: null pointer\n  at rt.c:23\n");
    expect_panic("convert", "panic: value out of range\n  at rt.c:24\n");
    expect_panic("unreachable", "panic: unreachable\n  at rt.c:25\n");
    expect_catch();
    expect_panic("uncatch", "panic: after\n  at rt.c:7\n");
}

static void test_stack_overflow(void) {
    int code = -1;
    char *got = NULL;
    int ran = run_mode("stack", NULL, &code, &got);
    CHECK(ran);
    if (!ran) {
        fprintf(stderr, "  stack did not start\n");
        free(got);
        return;
    }
    if (!is_abort_status(code)) {
        fprintf(stderr, "  stack status %d (0x%x)\n", code, (unsigned)code);
    }
    CHECK(is_abort_status(code));
    int saw = got && strstr(got, "panic: stack overflow\n");
    if (!saw) {
        fprintf(stderr, "  stack stderr:\n%s\n", got ? got : "(null)");
    }
    CHECK(saw);
    free(got);
}

static void test_exit(void) {
    expect_exit("0", 0);
    expect_exit("1", 1);
    expect_exit("42", 42);
    expect_exit("255", 255);
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "--jogrt-child") == 0) {
        return child_main(argc, argv);
    }
    g_self = argc > 0 ? argv[0] : NULL;
    test_capture();
    test_panics();
    test_stack_overflow();
    test_exit();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
