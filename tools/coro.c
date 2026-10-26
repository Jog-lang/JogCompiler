#include "coro.h"

#include "da.h"

#include <stdint.h>
#include <string.h>

enum { CORO_CTX_WORDS = 10 };

typedef struct CtCoroSlot {
    uint64_t ctx[CORO_CTX_WORDS];
    void *stack;
    void (*fn)(void *);
    void *arg;
    int alive;
} CtCoroSlot;

static struct {
    int ready;
    size_t current;
    CtDa slots;
} g_sched;

bool coro_stack_alloc(void **base, void **top);
void coro_stack_free(void *base);
void *coro_stack_top(void *base);
void coro_ctx_prepare(uint64_t *ctx, void *top);
void coro_ctx_switch(uint64_t *save, uint64_t *restore);

static CtCoroSlot *slot_at(size_t index) {
    return ct_da_at(&g_sched.slots, index);
}

static void switch_to(size_t index) {
    CtCoroSlot *from = slot_at(g_sched.current);
    CtCoroSlot *to = slot_at(index);
    if (!from || !to) {
        return;
    }
    /* The trampoline reads this after the jump lands. */
    g_sched.current = index;
    coro_ctx_switch(from->ctx, to->ctx);
}

/* First resume of a stack. fn runs, the slot is marked dead, then this
   yields forever so it never returns off the stack. */
void coro_trampoline(void) {
    size_t index = g_sched.current;
    CtCoroSlot *slot = slot_at(index);
    if (slot && slot->fn) {
        void (*fn)(void *) = slot->fn;
        void *arg = slot->arg;
        fn(arg);
        slot = slot_at(index);
        if (slot) {
            slot->alive = 0;
        }
    }
    for (;;) {
        coroutine_yield();
    }
}

static void arm_slot(CtCoroSlot *slot, void *stack, void *top, void (*fn)(void *), void *arg) {
    coro_ctx_prepare(slot->ctx, top);
    slot->stack = stack;
    slot->fn = fn;
    slot->arg = arg;
    slot->alive = 1;
}

bool coroutine_init(void) {
    if (g_sched.ready) {
        return true;
    }
    if (!ct_da_init(&g_sched.slots, sizeof(CtCoroSlot))) {
        return false;
    }

    CtCoroSlot main_slot;
    memset(&main_slot, 0, sizeof main_slot);
    main_slot.alive = 1;
    if (!ct_da_push(&g_sched.slots, &main_slot)) {
        ct_da_free(&g_sched.slots);
        return false;
    }

    g_sched.current = 0;
    g_sched.ready = 1;
    return true;
}

bool coroutine_go(void (*fn)(void *), void *arg) {
    if (!g_sched.ready || !fn) {
        return false;
    }

    size_t count = g_sched.slots.count;
    for (size_t index = 1; index < count; index++) {
        if (index == g_sched.current) {
            continue;
        }
        CtCoroSlot *slot = slot_at(index);
        if (!slot || slot->alive) {
            continue;
        }

        void *stack = slot->stack;
        void *top = NULL;
        if (!stack) {
            if (!coro_stack_alloc(&stack, &top)) {
                return false;
            }
            slot = slot_at(index);
            if (!slot) {
                coro_stack_free(stack);
                return false;
            }
        } else {
            top = coro_stack_top(stack);
        }
        arm_slot(slot, stack, top, fn, arg);
        return true;
    }

    void *stack = NULL;
    void *top = NULL;
    if (!coro_stack_alloc(&stack, &top)) {
        return false;
    }

    CtCoroSlot slot;
    memset(&slot, 0, sizeof slot);
    arm_slot(&slot, stack, top, fn, arg);
    if (!ct_da_push(&g_sched.slots, &slot)) {
        coro_stack_free(stack);
        return false;
    }
    return true;
}

void coroutine_yield(void) {
    if (!g_sched.ready) {
        return;
    }

    size_t count = g_sched.slots.count;
    if (count == 0) {
        return;
    }

    size_t start = g_sched.current;
    for (size_t step = 1; step < count; step++) {
        size_t index = (start + step) % count;
        CtCoroSlot *slot = slot_at(index);
        if (!slot || !slot->alive || index == start) {
            continue;
        }
        switch_to(index);
        return;
    }
}

size_t coroutine_id(void) {
    if (!g_sched.ready) {
        return 0;
    }
    return g_sched.current;
}

size_t coroutine_alive(void) {
    if (!g_sched.ready) {
        return 0;
    }
    size_t alive = 0;
    size_t count = g_sched.slots.count;
    for (size_t index = 0; index < count; index++) {
        CtCoroSlot *slot = slot_at(index);
        if (slot && slot->alive) {
            alive++;
        }
    }
    return alive;
}

void coroutine_shutdown(void) {
    if (!g_sched.ready) {
        return;
    }

    size_t current = g_sched.current;
    size_t count = g_sched.slots.count;
    for (size_t index = 0; index < count; index++) {
        if (index == current) {
            continue;
        }
        CtCoroSlot *slot = slot_at(index);
        if (!slot) {
            continue;
        }
        void *stack = slot->stack;
        slot->stack = NULL;
        slot->alive = 0;
        coro_stack_free(stack);
    }

    ct_da_free(&g_sched.slots);
    g_sched.current = 0;
    g_sched.ready = 0;
}
