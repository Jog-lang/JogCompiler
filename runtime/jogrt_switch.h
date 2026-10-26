#ifndef JOGRT_SWITCH_H
#define JOGRT_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stack switch for the V1 scheduler. The register set matches tools/coro_switch.c.
   Not a contract header. Generated programs do not include this file. */

enum { JOGRT_CTX_WORDS = 10 };

void jogrt_ctx_prepare(uint64_t *ctx, void *top);
void jogrt_ctx_switch(uint64_t *save, uint64_t *restore);

/* Reserves usable bytes plus one guard page. usable 0 means 256 KiB.
   The usable range is committed. The guard page is no-access.
   False if the OS reservation fails. */
bool jogrt_stack_alloc(size_t usable, void **base, void **top);

/* Arms the guard-page handler once. A hit on a Jog guard page panics. */
void jogrt_stack_guard_init(void);

#endif
