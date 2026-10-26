#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "jogrt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

static int g_failures = 0;

static void check(int condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static int stream_binary(int fd, int *old_mode) {
#if defined(_WIN32)
    int mode = _setmode(fd, O_BINARY);
    if (mode < 0) {
        return 0;
    }
    *old_mode = mode;
    return 1;
#else
    (void)fd;
    *old_mode = 0;
    return 1;
#endif
}

static void stream_restore_mode(int fd, int old_mode) {
#if defined(_WIN32)
    if (_setmode(fd, old_mode) < 0) {
        return;
    }
#else
    (void)fd;
    (void)old_mode;
#endif
}

static char *read_raw(const char *path, size_t *out_len) {
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
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Redirect fd 1 or 2 at a binary file for one jogrt_write, then put the
   stream back. *bytes is the file contents, including embedded NULs. */
static int capture_write(int fd, const void *data, size_t len, int *wrote, char **bytes, size_t *nbytes) {
    FILE *stream = fd == 1 ? stdout : stderr;
    const char *path = "runtime_write_out.txt";
    *bytes = NULL;
    *nbytes = 0;
    *wrote = 0;
    remove(path);
    if (fflush(stream) != 0) {
        return 0;
    }

    int raw = fileno(stream);
    int saved = dup(raw);
    if (saved < 0) {
        return 0;
    }
    int old_mode = 0;
    if (!stream_binary(raw, &old_mode)) {
        close(saved);
        return 0;
    }

    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (file < 0 || dup2(file, raw) < 0) {
        if (file >= 0) {
            close(file);
        }
        stream_restore_mode(raw, old_mode);
        dup2(saved, raw);
        close(saved);
        clearerr(stream);
        return 0;
    }
    close(file);
    clearerr(stream);

    *wrote = jogrt_write(fd, data, len) ? 1 : 0;
    fflush(stream);
    dup2(saved, raw);
    close(saved);
    stream_restore_mode(raw, old_mode);
    clearerr(stream);

    *bytes = read_raw(path, nbytes);
    remove(path);
    return *bytes != NULL;
}

static void expect_write(int fd, const void *data, size_t len) {
    int wrote = 0;
    char *bytes = NULL;
    size_t nbytes = 0;
    int captured = capture_write(fd, data, len, &wrote, &bytes, &nbytes);
    CHECK(captured);
    CHECK(wrote);
    CHECK(captured && nbytes == len && memcmp(bytes, data, len) == 0);
    free(bytes);
}

static void test_shims(void) {
    CHECK(jogrt_stdin() == (void *)stdin);
    CHECK(jogrt_stdout() == (void *)stdout);
    CHECK(jogrt_stderr() == (void *)stderr);

    errno = 0;
    CHECK(jogrt_errno() == 0);
    errno = ERANGE;
    CHECK(jogrt_errno() == ERANGE);
    errno = 0;
}

static void test_write(void) {
    CHECK(jogrt_write(1, NULL, 0));
    CHECK(jogrt_write(2, "x", 0));
    CHECK(!jogrt_write(0, "x", 1));
    CHECK(!jogrt_write(3, "x", 1));
    CHECK(!jogrt_write(-1, "x", 1));
    CHECK(!jogrt_write(1, NULL, 4));
    CHECK(!jogrt_write(2, NULL, 1));

    const char hello[] = "hello";
    expect_write(1, hello, 5);
    expect_write(2, hello, 5);

    const char mixed[] = {'a', '\n', '\0', 'z', '\r'};
    expect_write(1, mixed, sizeof(mixed));

    unsigned char block[3000];
    for (size_t i = 0; i < sizeof(block); i++) {
        block[i] = (unsigned char)(i * 17u);
    }
    expect_write(2, block, sizeof(block));

    const char *path = "runtime_write_ro.txt";
    FILE *created = fopen(path, "wb");
    CHECK(created != NULL);
    if (!created) {
        return;
    }
    CHECK(fputc('x', created) != EOF);
    CHECK(fclose(created) == 0);

    CHECK(fflush(stdout) == 0);
    int raw = fileno(stdout);
    int saved = dup(raw);
    int file = open(path, O_RDONLY | O_BINARY);
    CHECK(saved >= 0 && file >= 0);
    if (saved < 0 || file < 0 || dup2(file, raw) < 0) {
        if (file >= 0) {
            close(file);
        }
        if (saved >= 0) {
            close(saved);
        }
        remove(path);
        return;
    }
    close(file);
    clearerr(stdout);
    CHECK(!jogrt_write(1, "y", 1));
    fflush(stdout);
    dup2(saved, raw);
    close(saved);
    clearerr(stdout);
    remove(path);
}

/* Child mode: one jogrt_stdin_read of up to 64 bytes. The exit code is the
   count, 200 for an error, or 201 when the bytes are not a prefix of
   "abcdef". */
static int stdin_child(void) {
    unsigned char buf[64];
    intptr_t n = jogrt_stdin_read(buf, sizeof buf);
    if (n < 0) {
        return 200;
    }
    if (n > 6 || memcmp(buf, "abcdef", (size_t)n) != 0) {
        return 201;
    }
    return (int)n;
}

enum { STDIN_CHILD_WAIT_MS = 10000 };

/* Starts this test again with --stdin-child and a pipe on its stdin, writes
   len bytes, and closes the write end only when close_after is set. The
   child must exit within STDIN_CHILD_WAIT_MS with the pipe still open, so a
   read that waits for its whole buffer is killed and gives -1. */
#if defined(_WIN32)
static int run_stdin_child(const char *data, size_t len, int close_after) {
    char self[MAX_PATH];
    char command[MAX_PATH + 32];
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL;
    HANDLE wr = NULL;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    DWORD got = GetModuleFileNameA(NULL, self, (DWORD)sizeof self);
    int code = -1;
    if (got == 0 || got >= sizeof self) {
        return -2;
    }
    snprintf(command, sizeof command, "\"%s\" --stdin-child", self);
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return -2;
    }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = rd;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    memset(&process, 0, sizeof process);
    if (!CreateProcessA(self, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -2;
    }
    CloseHandle(rd);
    if (len > 0) {
        DWORD wrote = 0;
        if (!WriteFile(wr, data, (DWORD)len, &wrote, NULL) || wrote != len) {
            code = -3;
        }
    }
    if (close_after) {
        CloseHandle(wr);
        wr = NULL;
    }
    if (WaitForSingleObject(process.hProcess, STDIN_CHILD_WAIT_MS) == WAIT_OBJECT_0) {
        DWORD status = 0;
        if (code == -1 && GetExitCodeProcess(process.hProcess, &status)) {
            code = (int)status;
        }
    } else {
        TerminateProcess(process.hProcess, 99);
        WaitForSingleObject(process.hProcess, INFINITE);
        code = -1;
    }
    if (wr) {
        CloseHandle(wr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}
#else
static const char *g_self;

static int run_stdin_child(const char *data, size_t len, int close_after) {
    int fds[2];
    posix_spawn_file_actions_t actions;
    pid_t pid = 0;
    char *argv[3];
    int code = -1;
    int status = 0;
    int waited_ms = 0;
    if (!g_self || pipe(fds) != 0) {
        return -2;
    }
    argv[0] = (char *)g_self;
    argv[1] = "--stdin-child";
    argv[2] = NULL;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -2;
    }
    posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    if (posix_spawn(&pid, g_self, &actions, NULL, argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(fds[0]);
        close(fds[1]);
        return -2;
    }
    posix_spawn_file_actions_destroy(&actions);
    close(fds[0]);
    if (len > 0 && write(fds[1], data, len) != (ssize_t)len) {
        code = -3;
    }
    if (close_after) {
        close(fds[1]);
        fds[1] = -1;
    }
    for (;;) {
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            if (code == -1 && WIFEXITED(status)) {
                code = WEXITSTATUS(status);
            }
            break;
        }
        if (waited_ms >= STDIN_CHILD_WAIT_MS) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            code = -1;
            break;
        }
        struct timespec pause = {0, 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
        waited_ms += 10;
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    return code;
}
#endif

static void test_stdin_read(void) {
    unsigned char buf[4];
    CHECK(jogrt_stdin_read(NULL, 4) == -1);
    CHECK(jogrt_stdin_read(buf, 0) == -1);
    /* Three bytes into a 64-byte read, with the pipe left open. */
    CHECK(run_stdin_child("abc", 3, 0) == 3);
    /* A closed pipe with nothing written is end of input. */
    CHECK(run_stdin_child("", 0, 1) == 0);
}

static void test_stdio_binary(void) {
    CHECK(jogrt_stdio_binary());
#if defined(_WIN32)
    /* _setmode returns the previous mode: binary, set by the call above. */
    CHECK(_setmode(_fileno(stdout), _O_TEXT) == _O_BINARY);
    CHECK(_setmode(_fileno(stdin), _O_TEXT) == _O_BINARY);
#endif
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--stdin-child") == 0) {
        return stdin_child();
    }
#if !defined(_WIN32)
    g_self = argc > 0 ? argv[0] : NULL;
#else
    (void)argv;
#endif
    test_shims();
    test_write();
    test_stdin_read();
    test_stdio_binary();
    if (g_failures != 0) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
