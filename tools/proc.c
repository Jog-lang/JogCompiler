#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "proc.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include "fs.h"
#include "str.h"

#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

/* Quote one argument for the C runtime command-line parser used by
   CreateProcess. Always wrapped in double quotes. A run of backslashes
   before a quote, and backslashes at the end of the argument, is doubled
   so the closing quote stays a delimiter. */
static bool append_slashes(CtSb *sb, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!ct_sb_putc(sb, '\\')) {
            return false;
        }
    }
    return true;
}

static bool quote_arg(CtSb *sb, const char *arg) {
    if (!ct_sb_putc(sb, '"')) {
        return false;
    }
    size_t slashes = 0;
    for (const char *p = arg; *p; p++) {
        char c = *p;
        if (c == '\\') {
            slashes++;
            continue;
        }
        if (c == '"') {
            if (slashes > (SIZE_MAX - 1) / 2 || !append_slashes(sb, slashes * 2 + 1) || !ct_sb_putc(sb, '"')) {
                return false;
            }
            slashes = 0;
            continue;
        }
        if (!append_slashes(sb, slashes) || !ct_sb_putc(sb, c)) {
            return false;
        }
        slashes = 0;
    }
    if (slashes > SIZE_MAX / 2 || !append_slashes(sb, slashes * 2)) {
        return false;
    }
    return ct_sb_putc(sb, '"');
}

static bool build_command(CtSb *sb, const char *const *argv) {
    for (size_t i = 0; argv[i]; i++) {
        if (i > 0 && !ct_sb_putc(sb, ' ')) {
            return false;
        }
        if (!quote_arg(sb, argv[i])) {
            return false;
        }
    }
    return sb->data != NULL && !sb->failed;
}

static HANDLE open_capture(const char *path) {
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    wchar_t *wide = ct_fs_widen(path);
    if (!wide) {
        return NULL;
    }
    HANDLE handle = CreateFileW(
        wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    return handle;
}

static HANDLE open_nul(void) {
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    return handle;
}

static HANDLE inherited_std(DWORD id) {
    HANDLE handle = GetStdHandle(id);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    /* Already-inheritable handles can reject the update. CreateProcess
       reports a handle the child cannot inherit. */
    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
        return handle;
    }
    return handle;
}

static void close_owned(HANDLE *owned, int count) {
    for (int i = 0; i < count; i++) {
        CloseHandle(owned[i]);
    }
}

/* Fills the three standard handles. owned receives handles this function
   opened; the caller closes them. Inherited standard handles are not
   owned. stdout and stderr share one owned handle when their paths match. */
static bool setup_stdio(const char *stdout_path, const char *stderr_path, HANDLE *in_h, HANDLE *out_h, HANDLE *err_h,
    HANDLE owned[3], int *owned_count) {
    *owned_count = 0;
    *in_h = inherited_std(STD_INPUT_HANDLE);
    if (!*in_h) {
        *in_h = open_nul();
        if (!*in_h) {
            return false;
        }
        owned[(*owned_count)++] = *in_h;
    }

    if (stdout_path && stderr_path && strcmp(stdout_path, stderr_path) == 0) {
        HANDLE file = open_capture(stdout_path);
        if (!file) {
            close_owned(owned, *owned_count);
            *owned_count = 0;
            return false;
        }
        owned[(*owned_count)++] = file;
        *out_h = file;
        *err_h = file;
        return true;
    }

    if (stdout_path) {
        *out_h = open_capture(stdout_path);
        if (!*out_h) {
            close_owned(owned, *owned_count);
            *owned_count = 0;
            return false;
        }
        owned[(*owned_count)++] = *out_h;
    } else {
        *out_h = inherited_std(STD_OUTPUT_HANDLE);
        if (!*out_h) {
            *out_h = open_nul();
            if (!*out_h) {
                close_owned(owned, *owned_count);
                *owned_count = 0;
                return false;
            }
            owned[(*owned_count)++] = *out_h;
        }
    }

    if (stderr_path) {
        *err_h = open_capture(stderr_path);
        if (!*err_h) {
            close_owned(owned, *owned_count);
            *owned_count = 0;
            return false;
        }
        owned[(*owned_count)++] = *err_h;
    } else {
        *err_h = inherited_std(STD_ERROR_HANDLE);
        if (!*err_h) {
            *err_h = open_nul();
            if (!*err_h) {
                close_owned(owned, *owned_count);
                *owned_count = 0;
                return false;
            }
            owned[(*owned_count)++] = *err_h;
        }
    }
    return true;
}

static bool run_windows(const char *const *argv, const char *stdout_path, const char *stderr_path, int *exit_code) {
    CtSb command;
    ct_sb_init(&command);
    if (!build_command(&command, argv)) {
        ct_sb_free(&command);
        return false;
    }
    wchar_t *wide_command = ct_fs_widen(command.data);
    wchar_t *wide_app = ct_fs_widen(argv[0]);
    ct_sb_free(&command);
    /* CreateProcessW accepts at most 32767 characters, including the NUL. */
    if (!wide_command || !wide_app || wcslen(wide_command) > 32766u) {
        free(wide_command);
        free(wide_app);
        return false;
    }

    HANDLE owned[3] = {0};
    int owned_count = 0;
    HANDLE in_h = NULL;
    HANDLE out_h = NULL;
    HANDLE err_h = NULL;
    int redirect = stdout_path != NULL || stderr_path != NULL;
    if (redirect && !setup_stdio(stdout_path, stderr_path, &in_h, &out_h, &err_h, owned, &owned_count)) {
        free(wide_command);
        free(wide_app);
        return false;
    }

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (redirect) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = in_h;
        startup.hStdOutput = out_h;
        startup.hStdError = err_h;
    }

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    /* CreateProcessW may write into the command line. argv[0] is the
       module itself, so PATH is not searched. */
    BOOL created = CreateProcessW(wide_app, wide_command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    free(wide_command);
    free(wide_app);
    close_owned(owned, owned_count);
    if (!created) {
        return false;
    }

    DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    BOOL got_status = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!got_status) {
        return false;
    }
    *exit_code = (int)status;
    return true;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int move_high(int fd) {
    if (fd > STDERR_FILENO) {
        return fd;
    }
    int high = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
    int saved = errno;
    close(fd);
    if (high < 0) {
        errno = saved;
        return -1;
    }
    if (fcntl(high, F_SETFD, FD_CLOEXEC) < 0) {
        saved = errno;
        close(high);
        errno = saved;
        return -1;
    }
    return high;
}

static int open_capture(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    return move_high(fd);
}

static int dup_high(int fd) {
    int duped = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
    if (duped < 0) {
        return -1;
    }
    if (fcntl(duped, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(duped);
        errno = saved;
        return -1;
    }
    return duped;
}

static void release_posix(int out_fd, int err_fd, posix_spawn_file_actions_t *actions) {
    if (actions) {
        posix_spawn_file_actions_destroy(actions);
    }
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (err_fd >= 0) {
        close(err_fd);
    }
}

static bool add_redirect(posix_spawn_file_actions_t *actions, int fd, int target) {
    if (posix_spawn_file_actions_adddup2(actions, fd, target) != 0) {
        return false;
    }
    return posix_spawn_file_actions_addclose(actions, fd) == 0;
}

static bool run_posix(const char *const *argv, const char *stdout_path, const char *stderr_path, int *exit_code) {
    int out_fd = -1;
    int err_fd = -1;
    if (stdout_path) {
        out_fd = open_capture(stdout_path);
        if (out_fd < 0) {
            return false;
        }
    }
    if (stderr_path) {
        if (stdout_path && strcmp(stdout_path, stderr_path) == 0) {
            err_fd = dup_high(out_fd);
        } else {
            err_fd = open_capture(stderr_path);
        }
        if (err_fd < 0) {
            release_posix(out_fd, -1, NULL);
            return false;
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t *actions_ptr = NULL;
    if (out_fd >= 0 || err_fd >= 0) {
        if (posix_spawn_file_actions_init(&actions) != 0) {
            release_posix(out_fd, err_fd, NULL);
            return false;
        }
        actions_ptr = &actions;
        if (out_fd >= 0 && !add_redirect(&actions, out_fd, STDOUT_FILENO)) {
            release_posix(out_fd, err_fd, actions_ptr);
            return false;
        }
        if (err_fd >= 0 && !add_redirect(&actions, err_fd, STDERR_FILENO)) {
            release_posix(out_fd, err_fd, actions_ptr);
            return false;
        }
    }

    /* posix_spawn does not write the argument strings and does not search PATH. */
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, argv[0], actions_ptr, NULL, (char *const *)argv, environ);
    release_posix(out_fd, err_fd, actions_ptr);
    if (spawned != 0) {
        return false;
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
        return false;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
        return true;
    }
    if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
        return true;
    }
    return false;
}

#endif

bool ct_proc_run(const char *const *argv, const char *stdout_path, const char *stderr_path, int *exit_code) {
    if (!exit_code || !argv || !argv[0] || argv[0][0] == '\0') {
        return false;
    }
#if defined(_WIN32)
    return run_windows(argv, stdout_path, stderr_path, exit_code);
#else
    return run_posix(argv, stdout_path, stderr_path, exit_code);
#endif
}
