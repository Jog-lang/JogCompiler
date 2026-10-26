#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "jogrt.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single-threaded V1 runtime. The default allocator is malloc. Each block
   stores the malloc pointer immediately before the user pointer, so free
   does not need the size. The root error arena is a bump allocator: a mark
   is a byte offset from its start, and release rewinds to that offset.
   Chunks are kept and reused; jogrt_finish frees them. */

enum { JOGRT_ERROR_CHUNK = 8192 };

typedef struct ErrChunk {
    struct ErrChunk *next;
    size_t offset;
    size_t capacity;
    size_t used;
    unsigned char data[];
} ErrChunk;

static int g_argc;
static char **g_argv;
static ErrChunk *g_err_head;
static ErrChunk *g_err_current;
static jmp_buf *g_panic_jmp;

static int is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static void emit(FILE *out, const void *data, size_t len) {
    if (!data || len == 0) {
        return;
    }
    size_t wrote = fwrite(data, 1, len, out);
    (void)wrote;
}

static void emit_str(FILE *out, const char *text) {
    if (text) {
        emit(out, text, strlen(text));
    }
}

static void emit_usize(FILE *out, size_t value) {
    char buf[40];
    size_t n = 0;
    do {
        buf[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof(buf));
    while (n > 0) {
        int ch = fputc(buf[--n], out);
        (void)ch;
    }
}

/* "panic: <what>\n  at <file>:<line>\n" then abort, or longjmp when
   jogrt_panic_call has installed a handler. stderr is flushed because
   abort does not flush, and a pipe would otherwise drop the text.
   The handler is cleared before the jump so a panic while unwinding aborts. */
static _Noreturn void panic_end(const char *file, uint32_t line) {
    FILE *out = stderr;
    emit_str(out, "\n  at ");
    emit_str(out, file);
    int ch = fputc(':', out);
    (void)ch;
    emit_usize(out, line);
    ch = fputc('\n', out);
    (void)ch;
    int flush = fflush(out);
    (void)flush;
    if (g_panic_jmp) {
        jmp_buf *target = g_panic_jmp;
        g_panic_jmp = NULL;
        longjmp(*target, 1);
    }
    abort();
}

int jogrt_panic_call(void *body) {
    jogrt_closure closure;
    jmp_buf buf;
    jmp_buf *volatile prev = g_panic_jmp;
    void (*volatile code)(void *);
    void *volatile env;
    if (!body) {
        return 0;
    }
    memcpy(&closure, body, sizeof(closure));
    code = closure.code;
    env = closure.env;
    if (!code) {
        return 0;
    }
    if (setjmp(buf) != 0) {
        g_panic_jmp = prev;
        return 1;
    }
    g_panic_jmp = &buf;
    code(env);
    g_panic_jmp = prev;
    return 0;
}

void jogrt_start(int argc, char **argv) {
    g_argc = argc > 0 ? argc : 0;
    g_argv = argv;
    jogrt_sched_boot();
}

static void error_arena_free(void) {
    ErrChunk *chunk = g_err_head;
    while (chunk) {
        ErrChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    g_err_head = NULL;
    g_err_current = NULL;
}

int jogrt_finish(void) {
    error_arena_free();
    return 0;
}

_Noreturn void jogrt_panic(const char *msg, size_t len, const char *file, uint32_t line) {
    emit_str(stderr, "panic: ");
    if (msg && len > 0) {
        emit(stderr, msg, len);
    }
    panic_end(file, line);
}

_Noreturn void jogrt_panic_bounds(size_t index, size_t len, const char *file, uint32_t line) {
    emit_str(stderr, "panic: index ");
    emit_usize(stderr, index);
    emit_str(stderr, " out of bounds for length ");
    emit_usize(stderr, len);
    panic_end(file, line);
}

_Noreturn void jogrt_panic_overflow(const char *file, uint32_t line) {
    emit_str(stderr, "panic: integer overflow");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_divide_by_zero(const char *file, uint32_t line) {
    emit_str(stderr, "panic: division by zero");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_null(const char *file, uint32_t line) {
    emit_str(stderr, "panic: null pointer");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_convert(const char *file, uint32_t line) {
    emit_str(stderr, "panic: value out of range");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_unreachable(const char *file, uint32_t line) {
    emit_str(stderr, "panic: unreachable");
    panic_end(file, line);
}

int jogrt_argc(void) {
    return g_argc;
}

const int8_t *jogrt_argv(int i, size_t *len) {
    if (i < 0 || i >= g_argc || !g_argv || !g_argv[i]) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = strlen(g_argv[i]);
    }
    return (const int8_t *)(const void *)g_argv[i];
}

/* Same bytes as jogrt_argv, without the length out-parameter.
   The wasm runtime has the same function. */
const int8_t *jogrt_arg_ptr(int32_t i) {
    const int8_t *arg = jogrt_argv((int)i, NULL);
    if (!arg) {
        return NULL;
    }
    return (const int8_t *)(const void *)arg;
}

_Noreturn void jogrt_exit(int code) {
    exit(code);
}

/* align must be a power of two. size 0 still returns a distinct non-NULL
   pointer (a one-byte heap block). The result is zeroed. NULL means the
   alignment is invalid or malloc failed. */
void *jogrt_alloc(size_t size, size_t align) {
    if (!is_power_of_two(align)) {
        return NULL;
    }
    /* The header is a void *, so the block is at least that aligned.
       A larger power of two still satisfies the caller's alignment. */
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    size_t bytes = size == 0 ? 1 : size;
    if (bytes > SIZE_MAX - sizeof(void *) - (align - 1)) {
        return NULL;
    }

    size_t total = sizeof(void *) + (align - 1) + bytes;
    void *raw = malloc(total);
    if (!raw) {
        return NULL;
    }

    uintptr_t start = (uintptr_t)raw + sizeof(void *);
    uintptr_t aligned = (start + (align - 1)) & ~(uintptr_t)(align - 1);
    unsigned char *user = (unsigned char *)aligned;
    memcpy(user - sizeof(void *), &raw, sizeof(raw));
    memset(user, 0, bytes);
    return user;
}

void *jogrt_realloc(void *ptr, size_t old_size, size_t new_size, size_t align) {
    void *next = jogrt_alloc(new_size, align);
    if (!next) {
        return NULL;
    }
    if (ptr && old_size > 0) {
        size_t copy = old_size < new_size ? old_size : new_size;
        if (copy > 0) {
            memcpy(next, ptr, copy);
        }
    }
    if (ptr) {
        jogrt_free(ptr, old_size, align);
    }
    return next;
}

void jogrt_free(void *ptr, size_t size, size_t align) {
    (void)size;
    (void)align;
    if (!ptr) {
        return;
    }
    void *raw = NULL;
    memcpy(&raw, (unsigned char *)ptr - sizeof(void *), sizeof(raw));
    free(raw);
}

static int err_try(ErrChunk *chunk, size_t size, size_t align, void **out) {
    uintptr_t base = (uintptr_t)chunk->data;
    uintptr_t current = base + chunk->used;
    uintptr_t aligned = (current + (align - 1)) & ~(uintptr_t)(align - 1);
    if (aligned < current) {
        return 0;
    }

    size_t padding = (size_t)(aligned - current);
    if (chunk->used > chunk->capacity) {
        return 0;
    }
    size_t available = chunk->capacity - chunk->used;
    if (padding > available || size > available - padding) {
        return 0;
    }

    if (size > 0) {
        memset((void *)aligned, 0, size);
    }
    chunk->used += padding + size;
    *out = (void *)aligned;
    return 1;
}

static ErrChunk *err_chunk_new(size_t min_capacity) {
    size_t capacity = JOGRT_ERROR_CHUNK;
    if (min_capacity > capacity) {
        capacity = min_capacity;
    }
    if (capacity > SIZE_MAX - sizeof(ErrChunk)) {
        return NULL;
    }

    ErrChunk *chunk = malloc(sizeof(ErrChunk) + capacity);
    if (!chunk) {
        return NULL;
    }
    chunk->next = NULL;
    chunk->offset = 0;
    chunk->capacity = capacity;
    chunk->used = 0;
    return chunk;
}

void *jogrt_error_alloc(size_t size, size_t align) {
    if (!is_power_of_two(align) || size > SIZE_MAX - (align - 1)) {
        return NULL;
    }

    void *found = NULL;
    if (g_err_current && err_try(g_err_current, size, align, &found)) {
        return found;
    }

    ErrChunk *last = g_err_current;
    for (ErrChunk *chunk = last ? last->next : NULL; chunk; chunk = chunk->next) {
        last = chunk;
        if (err_try(chunk, size, align, &found)) {
            g_err_current = chunk;
            return found;
        }
    }

    ErrChunk *chunk = err_chunk_new(size + (align - 1));
    if (!chunk) {
        return NULL;
    }
    if (last) {
        if (last->capacity > SIZE_MAX - last->offset || chunk->capacity > SIZE_MAX - (last->offset + last->capacity)) {
            free(chunk);
            return NULL;
        }
        chunk->offset = last->offset + last->capacity;
        last->next = chunk;
    } else {
        g_err_head = chunk;
    }
    g_err_current = chunk;
    if (!err_try(chunk, size, align, &found)) {
        return NULL;
    }
    return found;
}

size_t jogrt_error_mark(void) {
    if (!g_err_current) {
        return 0;
    }
    return g_err_current->offset + g_err_current->used;
}

void jogrt_error_release(size_t mark) {
    if (!g_err_current) {
        return;
    }
    size_t frontier = g_err_current->offset + g_err_current->used;
    if (mark > frontier) {
        return;
    }

    for (ErrChunk *chunk = g_err_head; chunk; chunk = chunk->next) {
        size_t end = chunk->offset + chunk->capacity;
        if (mark > end) {
            continue;
        }
        if (mark < chunk->offset) {
            chunk->used = 0;
            continue;
        }
        chunk->used = mark - chunk->offset;
        for (ErrChunk *rest = chunk->next; rest; rest = rest->next) {
            rest->used = 0;
        }
        g_err_current = chunk;
        return;
    }
}

void *jogrt_stdin(void) {
    return stdin;
}

void *jogrt_stdout(void) {
    return stdout;
}

void *jogrt_stderr(void) {
    return stderr;
}

int jogrt_errno(void) {
    return errno;
}

bool jogrt_write(int fd, const void *ptr, size_t len) {
    FILE *out = NULL;
    if (fd == 1) {
        out = stdout;
    } else if (fd == 2) {
        out = stderr;
    } else {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (!ptr) {
        return false;
    }

    const unsigned char *bytes = ptr;
    size_t done = 0;
    while (done < len) {
        size_t n = fwrite(bytes + done, 1, len - done, out);
        if (n == 0) {
            return false;
        }
        done += n;
    }
    if (fflush(out) != 0) {
        return false;
    }
    return true;
}

#if defined(_WIN32)

intptr_t jogrt_stdin_read(void *buf, size_t cap) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD want = cap > 0x7FFFFFFFu ? 0x7FFFFFFFu : (DWORD)cap;
    DWORD got = 0;
    if (!buf || cap == 0 || !in || in == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!ReadFile(in, buf, want, &got, NULL)) {
        DWORD err = GetLastError();
        return err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (intptr_t)got;
}

bool jogrt_stdio_binary(void) {
    if (fflush(stdout) != 0) {
        return false;
    }
    return _setmode(_fileno(stdin), _O_BINARY) != -1 && _setmode(_fileno(stdout), _O_BINARY) != -1;
}

#else

intptr_t jogrt_stdin_read(void *buf, size_t cap) {
    if (!buf || cap == 0) {
        return -1;
    }
    if (cap > (size_t)INTPTR_MAX) {
        cap = (size_t)INTPTR_MAX;
    }
    for (;;) {
        ssize_t got = read(0, buf, cap);
        if (got >= 0) {
            return (intptr_t)got;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

bool jogrt_stdio_binary(void) {
    return fflush(stdout) == 0;
}

#endif

/* dst NULL measures: cap is ignored and *overflow is left alone unless the
   cursor would wrap. A piece that does not fit sets *overflow and returns
   `at` unchanged. jogrt_fmt_error is a stable non-null byte. */

static uint8_t g_fmt_error;

static uintptr_t fmt_put(uint8_t *dst, uintptr_t cap, uintptr_t at, const uint8_t *bytes, uintptr_t len,
                         int32_t *overflow) {
    uintptr_t next;
    if (len == 0) {
        return at;
    }
    if (!bytes) {
        if (overflow) {
            *overflow = 1;
        }
        return at;
    }
    if (at > UINTPTR_MAX - len) {
        if (overflow) {
            *overflow = 1;
        }
        return at;
    }
    next = at + len;
    if (!dst) {
        return next;
    }
    if (at > cap || len > cap - at) {
        if (overflow) {
            *overflow = 1;
        }
        return at;
    }
    memcpy(dst + at, bytes, (size_t)len);
    return next;
}

uint8_t *jogrt_fmt_error(void) {
    return &g_fmt_error;
}

uint8_t *jogrt_fmt_alloc(uintptr_t size) {
    return jogrt_alloc((size_t)size, 1);
}

uintptr_t jogrt_fmt_i64(uint8_t *dst, uintptr_t cap, uintptr_t at, int64_t value, int32_t *overflow) {
    uint8_t buf[20];
    uint8_t *p = buf + 20;
    uint64_t mag;
    if (value < 0) {
        mag = (uint64_t)(-(value + 1)) + 1;
    } else {
        mag = (uint64_t)value;
    }
    do {
        *--p = (uint8_t)('0' + (mag % 10));
        mag /= 10;
    } while (mag != 0);
    if (value < 0) {
        *--p = (uint8_t)'-';
    }
    return fmt_put(dst, cap, at, p, (uintptr_t)(buf + 20 - p), overflow);
}

uintptr_t jogrt_fmt_u64(uint8_t *dst, uintptr_t cap, uintptr_t at, uint64_t value, int32_t *overflow) {
    uint8_t buf[20];
    uint8_t *p = buf + 20;
    uint64_t mag = value;
    do {
        *--p = (uint8_t)('0' + (mag % 10));
        mag /= 10;
    } while (mag != 0);
    return fmt_put(dst, cap, at, p, (uintptr_t)(buf + 20 - p), overflow);
}

uintptr_t jogrt_fmt_bool(uint8_t *dst, uintptr_t cap, uintptr_t at, bool value, int32_t *overflow) {
    const uint8_t *text = value ? (const uint8_t *)"true" : (const uint8_t *)"false";
    uintptr_t len = value ? 4 : 5;
    return fmt_put(dst, cap, at, text, len, overflow);
}

uintptr_t jogrt_fmt_bytes(uint8_t *dst, uintptr_t cap, uintptr_t at, const uint8_t *ptr, uintptr_t len,
                          int32_t *overflow) {
    return fmt_put(dst, cap, at, ptr, len, overflow);
}

const int8_t *jogrt_dirent_name(const void *ent) {
#if defined(_WIN32)
    (void)ent;
    return NULL;
#else
    const struct dirent *entry = ent;
    if (!entry) {
        return NULL;
    }
    return (const int8_t *)entry->d_name;
#endif
}

int jogrt_mkdir(const int8_t *path) {
#if defined(_WIN32)
    (void)path;
    return -1;
#else
    if (!path) {
        return -1;
    }
    return mkdir((const char *)path, 0777);
#endif
}

void *jogrt_spawn_actions_new(void) {
#if defined(_WIN32)
    return NULL;
#else
    posix_spawn_file_actions_t *actions = malloc(sizeof(*actions));
    if (!actions) {
        return NULL;
    }
    if (posix_spawn_file_actions_init(actions) != 0) {
        free(actions);
        return NULL;
    }
    return actions;
#endif
}

int jogrt_spawn_actions_adddup2(void *actions, int fd, int newfd) {
#if defined(_WIN32)
    (void)actions;
    (void)fd;
    (void)newfd;
    return -1;
#else
    if (!actions) {
        return -1;
    }
    return posix_spawn_file_actions_adddup2(actions, fd, newfd);
#endif
}

void jogrt_spawn_actions_free(void *actions) {
#if defined(_WIN32)
    (void)actions;
#else
    if (!actions) {
        return;
    }
    posix_spawn_file_actions_destroy(actions);
    free(actions);
#endif
}

int jogrt_wexitstatus(int status) {
#if defined(_WIN32)
    (void)status;
    return -1;
#else
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}
