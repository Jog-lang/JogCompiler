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
void jogrt_go(void *fn, void *arg);
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

#ifdef __cplusplus
}
#endif
