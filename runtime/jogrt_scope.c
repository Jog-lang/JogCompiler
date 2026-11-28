/* JOG-222: structured concurrency helpers layered on jogrt_go / yield / park.
   Freestanding: fixed BSS pool, no libc. Scope join from main uses yield-loop
   because jogrt_coro_park is a no-op on the home (cid 0) stack. */

#include <stdint.h>

enum { JOGRT_SCOPE_MAX = 64 };

typedef struct JogrtScope {
    struct JogrtScope *parent;
    int64_t children;
    int64_t cancelled;
    int64_t in_use;
    void *err;
} JogrtScope;

static JogrtScope jogrt_scopes[JOGRT_SCOPE_MAX];
static JogrtScope jogrt_scope_root_storage;
static JogrtScope *jogrt_scope_current_ptr;
static int jogrt_scope_booted;

extern int64_t jogrt_go(void *fn, void *arg); /* returns coro/task id */
extern void jogrt_yield(void);
extern int64_t jogrt_coro_id(void);
extern void jogrt_coro_park(void);
extern void jogrt_coro_unpark(int64_t cid);
extern void jogrt_panic(const void *msg, int64_t len);
extern int64_t jogrt_task_begin(int64_t cid);
extern void jogrt_task_finish(void);

static void scope_boot(void) {
    if (jogrt_scope_booted) return;
    jogrt_scope_booted = 1;
    jogrt_scope_root_storage.parent = 0;
    jogrt_scope_root_storage.children = 0;
    jogrt_scope_root_storage.cancelled = 0;
    jogrt_scope_root_storage.in_use = 1;
    jogrt_scope_root_storage.err = 0;
    jogrt_scope_current_ptr = &jogrt_scope_root_storage;
}

void *jogrt_scope_root(void) {
    scope_boot();
    return &jogrt_scope_root_storage;
}

void *jogrt_scope_current(void) {
    scope_boot();
    return jogrt_scope_current_ptr ? jogrt_scope_current_ptr : &jogrt_scope_root_storage;
}

void *jogrt_scope_enter(void) {
    int i;
    JogrtScope *s;
    scope_boot();
    for (i = 0; i < JOGRT_SCOPE_MAX; i++) {
        if (!jogrt_scopes[i].in_use) {
            s = &jogrt_scopes[i];
            s->in_use = 1;
            s->parent = jogrt_scope_current_ptr ? jogrt_scope_current_ptr : &jogrt_scope_root_storage;
            s->children = 0;
            s->cancelled = 0;
            s->err = 0;
            jogrt_scope_current_ptr = s;
            return s;
        }
    }
    {
        static const char msg[] = "scope pool exhausted\n";
        jogrt_panic(msg, (int64_t)(sizeof msg - 1));
    }
    return 0;
}

void jogrt_scope_child_inc(void *scope) {
    JogrtScope *s = (JogrtScope *)scope;
    scope_boot();
    if (!s) s = jogrt_scope_current_ptr;
    if (!s) s = &jogrt_scope_root_storage;
    s->children++;
}

void jogrt_scope_child_done(void *scope) {
    JogrtScope *s = (JogrtScope *)scope;
    scope_boot();
    if (!s) return;
    if (s->children > 0) s->children--;
}

void *jogrt_scope_leave(void *scope) {
    JogrtScope *s = (JogrtScope *)scope;
    JogrtScope *parent;
    scope_boot();
    if (!s) return 0;
    /* Join: run other coros until this scope's children finish. */
    while (s->children > 0) {
        jogrt_yield();
    }
    parent = s->parent ? s->parent : &jogrt_scope_root_storage;
    if (jogrt_scope_current_ptr == s) {
        jogrt_scope_current_ptr = parent;
    }
    {
        void *err = s->err;
        s->in_use = 0;
        s->parent = 0;
        s->children = 0;
        s->cancelled = 0;
        s->err = 0;
        return err;
    }
}

void jogrt_scope_cancel(void *scope) {
    JogrtScope *s = (JogrtScope *)scope;
    scope_boot();
    if (!s) s = jogrt_scope_current_ptr;
    if (s) s->cancelled = 1;
}

int64_t jogrt_scope_cancelled(void *scope) {
    JogrtScope *s = (JogrtScope *)scope;
    scope_boot();
    if (!s) s = jogrt_scope_current_ptr;
    return s && s->cancelled ? 1 : 0;
}

/* Parent packs {fn, arg, scope} on its stack (lives until scope leave joins). */
typedef struct JogrtGoWrap {
    void (*fn)(void *);
    void *arg;
    void *scope;
} JogrtGoWrap;

void jogrt_scope_trampoline(void *p) {
    JogrtGoWrap *w = (JogrtGoWrap *)p;
    jogrt_task_begin(jogrt_coro_id());
    if (w && w->fn) {
        w->fn(w->arg);
    }
    jogrt_task_finish();
    if (w) {
        jogrt_scope_child_done(w->scope);
    }
}

/* Returns Task id (coro slot). */
int64_t jogrt_scope_go(void *scope, void *fn, void *arg, void *wrap) {
    JogrtGoWrap *w = (JogrtGoWrap *)wrap;
    scope_boot();
    if (!w) {
        static const char msg[] = "scope go missing wrap\n";
        jogrt_panic(msg, (int64_t)(sizeof msg - 1));
    }
    w->fn = (void (*)(void *))fn;
    w->arg = arg;
    w->scope = scope ? scope : jogrt_scope_current();
    jogrt_scope_child_inc(w->scope);
    return jogrt_go((void *)jogrt_scope_trampoline, w);
}
