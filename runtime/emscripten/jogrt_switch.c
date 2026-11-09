#include "../jogrt_switch.h"

#include <stdio.h>
#include <stdlib.h>

/* wasm32-emscripten has no x86_64 coroutine switch. Programs that do not
   call go/yield link fine; a switch attempt aborts. */

void jogrt_ctx_prepare(uint64_t *ctx, void *top) {
    (void)ctx;
    (void)top;
}

void jogrt_ctx_switch(uint64_t *save, uint64_t *restore) {
    (void)save;
    (void)restore;
    fputs("panic: wasm32-emscripten has no coroutine stack switch\n", stderr);
    abort();
}

bool jogrt_stack_alloc(size_t usable, void **base, void **top) {
    (void)usable;
    if (base) {
        *base = NULL;
    }
    if (top) {
        *top = NULL;
    }
    return false;
}

void jogrt_stack_guard_init(void) {}
