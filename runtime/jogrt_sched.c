#if !defined(_WIN32)
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#endif

#include "jogrt.h"

#include "jogrt_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* Single-threaded cooperative scheduler. The host coroutine is index 0 and
   keeps the C stack. go copies nothing of the new stack until the parent
   parks. Stacks are reserved until process exit: the trampoline is still
   on a stack when that coroutine dies, so it is not freed here. */

enum {
    CORO_EMPTY = 0,
    CORO_READY = 1,
    CORO_RUNNING = 2,
    CORO_PARKED = 3,
    CORO_DEAD = 4
};

enum { PARK_NONE = 0, PARK_CHAN = 1, PARK_SCOPE = 2, PARK_SELECT = 3, PARK_SLEEP = 4, PARK_JOIN = 5 };

enum { OP_OK = 0, OP_CANCELLED = 1, OP_CLOSED = 2 };

enum { SEL_NONE = 0, SEL_RECV = 1, SEL_SEND = 2, SEL_DEFAULT = 3, SEL_TIMEOUT = 4, SEL_MAX = 32 };

enum { STARVE_MS = 50 };

typedef struct Chan Chan;
typedef struct Scope Scope;
typedef struct Coro Coro;

typedef struct Waiter {
    int coro;
    int kind;
    int select_case;
    void *data;
    size_t size;
    Chan *chan;
    struct Waiter *next;
    struct Waiter *prev;
    int live;
} Waiter;

struct Chan {
    size_t elem;
    size_t cap;
    size_t head;
    size_t count;
    int closed;
    unsigned char *buf;
    Waiter *send_q;
    Waiter *recv_q;
};

struct Scope {
    Scope *parent;
    int children;
    int cancelled;
    int waiter;
    void *err;
};

typedef struct Case {
    int op;
    Chan *chan;
    void *data;
    size_t size;
    int64_t deadline;
} Case;

struct Coro {
    int index;
    int state;
    int park;
    int op_status;
    int on_queue;
    int join_waiter;
    uint64_t ctx[JOGRT_CTX_WORDS];
    void *stack_base;
    void (*fn)(void *);
    void *arg;
    Scope *home;
    Scope *scope;
    const char *file;
    uint32_t line;
    int64_t started_ms;
    int has_deadline;
    int64_t deadline;
    void *result;
    size_t result_size;
    Case cases[SEL_MAX];
    int case_count;
    int sel_chosen;
    int sel_status;
    int sel_claimed;
    Waiter sel_waiters[SEL_MAX];
    Chan *park_chan;
    Waiter *park_waiter;
};

static Coro **g_coros;
static int g_count;
static int g_cap;
static int g_current;
static int *g_q;
static int g_q_head;
static int g_q_tail;
static int g_q_count;
static int g_q_cap;
static Scope g_root;
static Scope *g_current_scope;
static int g_booted;
static uint32_t g_rng = 0xC0FFEEU;
static const char g_err_cancelled[] = "ErrCancelled";
static const char g_err_closed[] = "ErrClosed";

static void schedule(void);
static void wake(int index);
static int scope_cancelled(Scope *scope);

static void panic_msg(const char *msg) {
    jogrt_panic(msg, msg ? strlen(msg) : 0, "jogrt", 0);
}

static int64_t jogrt_now_ms(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int64_t ms) {
    if (ms <= 0) {
        return;
    }
#if defined(_WIN32)
    if (ms > 2147483647) {
        ms = 2147483647;
    }
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

static uint32_t jogrt_rand(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static Coro *current(void) {
    if (!g_booted || g_current < 0 || g_current >= g_count) {
        panic_msg("scheduler is not running");
    }
    return g_coros[g_current];
}

static int coro_cancelled(const Coro *coro) {
    if (!coro) {
        return 0;
    }
    return scope_cancelled(coro->scope) || scope_cancelled(coro->home);
}

static int scope_cancelled(Scope *scope) {
    while (scope) {
        if (scope->cancelled) {
            return 1;
        }
        if (scope == &g_root) {
            break;
        }
        scope = scope->parent;
    }
    return 0;
}

static int scope_under(Scope *scope, Scope *ancestor) {
    while (scope) {
        if (scope == ancestor) {
            return 1;
        }
        if (scope == &g_root) {
            break;
        }
        scope = scope->parent;
    }
    return 0;
}

static void note_suspend(Coro *coro) {
    int64_t now = jogrt_now_ms();
    if (coro->started_ms > 0 && now - coro->started_ms > STARVE_MS) {
        fprintf(stderr, "warning: coroutine ran longer than %d ms without suspending", STARVE_MS);
        if (coro->file && coro->file[0] != '\0') {
            fprintf(stderr, " (launched at %s:%u)", coro->file, coro->line);
        }
        fprintf(stderr, "\n");
    }
}

static void enqueue(int index) {
    Coro *coro;
    if (index < 0 || index >= g_count) {
        return;
    }
    coro = g_coros[index];
    if (!coro || coro->on_queue || coro->state == CORO_DEAD) {
        return;
    }
    if (g_q_count == g_q_cap) {
        int ncap = g_q_cap > 0 ? g_q_cap * 2 : 16;
        int *fresh = (int *)malloc((size_t)ncap * sizeof(int));
        int i;
        if (!fresh) {
            panic_msg("out of memory");
        }
        for (i = 0; i < g_q_count; i++) {
            fresh[i] = g_q[(g_q_head + i) % g_q_cap];
        }
        free(g_q);
        g_q = fresh;
        g_q_head = 0;
        g_q_tail = g_q_count;
        g_q_cap = ncap;
    }
    g_q[g_q_tail] = index;
    g_q_tail = (g_q_tail + 1) % g_q_cap;
    g_q_count++;
    coro->on_queue = 1;
}

static int dequeue(void) {
    while (g_q_count > 0) {
        int index = g_q[g_q_head];
        Coro *coro;
        g_q_head = (g_q_head + 1) % g_q_cap;
        g_q_count--;
        coro = g_coros[index];
        coro->on_queue = 0;
        if (coro->state == CORO_READY) {
            return index;
        }
    }
    return -1;
}

static void switch_to(int next) {
    int src = g_current;
    Coro *from;
    Coro *to;
    if (next < 0 || next >= g_count) {
        return;
    }
    from = g_coros[src];
    to = g_coros[next];
    from->scope = g_current_scope;
    if (src == next) {
        to->state = CORO_RUNNING;
        to->started_ms = jogrt_now_ms();
        g_current_scope = to->scope ? to->scope : &g_root;
        return;
    }
    to->state = CORO_RUNNING;
    to->started_ms = jogrt_now_ms();
    g_current = next;
    g_current_scope = to->scope ? to->scope : &g_root;
    jogrt_ctx_switch(from->ctx, to->ctx);
    g_current_scope = g_coros[g_current]->scope ? g_coros[g_current]->scope : &g_root;
    g_coros[g_current]->started_ms = jogrt_now_ms();
}

static void wake(int index) {
    Coro *coro;
    if (index < 0 || index >= g_count) {
        return;
    }
    coro = g_coros[index];
    if (!coro || coro->state != CORO_PARKED) {
        return;
    }
    coro->state = CORO_READY;
    enqueue(index);
}

static void waiter_unlink(Chan *chan, Waiter *waiter) {
    Waiter **head;
    if (!waiter || !waiter->live) {
        return;
    }
    head = waiter->kind == SEL_SEND ? &chan->send_q : &chan->recv_q;
    waiter->live = 0;
    if (waiter->prev) {
        waiter->prev->next = waiter->next;
    } else if (*head == waiter) {
        *head = waiter->next;
    }
    if (waiter->next) {
        waiter->next->prev = waiter->prev;
    }
    waiter->next = NULL;
    waiter->prev = NULL;
}

static void waiter_push(Waiter **head, Waiter *waiter) {
    Waiter *tail;
    waiter->live = 1;
    waiter->next = NULL;
    waiter->prev = NULL;
    if (!*head) {
        *head = waiter;
        return;
    }
    tail = *head;
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = waiter;
    waiter->prev = tail;
}

static void unlink_select(Coro *coro) {
    int i;
    int n = coro->case_count;
    if (n > SEL_MAX) {
        n = SEL_MAX;
    }
    for (i = 0; i < n; i++) {
        if (coro->sel_waiters[i].live && coro->sel_waiters[i].chan) {
            waiter_unlink(coro->sel_waiters[i].chan, &coro->sel_waiters[i]);
        }
    }
    coro->park_chan = NULL;
    coro->park_waiter = NULL;
}

static void cancel_coro(int index) {
    Coro *coro = g_coros[index];
    if (!coro || coro->state != CORO_PARKED) {
        return;
    }
    if (coro->park == PARK_SELECT && coro->sel_claimed) {
        return;
    }
    coro->op_status = OP_CANCELLED;
    if (coro->park == PARK_SELECT) {
        coro->sel_chosen = -1;
        coro->sel_status = OP_CANCELLED;
        unlink_select(coro);
    }
    if (coro->park_chan && coro->park_waiter) {
        waiter_unlink(coro->park_chan, coro->park_waiter);
        coro->park_chan = NULL;
        coro->park_waiter = NULL;
    }
    wake(index);
}

static int claim_select(Waiter *waiter, int status, const void *src, int copy_to_case) {
    Coro *coro;
    Case *selected;
    if (!waiter || waiter->select_case < 0) {
        return 0;
    }
    coro = g_coros[waiter->coro];
    if (coro->sel_claimed) {
        waiter_unlink(waiter->chan, waiter);
        return 0;
    }
    coro->sel_claimed = 1;
    coro->sel_chosen = waiter->select_case;
    coro->sel_status = status;
    selected = &coro->cases[waiter->select_case];
    if (copy_to_case && status == OP_OK && selected->data && src) {
        memcpy(selected->data, src, selected->size);
    }
    unlink_select(coro);
    wake(waiter->coro);
    return 1;
}

static void buf_push(Chan *chan, const void *src) {
    size_t index = (chan->head + chan->count) % chan->cap;
    memcpy(chan->buf + index * chan->elem, src, chan->elem);
    chan->count++;
}

static void buf_pop(Chan *chan, void *dst) {
    memcpy(dst, chan->buf + chan->head * chan->elem, chan->elem);
    chan->head = (chan->head + 1) % chan->cap;
    chan->count--;
}

static int take_sender(Chan *chan, void *dest) {
    while (chan->send_q) {
        Waiter *waiter = chan->send_q;
        Coro *coro;
        if (!waiter->live) {
            chan->send_q = waiter->next;
            if (chan->send_q) {
                chan->send_q->prev = NULL;
            }
            continue;
        }
        coro = g_coros[waiter->coro];
        if (waiter->select_case >= 0) {
            Case *selected = &coro->cases[waiter->select_case];
            if (coro->sel_claimed) {
                waiter_unlink(chan, waiter);
                continue;
            }
            if (dest && selected->data) {
                memcpy(dest, selected->data, chan->elem);
            }
            if (!claim_select(waiter, OP_OK, NULL, 0)) {
                continue;
            }
            return 1;
        }
        if (dest && waiter->data) {
            memcpy(dest, waiter->data, chan->elem);
        }
        coro->op_status = OP_OK;
        waiter_unlink(chan, waiter);
        coro->park_chan = NULL;
        coro->park_waiter = NULL;
        wake(waiter->coro);
        return 1;
    }
    return 0;
}

static int give_receiver(Chan *chan, const void *src) {
    while (chan->recv_q) {
        Waiter *waiter = chan->recv_q;
        Coro *coro;
        if (!waiter->live) {
            chan->recv_q = waiter->next;
            if (chan->recv_q) {
                chan->recv_q->prev = NULL;
            }
            continue;
        }
        coro = g_coros[waiter->coro];
        if (waiter->select_case >= 0) {
            if (coro->sel_claimed) {
                waiter_unlink(chan, waiter);
                continue;
            }
            if (!claim_select(waiter, OP_OK, src, 1)) {
                continue;
            }
            return 1;
        }
        if (waiter->data && src) {
            memcpy(waiter->data, src, chan->elem);
        }
        coro->op_status = OP_OK;
        waiter_unlink(chan, waiter);
        coro->park_chan = NULL;
        coro->park_waiter = NULL;
        wake(waiter->coro);
        return 1;
    }
    return 0;
}

static void feed_senders(Chan *chan) {
    while (chan->cap > 0 && chan->count < chan->cap && chan->send_q) {
        void *tmp = malloc(chan->elem);
        if (!tmp) {
            panic_msg("out of memory");
        }
        if (!take_sender(chan, tmp)) {
            free(tmp);
            break;
        }
        buf_push(chan, tmp);
        free(tmp);
    }
}

static int poll_recv(Chan *chan, void *dest) {
    if (chan->count > 0) {
        if (!dest) {
            panic_msg("channel receive is missing a destination");
        }
        buf_pop(chan, dest);
        feed_senders(chan);
        return OP_OK;
    }
    if (chan->send_q && take_sender(chan, dest)) {
        return OP_OK;
    }
    if (chan->closed) {
        if (dest) {
            memset(dest, 0, chan->elem);
        }
        return OP_CLOSED;
    }
    return -1;
}

static int poll_send(Chan *chan, const void *src) {
    if (chan->closed) {
        return OP_CLOSED;
    }
    if (give_receiver(chan, src)) {
        return OP_OK;
    }
    if (chan->cap > 0 && chan->count < chan->cap) {
        buf_push(chan, src);
        return OP_OK;
    }
    return -1;
}

static void finish_coro(int index) {
    Coro *coro = g_coros[index];
    Scope *scope;
    note_suspend(coro);
    coro->state = CORO_DEAD;
    scope = coro->home ? coro->home : coro->scope;
    if (scope) {
        if (scope->children > 0) {
            scope->children--;
        }
        if (scope->children == 0 && scope->waiter >= 0) {
            int who = scope->waiter;
            scope->waiter = -1;
            wake(who);
        }
    }
    if (coro->join_waiter >= 0) {
        int who = coro->join_waiter;
        coro->join_waiter = -1;
        wake(who);
    }
}

static void wake_due(int64_t now) {
    int i;
    for (i = 0; i < g_count; i++) {
        Coro *coro = g_coros[i];
        int ids[SEL_MAX];
        int n = 0;
        int c;
        if (!coro || coro->state != CORO_PARKED || !coro->has_deadline || coro->deadline > now) {
            continue;
        }
        if (coro->park == PARK_SELECT && !coro->sel_claimed) {
            for (c = 0; c < coro->case_count; c++) {
                if (coro->cases[c].op == SEL_TIMEOUT && coro->cases[c].deadline <= now) {
                    ids[n++] = c;
                }
            }
            if (n > 0) {
                coro->sel_chosen = ids[jogrt_rand() % (uint32_t)n];
                coro->sel_status = OP_OK;
                coro->sel_claimed = 1;
                unlink_select(coro);
            }
        }
        wake(i);
    }
}

static void schedule(void) {
    for (;;) {
        int next = dequeue();
        int64_t soon = 0;
        int have_soon = 0;
        int any_park = 0;
        int i;
        if (next >= 0) {
            switch_to(next);
            return;
        }
        wake_due(jogrt_now_ms());
        next = dequeue();
        if (next >= 0) {
            switch_to(next);
            return;
        }
        for (i = 0; i < g_count; i++) {
            Coro *coro = g_coros[i];
            if (!coro || coro->state != CORO_PARKED) {
                continue;
            }
            any_park = 1;
            if (coro->has_deadline && (!have_soon || coro->deadline < soon)) {
                soon = coro->deadline;
                have_soon = 1;
            }
        }
        if (have_soon) {
            int64_t now = jogrt_now_ms();
            if (soon > now) {
                sleep_ms(soon - now);
            }
            continue;
        }
        if (any_park) {
            panic_msg("deadlock: no runnable coroutine");
        }
        panic_msg("deadlock: no runnable coroutine");
    }
}

static void park_current(void) {
    Coro *coro = current();
    note_suspend(coro);
    coro->state = CORO_PARKED;
    schedule();
}

static Coro *coro_alloc(void) {
    Coro *coro = (Coro *)calloc(1, sizeof(Coro));
    if (!coro) {
        panic_msg("out of memory");
    }
    if (g_count == g_cap) {
        int ncap = g_cap > 0 ? g_cap * 2 : 8;
        Coro **fresh = (Coro **)realloc(g_coros, (size_t)ncap * sizeof(Coro *));
        if (!fresh) {
            panic_msg("out of memory");
        }
        g_coros = fresh;
        g_cap = ncap;
    }
    coro->index = g_count;
    coro->join_waiter = -1;
    coro->state = CORO_EMPTY;
    g_coros[g_count++] = coro;
    return coro;
}

void jogrt_sched_boot(void) {
    Coro *main_coro;
    if (g_booted) {
        return;
    }
    g_booted = 1;
    g_root.parent = NULL;
    g_root.children = 0;
    g_root.cancelled = 0;
    g_root.waiter = -1;
    g_root.err = NULL;
    g_current_scope = &g_root;
    main_coro = coro_alloc();
    main_coro->state = CORO_RUNNING;
    main_coro->home = &g_root;
    main_coro->scope = &g_root;
    main_coro->started_ms = jogrt_now_ms();
    g_current = main_coro->index;
}

void jogrt_trampoline(void) {
    Coro *coro = current();
    if (coro->fn) {
        coro->fn(coro->arg);
    }
    finish_coro(coro->index);
    for (;;) {
        schedule();
    }
}

void *jogrt_scope_root(void) {
    jogrt_sched_boot();
    return &g_root;
}

void *jogrt_scope_current(void) {
    jogrt_sched_boot();
    return g_current_scope ? g_current_scope : &g_root;
}

void *jogrt_scope_enter(void *parent) {
    Scope *scope;
    jogrt_sched_boot();
    scope = (Scope *)calloc(1, sizeof(Scope));
    if (!scope) {
        panic_msg("out of memory");
    }
    scope->parent = parent ? (Scope *)parent : (g_current_scope ? g_current_scope : &g_root);
    scope->waiter = -1;
    g_current_scope = scope;
    current()->scope = scope;
    return scope;
}

void *jogrt_scope_leave(void *scope) {
    Scope *leaving = (Scope *)scope;
    Scope *parent;
    jogrt_sched_boot();
    if (!leaving) {
        return NULL;
    }
    if (leaving->children > 0) {
        leaving->waiter = g_current;
        current()->park = PARK_SCOPE;
        current()->has_deadline = 0;
        park_current();
    }
    parent = leaving->parent ? leaving->parent : &g_root;
    if (g_current_scope == leaving) {
        g_current_scope = parent;
        current()->scope = parent;
    }
    return leaving->err;
}

void jogrt_scope_cancel(void *scope) {
    Scope *target;
    int i;
    jogrt_sched_boot();
    target = scope ? (Scope *)scope : g_current_scope;
    if (!target) {
        return;
    }
    target->cancelled = 1;
    for (i = 0; i < g_count; i++) {
        Coro *coro = g_coros[i];
        if (coro && (scope_under(coro->scope, target) || scope_under(coro->home, target))) {
            cancel_coro(i);
        }
    }
}

void jogrt_scope_join(void *scope) {
    Scope *target;
    jogrt_sched_boot();
    target = scope ? (Scope *)scope : &g_root;
    if (target->children > 0) {
        target->waiter = g_current;
        current()->park = PARK_SCOPE;
        current()->has_deadline = 0;
        park_current();
    }
}

void jogrt_scope_fail(void *err) {
    Scope *scope;
    jogrt_sched_boot();
    scope = g_current_scope;
    if (!scope || !err || scope->err) {
        return;
    }
    scope->err = err;
    jogrt_scope_cancel(scope);
}

void *jogrt_go(void (*fn)(void *), void *env, uintptr_t stack_bytes, void *scope, const void *file, uint32_t line) {
    Coro *coro;
    Scope *home;
    void *base = NULL;
    void *top = NULL;
    jogrt_sched_boot();
    if (!fn) {
        panic_msg("go is missing a function");
    }
    if (!jogrt_stack_alloc((size_t)stack_bytes, &base, &top)) {
        panic_msg("out of memory");
    }
    home = scope ? (Scope *)scope : (g_current_scope ? g_current_scope : &g_root);
    coro = coro_alloc();
    coro->state = CORO_READY;
    coro->fn = fn;
    coro->arg = env;
    coro->home = home;
    coro->scope = home;
    coro->stack_base = base;
    coro->file = file ? (const char *)file : NULL;
    coro->line = line;
    jogrt_ctx_prepare(coro->ctx, top);
    home->children++;
    enqueue(coro->index);
    return coro;
}

void jogrt_yield(void) {
    Coro *coro;
    jogrt_sched_boot();
    coro = current();
    note_suspend(coro);
    coro->state = CORO_READY;
    coro->park = PARK_NONE;
    coro->has_deadline = 0;
    enqueue(coro->index);
    schedule();
}

void jogrt_task_set(const void *ptr, uintptr_t size) {
    Coro *coro;
    void *buf;
    jogrt_sched_boot();
    if (!ptr || size == 0) {
        return;
    }
    coro = current();
    buf = malloc((size_t)size);
    if (!buf) {
        panic_msg("out of memory");
    }
    memcpy(buf, ptr, (size_t)size);
    free(coro->result);
    coro->result = buf;
    coro->result_size = (size_t)size;
}

void jogrt_task_join(void *task, void *out, uintptr_t size) {
    Coro *target = (Coro *)task;
    size_t n;
    jogrt_sched_boot();
    if (!target) {
        panic_msg("task join is missing a task");
    }
    if (target->state != CORO_DEAD) {
        target->join_waiter = g_current;
        current()->park = PARK_JOIN;
        current()->has_deadline = 0;
        park_current();
    }
    if (!out || size == 0) {
        return;
    }
    n = target->result_size;
    if (n > (size_t)size) {
        n = (size_t)size;
    }
    if (n > 0 && target->result) {
        memcpy(out, target->result, n);
    }
    if ((size_t)size > n) {
        memset((unsigned char *)out + n, 0, (size_t)size - n);
    }
}

static Chan *chan_of(void *chan) {
    Chan *out = (Chan *)chan;
    if (!out) {
        panic_msg("channel is null");
    }
    return out;
}

void *jogrt_chan_init(uintptr_t elem_size, uintptr_t cap) {
    Chan *chan;
    jogrt_sched_boot();
    if (elem_size == 0) {
        panic_msg("channel element size is zero");
    }
    if (cap > SIZE_MAX / (size_t)elem_size) {
        panic_msg("channel is too large");
    }
    chan = (Chan *)calloc(1, sizeof(Chan));
    if (!chan) {
        panic_msg("out of memory");
    }
    chan->elem = (size_t)elem_size;
    chan->cap = (size_t)cap;
    if (cap > 0) {
        chan->buf = (unsigned char *)calloc((size_t)cap, (size_t)elem_size);
        if (!chan->buf) {
            panic_msg("out of memory");
        }
    }
    return chan;
}

void jogrt_chan_free(void *chan) {
    Chan *target = (Chan *)chan;
    if (!target) {
        return;
    }
    if (target->send_q || target->recv_q) {
        return;
    }
    free(target->buf);
    free(target);
}

void jogrt_chan_close(void *chan) {
    Chan *target;
    jogrt_sched_boot();
    target = chan_of(chan);
    if (target->closed) {
        return;
    }
    target->closed = 1;
    while (target->recv_q) {
        Waiter *waiter = target->recv_q;
        Coro *coro;
        if (!waiter->live) {
            target->recv_q = waiter->next;
            continue;
        }
        coro = g_coros[waiter->coro];
        if (waiter->select_case >= 0) {
            Case *selected = &coro->cases[waiter->select_case];
            if (selected->data) {
                memset(selected->data, 0, selected->size);
            }
            if (!claim_select(waiter, OP_OK, NULL, 0)) {
                continue;
            }
            continue;
        }
        if (waiter->data) {
            memset(waiter->data, 0, target->elem);
        }
        coro->op_status = OP_CLOSED;
        waiter_unlink(target, waiter);
        coro->park_chan = NULL;
        coro->park_waiter = NULL;
        wake(waiter->coro);
    }
    while (target->send_q) {
        Waiter *waiter = target->send_q;
        Coro *coro;
        if (!waiter->live) {
            target->send_q = waiter->next;
            continue;
        }
        coro = g_coros[waiter->coro];
        if (waiter->select_case >= 0) {
            if (!claim_select(waiter, OP_CLOSED, NULL, 0)) {
                continue;
            }
            continue;
        }
        coro->op_status = OP_CLOSED;
        waiter_unlink(target, waiter);
        coro->park_chan = NULL;
        coro->park_waiter = NULL;
        wake(waiter->coro);
    }
}

static int32_t park_op(Chan *chan, Waiter *waiter, int kind) {
    Coro *coro = current();
    if (coro_cancelled(coro)) {
        return OP_CANCELLED;
    }
    waiter->coro = coro->index;
    waiter->kind = kind;
    waiter->chan = chan;
    waiter->select_case = -1;
    waiter_push(kind == SEL_SEND ? &chan->send_q : &chan->recv_q, waiter);
    coro->park = PARK_CHAN;
    coro->park_chan = chan;
    coro->park_waiter = waiter;
    coro->has_deadline = 0;
    coro->op_status = OP_OK;
    park_current();
    if (waiter->live) {
        waiter_unlink(chan, waiter);
    }
    coro->park_chan = NULL;
    coro->park_waiter = NULL;
    if (coro_cancelled(coro) && coro->op_status == OP_OK) {
        return OP_CANCELLED;
    }
    return coro->op_status;
}

int32_t jogrt_chan_send(void *chan, const void *elem, uintptr_t size) {
    Chan *target = chan_of(chan);
    Waiter waiter;
    int ready;
    jogrt_sched_boot();
    if ((size_t)size != target->elem || !elem) {
        panic_msg("channel send size does not match");
    }
    if (coro_cancelled(current())) {
        return OP_CANCELLED;
    }
    ready = poll_send(target, elem);
    if (ready >= 0) {
        return ready;
    }
    memset(&waiter, 0, sizeof(waiter));
    waiter.data = (void *)elem;
    waiter.size = (size_t)size;
    return park_op(target, &waiter, SEL_SEND);
}

int32_t jogrt_chan_recv(void *chan, void *elem, uintptr_t size) {
    Chan *target = chan_of(chan);
    Waiter waiter;
    int ready;
    jogrt_sched_boot();
    if ((size_t)size != target->elem || !elem) {
        panic_msg("channel receive size does not match");
    }
    if (elem) {
        memset(elem, 0, target->elem);
    }
    if (coro_cancelled(current())) {
        return OP_CANCELLED;
    }
    ready = poll_recv(target, elem);
    if (ready >= 0) {
        return ready;
    }
    memset(&waiter, 0, sizeof(waiter));
    waiter.data = elem;
    waiter.size = (size_t)size;
    return park_op(target, &waiter, SEL_RECV);
}

static Coro *select_coro(void) {
    jogrt_sched_boot();
    return current();
}

static int select_add(Coro *coro, int op, Chan *chan, void *data, size_t size, int64_t deadline) {
    int index;
    if (coro->case_count >= SEL_MAX) {
        panic_msg("too many select cases");
    }
    index = coro->case_count++;
    coro->cases[index].op = op;
    coro->cases[index].chan = chan;
    coro->cases[index].data = data;
    coro->cases[index].size = size;
    coro->cases[index].deadline = deadline;
    return index;
}

void jogrt_select_begin(void) {
    Coro *coro = select_coro();
    int i;
    unlink_select(coro);
    coro->case_count = 0;
    coro->sel_chosen = -1;
    coro->sel_status = OP_OK;
    coro->sel_claimed = 0;
    coro->has_deadline = 0;
    for (i = 0; i < SEL_MAX; i++) {
        coro->sel_waiters[i].live = 0;
        coro->sel_waiters[i].next = NULL;
        coro->sel_waiters[i].prev = NULL;
        coro->sel_waiters[i].chan = NULL;
    }
}

int32_t jogrt_select_add_recv(void *chan, void *dest, uintptr_t size) {
    Chan *target = chan_of(chan);
    if ((size_t)size != target->elem || !dest) {
        panic_msg("channel receive size does not match");
    }
    return select_add(select_coro(), SEL_RECV, target, dest, (size_t)size, 0);
}

int32_t jogrt_select_add_send(void *chan, const void *elem, uintptr_t size) {
    Chan *target = chan_of(chan);
    if ((size_t)size != target->elem || !elem) {
        panic_msg("channel send size does not match");
    }
    return select_add(select_coro(), SEL_SEND, target, (void *)elem, (size_t)size, 0);
}

int32_t jogrt_select_add_default(void) {
    return select_add(select_coro(), SEL_DEFAULT, NULL, NULL, 0, 0);
}

int32_t jogrt_select_add_timeout(int64_t ms) {
    int64_t now = jogrt_now_ms();
    int64_t deadline = now;
    if (ms > 0 && now <= INT64_MAX - ms) {
        deadline = now + ms;
    }
    return select_add(select_coro(), SEL_TIMEOUT, NULL, NULL, 0, deadline);
}

static int case_ready(Case *selected, int64_t now) {
    if (selected->op == SEL_RECV) {
        Chan *chan = selected->chan;
        return chan->count > 0 || chan->send_q != NULL || chan->closed;
    }
    if (selected->op == SEL_SEND) {
        Chan *chan = selected->chan;
        return !chan->closed && (chan->count < chan->cap || chan->recv_q != NULL);
    }
    if (selected->op == SEL_TIMEOUT) {
        return selected->deadline <= now;
    }
    return 0;
}

int32_t jogrt_select_wait(void) {
    Coro *coro = select_coro();
    int ready[SEL_MAX];
    int nready = 0;
    int def = -1;
    int closed_send = 0;
    int parkable = 0;
    int i;
    int64_t now;
    int chosen;
    if (coro_cancelled(coro)) {
        coro->sel_status = OP_CANCELLED;
        return -1;
    }
    now = jogrt_now_ms();
    for (i = 0; i < coro->case_count; i++) {
        Case *selected = &coro->cases[i];
        if (selected->op == SEL_DEFAULT) {
            def = i;
            continue;
        }
        if (selected->op == SEL_SEND && selected->chan && selected->chan->closed) {
            closed_send = 1;
            continue;
        }
        if (case_ready(selected, now)) {
            ready[nready++] = i;
        } else if (selected->op == SEL_RECV || selected->op == SEL_SEND || selected->op == SEL_TIMEOUT) {
            parkable = 1;
        }
    }
    if (nready > 0) {
        Case *selected;
        int status;
        chosen = ready[jogrt_rand() % (uint32_t)nready];
        selected = &coro->cases[chosen];
        status = OP_OK;
        if (selected->op == SEL_RECV) {
            status = poll_recv(selected->chan, selected->data);
            if (status < 0) {
                status = OP_OK;
            }
            if (status == OP_CLOSED) {
                status = OP_OK;
            }
        } else if (selected->op == SEL_SEND) {
            status = poll_send(selected->chan, selected->data);
            if (status < 0) {
                status = OP_OK;
            }
        }
        coro->sel_chosen = chosen;
        coro->sel_status = status;
        coro->sel_claimed = 1;
        return chosen;
    }
    if (def >= 0) {
        coro->sel_chosen = def;
        coro->sel_status = OP_OK;
        coro->sel_claimed = 1;
        return def;
    }
    if (!parkable) {
        coro->sel_status = closed_send ? OP_CLOSED : OP_CANCELLED;
        return closed_send ? -2 : -1;
    }
    coro->has_deadline = 0;
    for (i = 0; i < coro->case_count; i++) {
        Case *selected = &coro->cases[i];
        Waiter *waiter = &coro->sel_waiters[i];
        if (selected->op == SEL_TIMEOUT) {
            if (!coro->has_deadline || selected->deadline < coro->deadline) {
                coro->deadline = selected->deadline;
                coro->has_deadline = 1;
            }
            continue;
        }
        if (selected->op != SEL_RECV && selected->op != SEL_SEND) {
            continue;
        }
        if (selected->op == SEL_SEND && selected->chan->closed) {
            continue;
        }
        memset(waiter, 0, sizeof(*waiter));
        waiter->coro = coro->index;
        waiter->kind = selected->op;
        waiter->select_case = i;
        waiter->data = selected->data;
        waiter->size = selected->size;
        waiter->chan = selected->chan;
        waiter_push(selected->op == SEL_SEND ? &selected->chan->send_q : &selected->chan->recv_q, waiter);
    }
    coro->park = PARK_SELECT;
    coro->sel_claimed = 0;
    coro->sel_chosen = -1;
    park_current();
    unlink_select(coro);
    coro->has_deadline = 0;
    if (!coro->sel_claimed || coro->sel_chosen < 0) {
        coro->sel_status = OP_CANCELLED;
        return -1;
    }
    return coro->sel_chosen;
}

int32_t jogrt_select_status(void) {
    return select_coro()->sel_status;
}

int32_t jogrt_sleep(int64_t ms) {
    Coro *coro;
    jogrt_sched_boot();
    coro = current();
    if (coro_cancelled(coro)) {
        return OP_CANCELLED;
    }
    if (ms <= 0) {
        jogrt_yield();
        return coro_cancelled(coro) ? OP_CANCELLED : OP_OK;
    }
    coro->deadline = jogrt_now_ms();
    if (coro->deadline <= INT64_MAX - ms) {
        coro->deadline += ms;
    }
    coro->has_deadline = 1;
    coro->park = PARK_SLEEP;
    park_current();
    coro->has_deadline = 0;
    return coro_cancelled(coro) ? OP_CANCELLED : OP_OK;
}

void *jogrt_err_cancelled(void) {
    return (void *)g_err_cancelled;
}

void *jogrt_err_closed(void) {
    return (void *)g_err_closed;
}

void *jogrt_cap_new(uintptr_t count) {
    void **cap;
    if (count == 0) {
        return NULL;
    }
    if (count > SIZE_MAX / sizeof(void *)) {
        panic_msg("capture is too large");
    }
    cap = (void **)calloc((size_t)count, sizeof(void *));
    if (!cap) {
        panic_msg("out of memory");
    }
    return cap;
}

void jogrt_cap_set(void *cap, uintptr_t index, void *ptr) {
    void **slots = (void **)cap;
    if (!slots) {
        panic_msg("capture is null");
    }
    slots[index] = ptr;
}

void *jogrt_cap_get(void *cap, uintptr_t index) {
    void **slots = (void **)cap;
    if (!slots) {
        panic_msg("capture is null");
    }
    return slots[index];
}
