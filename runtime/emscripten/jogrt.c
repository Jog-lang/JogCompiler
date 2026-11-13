/* Jog v2 runtime for --target wasm32-emscripten (emcc + libc). */
#include "jogrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOGRT_HEAP_SIZE (4 * 1024 * 1024)

static unsigned char jogrt_heap[JOGRT_HEAP_SIZE];
static size_t jogrt_heap_used = 0;

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

static void jogrt_coro_unsupported(const char *what) {
    char buf[128];
    int n = snprintf(buf, sizeof buf, "panic: wasm32-emscripten has no %s\n", what);
    if (n < 0) n = 0;
    jogrt_panic(buf, n);
}

void jogrt_spawn(void *fn, void *arg) {
    (void)fn;
    (void)arg;
    jogrt_coro_unsupported("jogrt_spawn");
}
void jogrt_dispatcher_stub(void) {}
void jogrt_go(void *fn, void *arg) {
    (void)fn;
    (void)arg;
    jogrt_coro_unsupported("coroutine stack switch (go)");
}
void jogrt_yield(void) { jogrt_coro_unsupported("yield"); }
void jogrt_coro_drain(void) {}
int64_t jogrt_coro_alive(void) { return 0; }
int64_t jogrt_coro_id(void) { return 0; }
void jogrt_coro_park(void) {}
void jogrt_coro_unpark(int64_t id) { (void)id; }
void *jogrt_dispatcher_single(void) { return NULL; }
void *jogrt_dispatcher_pool(void) { return NULL; }
void *jogrt_dispatcher_new(int64_t n) {
    (void)n;
    return NULL;
}
void *jogrt_dispatcher_get(void) { return NULL; }
void *jogrt_dispatcher_io(void) { return NULL; }
void jogrt_dispatcher_enter(void *d) { (void)d; }
void jogrt_dispatcher_leave(void) {}
void jogrt_sleep_ms(int64_t ms) { (void)ms; }
void jogrt_print_tid(const void *ptr, int64_t len) { jogrt_print(ptr, len); }
int64_t jogrt_worker_id(void) { return 0; }
int64_t jogrt_gettid(void) { return 0; }
