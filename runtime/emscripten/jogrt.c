/* Jog v2 runtime for --target wasm32-emscripten (emcc + libc). */
#include "jogrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOGRT_HEAP_SIZE (4 * 1024 * 1024)
#define JOGRT_RET_ARENA_SIZE (1 * 1024 * 1024)

static unsigned char jogrt_heap[JOGRT_HEAP_SIZE];
static size_t jogrt_heap_used = 0;
static unsigned char jogrt_ret_arena[JOGRT_RET_ARENA_SIZE];
static size_t jogrt_ret_used = 0;

void jogrt_print(const void *ptr, int64_t len) {
    if (ptr == NULL || len <= 0) return;
    fwrite(ptr, 1, (size_t)len, stdout);
    fflush(stdout);
}

void jogrt_panic(const void *ptr, int64_t len) {
    if (ptr != NULL && len > 0) {
        fwrite(ptr, 1, (size_t)len, stderr);
    }
    abort();
}

void *jogrt_alloc(int64_t nbytes) {
    if (nbytes < 0) nbytes = 0;
    size_t n = (size_t)((nbytes + 7) & ~((int64_t)7));
    if (jogrt_heap_used + n > JOGRT_HEAP_SIZE) {
        static const char msg[] = "jogrt: out of memory\n";
        jogrt_panic(msg, (int64_t)(sizeof msg - 1));
    }
    void *p = jogrt_heap + jogrt_heap_used;
    jogrt_heap_used += n;
    return p;
}

int64_t jogrt_alloc_checkpoint(void) {
    return (int64_t)jogrt_heap_used;
}

void jogrt_alloc_reset(int64_t checkpoint) {
    if (checkpoint < 0) checkpoint = 0;
    if ((size_t)checkpoint > JOGRT_HEAP_SIZE) checkpoint = (int64_t)JOGRT_HEAP_SIZE;
    jogrt_heap_used = (size_t)checkpoint;
}

/* f64 bits in an int64 slot → f32 bit pattern (matches native jog_f32_bits). */
uint32_t jog_f32_bits(double d) {
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

uint32_t jogrt_f32_bits(int64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return jog_f32_bits(d);
}

int64_t jogrt_f32_from_bits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    double d = (double)f;
    int64_t out;
    memcpy(&out, &d, sizeof out);
    return out;
}

void *jogrt_ret_alloc(int64_t nbytes) {
    if (nbytes < 0) nbytes = 0;
    size_t n = (size_t)((nbytes + 7) & ~((int64_t)7));
    if (n > JOGRT_RET_ARENA_SIZE) n = JOGRT_RET_ARENA_SIZE;
    if (jogrt_ret_used + n > JOGRT_RET_ARENA_SIZE) jogrt_ret_used = 0;
    void *p = jogrt_ret_arena + jogrt_ret_used;
    jogrt_ret_used += n;
    return p;
}

static void jogrt_coro_unsupported(const char *what) {
    char buf[128];
    int n = snprintf(buf, sizeof buf, "panic: wasm32-emscripten has no %s\n", what);
    if (n < 0) n = 0;
    jogrt_panic(buf, n);
}

/* Jog IR→C functions are int64_t name(int64_t…); go/spawn pass one arg slot. */
typedef int64_t (*jogrt_go_fn)(int64_t);

/* Run-to-completion on the current stack: no stackful switch / steal.
   Enough for non-yielding band workers (raycaster). yield() still aborts. */
static void jogrt_call_rtc(void *fn, void *arg) {
    if (fn == NULL) return;
    jogrt_go_fn f = (jogrt_go_fn)fn;
    (void)f((int64_t)(intptr_t)arg);
}

void jogrt_spawn(void *fn, void *arg) {
    jogrt_call_rtc(fn, arg);
}
void jogrt_dispatcher_stub(void) {}
void jogrt_go(void *fn, void *arg) {
    jogrt_call_rtc(fn, arg);
}
void jogrt_yield(void) { jogrt_coro_unsupported("yield / stackful coroutine switch"); }
void jogrt_coro_drain(void) {}
int64_t jogrt_coro_alive(void) { return 0; }
int64_t jogrt_coro_id(void) { return 0; }
void jogrt_coro_park(void) {}
void jogrt_coro_unpark(int64_t id) { (void)id; }
/* Opaque non-null so callers that stash a Dispatcher handle do not NPE;
   enter/leave/pool are no-ops — single-threaded RTC only. */
static char jogrt_em_disp_single;
static char jogrt_em_disp_pool;
static char jogrt_em_disp_io;
void *jogrt_dispatcher_single(void) { return &jogrt_em_disp_single; }
void *jogrt_dispatcher_pool(int64_t n) {
    (void)n;
    return &jogrt_em_disp_pool;
}
void *jogrt_dispatcher_new(int64_t n) {
    (void)n;
    return &jogrt_em_disp_pool;
}
void *jogrt_dispatcher_get(int64_t id) {
    (void)id;
    return &jogrt_em_disp_pool;
}
void *jogrt_dispatcher_io(void) { return &jogrt_em_disp_io; }
void jogrt_dispatcher_enter(void *d) { (void)d; }
void jogrt_dispatcher_leave(void) {}
void jogrt_sleep_ms(int64_t ms) { (void)ms; }
void jogrt_print_tid(const void *ptr, int64_t len) { jogrt_print(ptr, len); }
int64_t jogrt_worker_id(void) { return 0; }
int64_t jogrt_gettid(void) { return 0; }
