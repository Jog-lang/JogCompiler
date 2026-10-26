#ifndef JOGRT_H
#define JOGRT_H

/* The ABI between generated C and the V1 runtime. Generated code includes
   only this header plus <stdint.h>, <stddef.h>, and <stdbool.h>.

   The runtime is C17, single-threaded (V1), and is compiled together with
   every program by `jog build`. jogrt.c holds everything below; jogrt_main.c
   holds the C main(), kept separate so tests can link the runtime with
   their own main.

   Contract owner: see docs/plans/V1Parallel.md. Generated code depends on
   every name here, so renaming one is a contract change. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- defined by generated code ---- */

/* Runs every global initializer in package dependency order. Empty when
   the program has none. */
void jog_entry_init(void);

/* Calls the Jog main. */
void jog_entry_main(void);

/* ---- startup (jogrt_main.c) ----

   int main(int argc, char **argv):
     1. jogrt_start(argc, argv)
     2. jog_entry_init()
     3. jog_entry_main()
     4. return jogrt_finish()  */

void jogrt_start(int argc, char **argv);
int jogrt_finish(void);

/* ---- panics ----
   Print "panic: <what>\n  at <file>:<line>" to stderr, flush, then abort().
   jogrt_panic_call installs a handler: while it is set, panic longjmps to
   that call instead of aborting. The handler is cleared before the jump. */

_Noreturn void jogrt_panic(const char *msg, size_t len, const char *file, uint32_t line);
_Noreturn void jogrt_panic_bounds(size_t index, size_t len, const char *file, uint32_t line);
_Noreturn void jogrt_panic_overflow(const char *file, uint32_t line);
_Noreturn void jogrt_panic_divide_by_zero(const char *file, uint32_t line);
_Noreturn void jogrt_panic_null(const char *file, uint32_t line);
_Noreturn void jogrt_panic_convert(const char *file, uint32_t line); /* checked `as` did not fit */
_Noreturn void jogrt_panic_unreachable(const char *file, uint32_t line);

/* Jog closure { code, env }, the same two words the C backend uses for a
   non-extern function value. For () -> (), code is void (*)(void *). */
typedef struct jogrt_closure {
    void (*code)(void *);
    void *env;
} jogrt_closure;

/* body points at a jogrt_closure. Calls code(env). Returns 1 if that call
   panics, 0 if it returns. A nested call saves the previous handler and
   restores it. The pointer is void * so generated code can pass &closure
   without a struct-type mismatch. */
int jogrt_panic_call(void *body);

/* ---- process ---- */

int jogrt_argc(void);
/* Argument i as bytes without the NUL, or NULL when out of range.
   const int8_t * is the C type of Jog *const c.char, so an extern
   binding can redeclare this function. len receives the byte count
   when it is not NULL. */
const int8_t *jogrt_argv(int i, size_t *len);
_Noreturn void jogrt_exit(int code);

/* ---- default Allocator context (malloc-backed) ----
   align is a power of two. Returns NULL on failure; size 0 returns a unique
   non-NULL pointer. */

void *jogrt_alloc(size_t size, size_t align);
void *jogrt_realloc(void *ptr, size_t old_size, size_t new_size, size_t align);
void jogrt_free(void *ptr, size_t size, size_t align);

/* ---- root ErrorAllocator arena ---- */

void *jogrt_error_alloc(size_t size, size_t align);
/* Mark and release for errorScope { }. */
size_t jogrt_error_mark(void);
void jogrt_error_release(size_t mark);

/* ---- libc shims for things that are macros on some C libraries ---- */

void *jogrt_stdin(void);  /* FILE * */
void *jogrt_stdout(void);
void *jogrt_stderr(void);
int jogrt_errno(void);

/* Writes all len bytes to fd 1 (stdout) or 2 (stderr) through stdio.
   False on a write error. */
bool jogrt_write(int fd, const void *ptr, size_t len);

/* Reads at most cap bytes from the stdin handle (fd 0) and returns as soon
   as at least one byte is available: a pipe does not wait for cap bytes.
   Returns the count, 0 at end of input (a closed pipe included), or -1 on
   an error. cap 0 returns -1. It bypasses stdio's buffer, so do not mix it
   with stdio reads of stdin. Windows reads with ReadFile, which does no
   CRLF translation; other hosts call read(0). */
intptr_t jogrt_stdin_read(void *buf, size_t cap);

/* Puts stdin and stdout in binary mode, so "\n" is not written as "\r\n"
   and "\r\n" is not read as "\n". stdout is flushed first. True on success;
   a no-op that returns true where text and binary mode are the same. */
bool jogrt_stdio_binary(void);

/* ---- coroutines: milestone 6, single-threaded scheduler ----
   One OS thread. go only enqueues. A channel send or recv, sleep, yield,
   select, and scope join park the running coroutine and switch to another.
   stack_bytes 0 selects the 256 KiB default. Status results are 0 ok,
   1 cancelled, 2 closed. jogrt_select_wait returns a case index, -1 when
   the scope was cancelled, or -2 when every send case was closed.
   file is a NUL-terminated launch site, or NULL. */

void jogrt_sched_boot(void);
void *jogrt_scope_root(void);
void *jogrt_scope_current(void);
void *jogrt_scope_enter(void *parent);
void *jogrt_scope_leave(void *scope);
void jogrt_scope_cancel(void *scope);
void jogrt_scope_join(void *scope);
void jogrt_scope_fail(void *err);
void *jogrt_go(void (*fn)(void *), void *env, uintptr_t stack_bytes, void *scope, const void *file, uint32_t line);
void jogrt_yield(void);
void jogrt_task_set(const void *ptr, uintptr_t size);
void jogrt_task_join(void *task, void *out, uintptr_t size);
void *jogrt_chan_init(uintptr_t elem_size, uintptr_t cap);
void jogrt_chan_free(void *chan);
void jogrt_chan_close(void *chan);
int32_t jogrt_chan_send(void *chan, const void *elem, uintptr_t size);
int32_t jogrt_chan_recv(void *chan, void *elem, uintptr_t size);
void jogrt_select_begin(void);
int32_t jogrt_select_add_recv(void *chan, void *dest, uintptr_t size);
int32_t jogrt_select_add_send(void *chan, const void *elem, uintptr_t size);
int32_t jogrt_select_add_default(void);
int32_t jogrt_select_add_timeout(int64_t ms);
int32_t jogrt_select_wait(void);
int32_t jogrt_select_status(void);
int32_t jogrt_sleep(int64_t ms);
void *jogrt_err_cancelled(void);
void *jogrt_err_closed(void);
void *jogrt_cap_new(uintptr_t count);
void jogrt_cap_set(void *cap, uintptr_t index, void *ptr);
void *jogrt_cap_get(void *cap, uintptr_t index);

/* POSIX helpers for std. The layouts of struct dirent and
   posix_spawn_file_actions_t, and the WEXITSTATUS macro, are not spelled
   in Jog. On Windows and wasm these return failure and do not spawn. */

/* d_name of a struct dirent from readdir. NULL when ent is NULL. The
   pointer is valid until the next readdir or closedir on that directory.
   const int8_t * is the C type of Jog *const c.char, so the generated
   redeclaration matches this prototype. */
const int8_t *jogrt_dirent_name(const void *ent);

/* mkdir(path, 0777). 0 on success, -1 on failure. mode_t stays here.
   path is const int8_t * so it matches Jog *const c.char. */
int jogrt_mkdir(const int8_t *path);

/* Heap posix_spawn_file_actions_t. NULL on failure. */
void *jogrt_spawn_actions_new(void);
/* 0 on success, -1 when actions is NULL or adddup2 fails. */
int jogrt_spawn_actions_adddup2(void *actions, int fd, int newfd);
void jogrt_spawn_actions_free(void *actions);

/* WEXITSTATUS when the child exited normally, otherwise -1. */
int jogrt_wexitstatus(int status);

#endif
