#include "jogrt.h"

/* Freestanding wasm32 runtime. The native files use libc, Windows, and
   POSIX stacks. No wasi-libc sysroot is provided, so step 2 does not
   link them. The host calls main(argc, argv) with the module path
   and any arguments after `--`; those pointers are in linear memory.
   wasm-ld imports each undefined symbol from "env" unless the declaration
   names another import module. Generated extern functions (hello's puts)
   arrive the same way. Allocation is a bump over a static heap. jogrt_free
   does not reuse memory. FILE* shims are null and errno is 0; programs
   write through jogrt_write. go runs the body on this call. Any other
   scheduler operation aborts, because core Wasm cannot switch stacks.
   jogrt_arg_ptr returns one argument as a pointer to const int8_t, which
   is the C type of a Jog *const i8. */

extern int jogrt_host_write(int fd, const void *ptr, size_t len);
extern _Noreturn void jogrt_host_abort(void);
extern _Noreturn void jogrt_host_exit(int code);

enum { JOGRT_WASM_HEAP = 1 << 20, JOGRT_WASM_ERROR = 1 << 16 };

static int g_argc;
static char **g_argv;
static _Alignas(16) unsigned char g_heap[JOGRT_WASM_HEAP];
static size_t g_heap_used;
static _Alignas(16) unsigned char g_error[JOGRT_WASM_ERROR];
static size_t g_error_used;

static int is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static void bytes_set(unsigned char *dst, unsigned char value, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        dst[i] = value;
    }
}

static void bytes_copy(unsigned char *dst, const unsigned char *src, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

static void host_write(int fd, const void *ptr, size_t len) {
    if (!ptr || len == 0) {
        return;
    }
    (void)jogrt_host_write(fd, ptr, len);
}

static void host_write_str(int fd, const char *text) {
    size_t n = 0;
    if (!text) {
        return;
    }
    while (text[n] != '\0') {
        n++;
    }
    host_write(fd, text, n);
}

static void host_write_usize(int fd, size_t value) {
    char buf[40];
    size_t n = 0;
    do {
        buf[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof buf);
    while (n > 0) {
        n--;
        host_write(fd, &buf[n], 1);
    }
}

static _Noreturn void panic_end(const char *file, uint32_t line) {
    host_write_str(2, "\n  at ");
    host_write_str(2, file ? file : "");
    host_write_str(2, ":");
    host_write_usize(2, line);
    host_write_str(2, "\n");
    jogrt_host_abort();
}

static _Noreturn void scheduler_unavailable(const char *what) {
    host_write_str(2, "panic: wasm32 scheduler is not available (");
    host_write_str(2, what ? what : "");
    host_write_str(2, ")\n");
    jogrt_host_abort();
}

static void *bump(unsigned char *base, size_t cap, size_t *used, size_t bytes, size_t align) {
    size_t at;
    size_t start;
    if (!is_power_of_two(align) || !used) {
        return NULL;
    }
    at = *used;
    if (align - 1 > SIZE_MAX - at) {
        return NULL;
    }
    start = (at + (align - 1)) & ~(align - 1);
    if (start < at || start > cap || bytes > cap - start) {
        return NULL;
    }
    if (bytes > 0) {
        bytes_set(base + start, 0, bytes);
    }
    *used = start + bytes;
    return base + start;
}

void jogrt_start(int argc, char **argv) {
    g_argc = argc > 0 ? argc : 0;
    g_argv = argv;
    jogrt_sched_boot();
}

int jogrt_finish(void) {
    g_error_used = 0;
    return 0;
}

_Noreturn void jogrt_panic(const char *msg, size_t len, const char *file, uint32_t line) {
    host_write_str(2, "panic: ");
    if (msg && len > 0) {
        host_write(2, msg, len);
    }
    panic_end(file, line);
}

_Noreturn void jogrt_panic_bounds(size_t index, size_t len, const char *file, uint32_t line) {
    host_write_str(2, "panic: index ");
    host_write_usize(2, index);
    host_write_str(2, " out of bounds for length ");
    host_write_usize(2, len);
    panic_end(file, line);
}

_Noreturn void jogrt_panic_overflow(const char *file, uint32_t line) {
    host_write_str(2, "panic: integer overflow");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_divide_by_zero(const char *file, uint32_t line) {
    host_write_str(2, "panic: division by zero");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_null(const char *file, uint32_t line) {
    host_write_str(2, "panic: null pointer");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_convert(const char *file, uint32_t line) {
    host_write_str(2, "panic: value out of range");
    panic_end(file, line);
}

_Noreturn void jogrt_panic_unreachable(const char *file, uint32_t line) {
    host_write_str(2, "panic: unreachable");
    panic_end(file, line);
}

/* Wasm has no setjmp. A panic still aborts. A normal return is 0.
   body points at { code, env }. */
int jogrt_panic_call(void *body) {
    union {
        jogrt_closure closure;
        unsigned char bytes[sizeof(jogrt_closure)];
    } pun;
    const unsigned char *src;
    size_t i;
    if (!body) {
        return 0;
    }
    src = body;
    for (i = 0; i < sizeof pun.bytes; i++) {
        pun.bytes[i] = src[i];
    }
    if (!pun.closure.code) {
        return 0;
    }
    pun.closure.code(pun.closure.env);
    return 0;
}

int jogrt_argc(void) {
    return g_argc;
}

const int8_t *jogrt_argv(int i, size_t *len) {
    const char *arg;
    size_t n = 0;
    if (i < 0 || i >= g_argc || !g_argv || !g_argv[i]) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    arg = g_argv[i];
    while (arg[n] != '\0') {
        n++;
    }
    if (len) {
        *len = n;
    }
    return (const int8_t *)(const void *)arg;
}

/* Same bytes as jogrt_argv, without the length out-parameter. */
const int8_t *jogrt_arg_ptr(int32_t i) {
    const int8_t *arg;
    if (i < 0 || i >= g_argc || !g_argv) {
        return NULL;
    }
    arg = (const int8_t *)(const void *)g_argv[i];
    if (!arg) {
        return NULL;
    }
    return arg;
}

_Noreturn void jogrt_exit(int code) {
    jogrt_host_exit(code);
}

void *jogrt_alloc(size_t size, size_t align) {
    size_t bytes = size == 0 ? 1 : size;
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    return bump(g_heap, sizeof g_heap, &g_heap_used, bytes, align);
}

void *jogrt_realloc(void *ptr, size_t old_size, size_t new_size, size_t align) {
    void *next = jogrt_alloc(new_size, align);
    size_t copy;
    if (!next) {
        return NULL;
    }
    if (ptr && old_size > 0 && new_size > 0) {
        copy = old_size < new_size ? old_size : new_size;
        bytes_copy(next, ptr, copy);
    }
    return next;
}

void jogrt_free(void *ptr, size_t size, size_t align) {
    (void)ptr;
    (void)size;
    (void)align;
}

void *jogrt_error_alloc(size_t size, size_t align) {
    return bump(g_error, sizeof g_error, &g_error_used, size, align);
}

size_t jogrt_error_mark(void) {
    return g_error_used;
}

void jogrt_error_release(size_t mark) {
    if (mark > g_error_used) {
        return;
    }
    g_error_used = mark;
}

void *jogrt_stdin(void) {
    return NULL;
}

void *jogrt_stdout(void) {
    return NULL;
}

void *jogrt_stderr(void) {
    return NULL;
}

int jogrt_errno(void) {
    return 0;
}

bool jogrt_write(int fd, const void *ptr, size_t len) {
    if (fd != 1 && fd != 2) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (!ptr) {
        return false;
    }
    return jogrt_host_write(fd, ptr, len) != 0;
}

/* The host has no stdin import. */
intptr_t jogrt_stdin_read(void *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return -1;
}

bool jogrt_stdio_binary(void) {
    return true;
}

void jogrt_sched_boot(void) {}

void *jogrt_scope_root(void) {
    scheduler_unavailable("scope");
}

void *jogrt_scope_current(void) {
    scheduler_unavailable("scope");
}

void *jogrt_scope_enter(void *parent) {
    (void)parent;
    scheduler_unavailable("scope");
}

void *jogrt_scope_leave(void *scope) {
    (void)scope;
    scheduler_unavailable("scope");
}

void jogrt_scope_cancel(void *scope) {
    (void)scope;
    scheduler_unavailable("scope");
}

void jogrt_scope_join(void *scope) {
    (void)scope;
    scheduler_unavailable("scope");
}

void jogrt_scope_fail(void *err) {
    (void)err;
    scheduler_unavailable("scope");
}

void *jogrt_go(void (*fn)(void *), void *env, uintptr_t stack_bytes, void *scope, const void *file, uint32_t line) {
    (void)stack_bytes;
    (void)scope;
    (void)file;
    (void)line;
    if (fn) {
        fn(env);
    }
    return NULL;
}

void jogrt_yield(void) {
    scheduler_unavailable("yield");
}

void jogrt_task_set(const void *ptr, uintptr_t size) {
    (void)ptr;
    (void)size;
    scheduler_unavailable("task");
}

void jogrt_task_join(void *task, void *out, uintptr_t size) {
    (void)task;
    (void)out;
    (void)size;
    scheduler_unavailable("task");
}

void *jogrt_chan_init(uintptr_t elem_size, uintptr_t cap) {
    (void)elem_size;
    (void)cap;
    scheduler_unavailable("channel");
}

void jogrt_chan_free(void *chan) {
    (void)chan;
    scheduler_unavailable("channel");
}

void jogrt_chan_close(void *chan) {
    (void)chan;
    scheduler_unavailable("channel");
}

int32_t jogrt_chan_send(void *chan, const void *elem, uintptr_t size) {
    (void)chan;
    (void)elem;
    (void)size;
    scheduler_unavailable("channel");
}

int32_t jogrt_chan_recv(void *chan, void *elem, uintptr_t size) {
    (void)chan;
    (void)elem;
    (void)size;
    scheduler_unavailable("channel");
}

void jogrt_select_begin(void) {
    scheduler_unavailable("select");
}

int32_t jogrt_select_add_recv(void *chan, void *dest, uintptr_t size) {
    (void)chan;
    (void)dest;
    (void)size;
    scheduler_unavailable("select");
}

int32_t jogrt_select_add_send(void *chan, const void *elem, uintptr_t size) {
    (void)chan;
    (void)elem;
    (void)size;
    scheduler_unavailable("select");
}

int32_t jogrt_select_add_default(void) {
    scheduler_unavailable("select");
}

int32_t jogrt_select_add_timeout(int64_t ms) {
    (void)ms;
    scheduler_unavailable("select");
}

int32_t jogrt_select_wait(void) {
    scheduler_unavailable("select");
}

int32_t jogrt_select_status(void) {
    scheduler_unavailable("select");
}

int32_t jogrt_sleep(int64_t ms) {
    (void)ms;
    scheduler_unavailable("sleep");
}

void *jogrt_err_cancelled(void) {
    scheduler_unavailable("error");
}

void *jogrt_err_closed(void) {
    scheduler_unavailable("error");
}

void *jogrt_cap_new(uintptr_t count) {
    (void)count;
    scheduler_unavailable("capture");
}

void jogrt_cap_set(void *cap, uintptr_t index, void *ptr) {
    (void)cap;
    (void)index;
    (void)ptr;
    scheduler_unavailable("capture");
}

void *jogrt_cap_get(void *cap, uintptr_t index) {
    (void)cap;
    (void)index;
    scheduler_unavailable("capture");
}

const int8_t *jogrt_dirent_name(const void *ent) {
    (void)ent;
    return NULL;
}

int jogrt_mkdir(const int8_t *path) {
    (void)path;
    return -1;
}

void *jogrt_spawn_actions_new(void) {
    return NULL;
}

int jogrt_spawn_actions_adddup2(void *actions, int fd, int newfd) {
    (void)actions;
    (void)fd;
    (void)newfd;
    return -1;
}

void jogrt_spawn_actions_free(void *actions) {
    (void)actions;
}

int jogrt_wexitstatus(int status) {
    (void)status;
    return -1;
}
