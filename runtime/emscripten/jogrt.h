#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void jogrt_print(const void *ptr, int64_t len);
void jogrt_panic(const void *ptr, int64_t len);
void *jogrt_alloc(int64_t nbytes);
int64_t jogrt_argc(void);
const char *jogrt_arg_ptr(int i);
char **jogrt_environ(void);
void jogrt_set_args(int argc, char **argv);
int64_t jogrt_alloc_checkpoint(void);
void jogrt_alloc_reset(int64_t checkpoint);
uint32_t jog_f32_bits(double d);
uint32_t jogrt_f32_bits(int64_t bits);
int64_t jogrt_f32_from_bits(uint32_t u);
void *jogrt_ret_alloc(int64_t nbytes);

/* Returning mainloop: tick returns 0 to stop. On Emscripten this hooks
   emscripten_set_main_loop and does not return until cancelled. */
void jogrt_host_run_loop(int64_t (*tick)(void));
void jog_host_run_loop(int64_t (*tick)(void));

/* go/spawn: run-to-completion on the current stack (no stackful switch).
   yield() still aborts — full M:N needs Asyncify/JSPI/stack-switching. */
void jogrt_spawn(void *fn, void *arg);
void jogrt_dispatcher_stub(void);
int64_t jogrt_go(void *fn, void *arg);
void jogrt_yield(void);
void jogrt_coro_drain(void);
int64_t jogrt_coro_alive(void);
int64_t jogrt_coro_id(void);
void jogrt_coro_park(void);
void jogrt_coro_unpark(int64_t id);
void *jogrt_dispatcher_single(void);
void *jogrt_dispatcher_pool(int64_t n);
void *jogrt_dispatcher_new(int64_t n);
void *jogrt_dispatcher_get(int64_t id);
void *jogrt_dispatcher_io(void);
void jogrt_dispatcher_enter(void *d);
void jogrt_dispatcher_leave(void);
void jogrt_sleep_ms(int64_t ms);
void jogrt_print_tid(const void *ptr, int64_t len);
int64_t jogrt_worker_id(void);
int64_t jogrt_gettid(void);

/* Structured concurrency (JOG-222). RTC: children finish before go returns. */
void *jogrt_scope_root(void);
void *jogrt_scope_current(void);
void *jogrt_scope_enter(void);
void *jogrt_scope_leave(void *scope);
void jogrt_scope_child_inc(void *scope);
void jogrt_scope_child_done(void *scope);
void jogrt_scope_cancel(void *scope);
int64_t jogrt_scope_cancelled(void *scope);
void jogrt_scope_trampoline(void *p);
int64_t jogrt_scope_go(void *scope, void *fn, void *arg, void *wrap);

void *jogrt_chan_init(uint64_t elem_size, uint64_t cap);
void jogrt_chan_free(void *chan);
void jogrt_chan_close(void *chan);
int32_t jogrt_chan_send(void *chan, const void *elem, uint64_t size);
int32_t jogrt_chan_recv(void *chan, void *elem, uint64_t size);
void jogrt_select_begin(void);
int32_t jogrt_select_add_recv(void *chan, void *dest, uint64_t size);
int32_t jogrt_select_add_send(void *chan, const void *elem, uint64_t size);
int32_t jogrt_select_add_default(void);
int32_t jogrt_select_add_timeout(int64_t ms);
int32_t jogrt_select_wait(void);
int32_t jogrt_select_status(void);
int32_t jogrt_sleep(int64_t ms);
void *jogrt_err_cancelled(void);
void *jogrt_err_closed(void);
int64_t jogrt_task_begin(int64_t cid);
void jogrt_task_set(const void *ptr, uint64_t size);
void jogrt_task_finish(void);
void jogrt_task_join(int64_t task, void *out, uint64_t size);

/* JOG-233 debug race detector (no-op stubs on wasm RTC). */
void jogrt_race_enable(void);
void jogrt_race_read(void *addr, uint64_t size);
void jogrt_race_write(void *addr, uint64_t size);
void jogrt_race_release(void *sync);
void jogrt_race_acquire(void *sync);

#ifdef __cplusplus
}
#endif
