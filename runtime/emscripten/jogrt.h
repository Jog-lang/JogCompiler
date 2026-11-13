#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void jogrt_print(const void *ptr, int64_t len);
void jogrt_panic(const void *ptr, int64_t len);
void *jogrt_alloc(int64_t nbytes);

/* Returning mainloop: tick returns 0 to stop. On Emscripten this hooks
   emscripten_set_main_loop and does not return until cancelled. */
void jogrt_host_run_loop(int64_t (*tick)(void));
void jog_host_run_loop(int64_t (*tick)(void));

/* Stubs for symbols the IR always registers; abort if a wasm demo uses them. */
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
void *jogrt_dispatcher_pool(void);
void *jogrt_dispatcher_new(int64_t n);
void *jogrt_dispatcher_get(void);
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
