/* JOG-242: Chan / select / Task.join / cancel on the v2 freestanding runtime.
   Parks via jogrt_coro_park / unpark; main (cid 0) yield-spins like scope join.
   No libc: local memcpy/memset, buffers from jogrt_alloc. */

#include <stdint.h>

enum { OP_OK = 0, OP_CANCELLED = 1, OP_CLOSED = 2 };
enum { SEL_NONE = 0, SEL_RECV = 1, SEL_SEND = 2, SEL_DEFAULT = 3, SEL_TIMEOUT = 4, SEL_MAX = 32 };
enum { JOGRT_CORO_MAX = 32, JOGRT_CHAN_MAX = 64, JOGRT_TASK_MAX = 32 };

typedef struct Chan Chan;
typedef struct Waiter Waiter;
typedef struct Case Case;
typedef struct CoroSide CoroSide;

struct Waiter {
    int64_t coro;
    int kind;
    int select_case;
    void *data;
    uint64_t size;
    Chan *chan;
    Waiter *next;
    Waiter *prev;
    int live;
};

struct Case {
    int op;
    Chan *chan;
    void *data;
    uint64_t size;
    int64_t deadline_ms;
};

struct Chan {
    uint64_t elem;
    uint64_t cap;
    uint64_t head;
    uint64_t count;
    int closed;
    int in_use;
    unsigned char *buf;
    Waiter *send_q;
    Waiter *recv_q;
};

struct CoroSide {
    int op_status;
    int sel_status;
    int sel_chosen;
    int sel_claimed;
    int case_count;
    Case cases[SEL_MAX];
    Waiter sel_waiters[SEL_MAX];
    Chan *park_chan;
    Waiter *park_waiter;
    /* Task */
    int task_live;
    int task_done;
    int has_joiner;
    void *result;
    uint64_t result_size;
    int64_t join_waiter;
};

static Chan jogrt_chans[JOGRT_CHAN_MAX];
static CoroSide jogrt_sides[JOGRT_CORO_MAX];
static uint32_t jogrt_rng = 0xC0FFEEU;
static const char g_err_cancelled[] = "ErrCancelled";
static const char g_err_closed[] = "ErrClosed";

extern void *jogrt_alloc(int64_t nbytes);
extern void jogrt_panic(const void *msg, int64_t len);
extern void jogrt_yield(void);
extern int64_t jogrt_coro_id(void);
extern void jogrt_coro_park(void);
extern void jogrt_coro_unpark(int64_t cid);
extern int64_t jogrt_scope_cancelled(void *scope);
extern void *jogrt_scope_current(void);
extern void jogrt_sleep_ms(int64_t ms);

/* ---- freestanding helpers ---- */

static void mem_set(void *dst, int v, uint64_t n) {
    unsigned char *p = (unsigned char *)dst;
    uint64_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)v;
}

static void mem_cpy(void *dst, const void *src, uint64_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    uint64_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void panic_msg(const char *msg) {
    uint64_t n = 0;
    while (msg[n]) n++;
    jogrt_panic(msg, (int64_t)n);
}

static uint32_t next_rng(void) {
    jogrt_rng = jogrt_rng * 1103515245u + 12345u;
    return jogrt_rng;
}

static CoroSide *side_of(int64_t cid) {
    if (cid < 0 || cid >= JOGRT_CORO_MAX) panic_msg("bad coro id\n");
    return &jogrt_sides[cid];
}

static CoroSide *current_side(void) {
    return side_of(jogrt_coro_id());
}

static int coro_cancelled(void) {
    return jogrt_scope_cancelled(jogrt_scope_current()) != 0;
}

static void wake_coro(int64_t cid) {
    if (cid <= 0) return; /* main yield-spins */
    jogrt_coro_unpark(cid);
}

/* ---- wait queues ---- */

static void waiter_unlink(Chan *chan, Waiter *waiter) {
    Waiter **head;
    if (!waiter || !waiter->live) return;
    head = waiter->kind == SEL_SEND ? &chan->send_q : &chan->recv_q;
    waiter->live = 0;
    if (waiter->prev) waiter->prev->next = waiter->next;
    else if (*head == waiter) *head = waiter->next;
    if (waiter->next) waiter->next->prev = waiter->prev;
    waiter->next = 0;
    waiter->prev = 0;
}

static void waiter_push(Waiter **head, Waiter *waiter) {
    Waiter *tail;
    waiter->live = 1;
    waiter->next = 0;
    waiter->prev = 0;
    if (!*head) {
        *head = waiter;
        return;
    }
    tail = *head;
    while (tail->next) tail = tail->next;
    tail->next = waiter;
    waiter->prev = tail;
}

static void unlink_select(CoroSide *side) {
    int i;
    for (i = 0; i < side->case_count && i < SEL_MAX; i++) {
        if (side->sel_waiters[i].live && side->sel_waiters[i].chan) {
            waiter_unlink(side->sel_waiters[i].chan, &side->sel_waiters[i]);
        }
    }
    side->park_chan = 0;
    side->park_waiter = 0;
}

static int claim_select(Waiter *waiter, int status, const void *src, int copy_to_case) {
    CoroSide *side;
    Case *selected;
    if (!waiter || waiter->select_case < 0) return 0;
    side = side_of(waiter->coro);
    if (side->sel_claimed) {
        waiter_unlink(waiter->chan, waiter);
        return 0;
    }
    side->sel_claimed = 1;
    side->sel_chosen = waiter->select_case;
    side->sel_status = status;
    selected = &side->cases[waiter->select_case];
    if (copy_to_case && status == OP_OK && selected->data && src) {
        mem_cpy(selected->data, src, selected->size);
    }
    unlink_select(side);
    wake_coro(waiter->coro);
    return 1;
}

static void buf_push(Chan *chan, const void *src) {
    uint64_t i = (chan->head + chan->count) % chan->cap;
    mem_cpy(chan->buf + i * chan->elem, src, chan->elem);
    chan->count++;
}

static void buf_pop(Chan *chan, void *dst) {
    mem_cpy(dst, chan->buf + chan->head * chan->elem, chan->elem);
    chan->head = (chan->head + 1) % chan->cap;
    chan->count--;
}

static int take_sender(Chan *chan, void *dest) {
    while (chan->send_q) {
        Waiter *waiter = chan->send_q;
        CoroSide *side;
        if (!waiter->live) {
            chan->send_q = waiter->next;
            continue;
        }
        side = side_of(waiter->coro);
        if (waiter->select_case >= 0) {
            if (!claim_select(waiter, OP_OK, waiter->data, 0)) continue;
            if (dest && waiter->data) mem_cpy(dest, waiter->data, chan->elem);
            return 1;
        }
        if (dest && waiter->data) mem_cpy(dest, waiter->data, chan->elem);
        side->op_status = OP_OK;
        waiter_unlink(chan, waiter);
        side->park_chan = 0;
        side->park_waiter = 0;
        wake_coro(waiter->coro);
        return 1;
    }
    return 0;
}

static int give_receiver(Chan *chan, const void *src) {
    while (chan->recv_q) {
        Waiter *waiter = chan->recv_q;
        CoroSide *side;
        if (!waiter->live) {
            chan->recv_q = waiter->next;
            continue;
        }
        side = side_of(waiter->coro);
        if (waiter->select_case >= 0) {
            if (!claim_select(waiter, OP_OK, src, 1)) continue;
            return 1;
        }
        if (waiter->data && src) mem_cpy(waiter->data, src, chan->elem);
        side->op_status = OP_OK;
        waiter_unlink(chan, waiter);
        side->park_chan = 0;
        side->park_waiter = 0;
        wake_coro(waiter->coro);
        return 1;
    }
    return 0;
}

static void feed_senders(Chan *chan) {
    while (chan->count < chan->cap || chan->cap == 0) {
        unsigned char tmp[256];
        void *slot;
        if (chan->elem > sizeof tmp) panic_msg("channel element too large for feed\n");
        if (chan->cap == 0) {
            /* unbuffered: only pair with a waiting receiver */
            return;
        }
        if (!chan->send_q) return;
        slot = tmp;
        if (!take_sender(chan, slot)) return;
        if (chan->count < chan->cap) buf_push(chan, slot);
        else return;
    }
}

static int poll_recv(Chan *chan, void *dest) {
    if (chan->count > 0) {
        buf_pop(chan, dest);
        feed_senders(chan);
        return OP_OK;
    }
    if (take_sender(chan, dest)) return OP_OK;
    if (chan->closed) return OP_CLOSED;
    return -1;
}

static int poll_send(Chan *chan, const void *src) {
    if (chan->closed) return OP_CLOSED;
    if (give_receiver(chan, src)) return OP_OK;
    if (chan->cap > 0 && chan->count < chan->cap) {
        buf_push(chan, src);
        return OP_OK;
    }
    return -1;
}

static void park_wait(CoroSide *side) {
    int64_t cid = jogrt_coro_id();
    if (cid == 0) {
        /* Home stack: park is a no-op; yield until waiter completes. */
        while (side->park_waiter && side->park_waiter->live) {
            if (coro_cancelled()) {
                side->op_status = OP_CANCELLED;
                if (side->park_chan && side->park_waiter) {
                    waiter_unlink(side->park_chan, side->park_waiter);
                }
                side->park_chan = 0;
                side->park_waiter = 0;
                return;
            }
            jogrt_yield();
        }
        return;
    }
    jogrt_coro_park();
}

static int32_t park_op(Chan *chan, Waiter *waiter, int kind) {
    CoroSide *side = current_side();
    if (coro_cancelled()) return OP_CANCELLED;
    waiter->coro = jogrt_coro_id();
    waiter->kind = kind;
    waiter->chan = chan;
    waiter->select_case = -1;
    waiter_push(kind == SEL_SEND ? &chan->send_q : &chan->recv_q, waiter);
    side->park_chan = chan;
    side->park_waiter = waiter;
    side->op_status = OP_OK;
    park_wait(side);
    if (waiter->live) waiter_unlink(chan, waiter);
    side->park_chan = 0;
    side->park_waiter = 0;
    if (coro_cancelled() && side->op_status == OP_OK) return OP_CANCELLED;
    return (int32_t)side->op_status;
}

/* ---- public Chan API ---- */

void *jogrt_chan_init(uint64_t elem_size, uint64_t cap) {
    int i;
    Chan *chan;
    if (elem_size == 0) panic_msg("channel element size is zero\n");
    for (i = 0; i < JOGRT_CHAN_MAX; i++) {
        if (!jogrt_chans[i].in_use) {
            chan = &jogrt_chans[i];
            mem_set(chan, 0, sizeof(*chan));
            chan->in_use = 1;
            chan->elem = elem_size;
            chan->cap = cap;
            if (cap > 0) {
                chan->buf = (unsigned char *)jogrt_alloc((int64_t)(cap * elem_size));
                if (!chan->buf) panic_msg("out of memory\n");
                mem_set(chan->buf, 0, cap * elem_size);
            }
            return chan;
        }
    }
    panic_msg("channel pool exhausted\n");
    return 0;
}

void jogrt_chan_free(void *chan) {
    Chan *target = (Chan *)chan;
    if (!target) return;
    if (target->send_q || target->recv_q) return;
    target->buf = 0; /* bump heap; not returned */
    target->in_use = 0;
}

void jogrt_chan_close(void *chan) {
    Chan *target = (Chan *)chan;
    if (!target) panic_msg("channel is null\n");
    if (target->closed) return;
    target->closed = 1;
    while (target->recv_q) {
        Waiter *waiter = target->recv_q;
        CoroSide *side;
        if (!waiter->live) {
            target->recv_q = waiter->next;
            continue;
        }
        side = side_of(waiter->coro);
        if (waiter->select_case >= 0) {
            if (side->cases[waiter->select_case].data) {
                mem_set(side->cases[waiter->select_case].data, 0, side->cases[waiter->select_case].size);
            }
            if (!claim_select(waiter, OP_OK, 0, 0)) continue;
            continue;
        }
        if (waiter->data) mem_set(waiter->data, 0, target->elem);
        side->op_status = OP_CLOSED;
        waiter_unlink(target, waiter);
        side->park_chan = 0;
        side->park_waiter = 0;
        wake_coro(waiter->coro);
    }
    while (target->send_q) {
        Waiter *waiter = target->send_q;
        CoroSide *side;
        if (!waiter->live) {
            target->send_q = waiter->next;
            continue;
        }
        side = side_of(waiter->coro);
        if (waiter->select_case >= 0) {
            if (!claim_select(waiter, OP_CLOSED, 0, 0)) continue;
            continue;
        }
        side->op_status = OP_CLOSED;
        waiter_unlink(target, waiter);
        side->park_chan = 0;
        side->park_waiter = 0;
        wake_coro(waiter->coro);
    }
}

int32_t jogrt_chan_send(void *chan, const void *elem, uint64_t size) {
    Chan *target = (Chan *)chan;
    Waiter waiter;
    int ready;
    if (!target) panic_msg("channel is null\n");
    if (size != target->elem || !elem) panic_msg("channel send size does not match\n");
    if (coro_cancelled()) return OP_CANCELLED;
    ready = poll_send(target, elem);
    if (ready >= 0) return (int32_t)ready;
    mem_set(&waiter, 0, sizeof(waiter));
    waiter.data = (void *)elem;
    waiter.size = size;
    return park_op(target, &waiter, SEL_SEND);
}

int32_t jogrt_chan_recv(void *chan, void *elem, uint64_t size) {
    Chan *target = (Chan *)chan;
    Waiter waiter;
    int ready;
    if (!target) panic_msg("channel is null\n");
    if (size != target->elem || !elem) panic_msg("channel receive size does not match\n");
    mem_set(elem, 0, target->elem);
    if (coro_cancelled()) return OP_CANCELLED;
    ready = poll_recv(target, elem);
    if (ready >= 0) return (int32_t)ready;
    mem_set(&waiter, 0, sizeof(waiter));
    waiter.data = elem;
    waiter.size = size;
    return park_op(target, &waiter, SEL_RECV);
}

/* ---- select ---- */

void jogrt_select_begin(void) {
    CoroSide *side = current_side();
    side->case_count = 0;
    side->sel_chosen = -1;
    side->sel_status = OP_OK;
    side->sel_claimed = 0;
    unlink_select(side);
}

static int32_t select_add(int op, Chan *chan, void *data, uint64_t size, int64_t deadline_ms) {
    CoroSide *side = current_side();
    int idx;
    if (side->case_count >= SEL_MAX) panic_msg("too many select cases\n");
    idx = side->case_count++;
    side->cases[idx].op = op;
    side->cases[idx].chan = chan;
    side->cases[idx].data = data;
    side->cases[idx].size = size;
    side->cases[idx].deadline_ms = deadline_ms;
    return (int32_t)idx;
}

int32_t jogrt_select_add_recv(void *chan, void *dest, uint64_t size) {
    return select_add(SEL_RECV, (Chan *)chan, dest, size, 0);
}

int32_t jogrt_select_add_send(void *chan, const void *elem, uint64_t size) {
    return select_add(SEL_SEND, (Chan *)chan, (void *)elem, size, 0);
}

int32_t jogrt_select_add_default(void) {
    return select_add(SEL_DEFAULT, 0, 0, 0, 0);
}

int32_t jogrt_select_add_timeout(int64_t ms) {
    return select_add(SEL_TIMEOUT, 0, 0, 0, ms < 0 ? 0 : ms);
}

static int case_ready(Case *c) {
    if (c->op == SEL_DEFAULT) return 1;
    if (c->op == SEL_TIMEOUT) return 0; /* handled separately */
    if (c->op == SEL_RECV) return poll_recv(c->chan, c->data) >= 0;
    if (c->op == SEL_SEND) return poll_send(c->chan, c->data) >= 0;
    return 0;
}

int32_t jogrt_select_wait(void) {
    CoroSide *side = current_side();
    int ready[SEL_MAX];
    int nready;
    int i;
    int64_t timeout_ms = -1;
    int has_default = 0;
    int closed_send = 0;

    if (coro_cancelled()) {
        side->sel_status = OP_CANCELLED;
        side->sel_chosen = -1;
        return -1;
    }

    for (;;) {
        nready = 0;
        has_default = 0;
        closed_send = 0;
        timeout_ms = -1;
        for (i = 0; i < side->case_count; i++) {
            Case *c = &side->cases[i];
            if (c->op == SEL_DEFAULT) {
                has_default = 1;
                ready[nready++] = i;
                continue;
            }
            if (c->op == SEL_TIMEOUT) {
                if (timeout_ms < 0 || c->deadline_ms < timeout_ms) timeout_ms = c->deadline_ms;
                continue;
            }
            if (c->op == SEL_RECV) {
                int st = poll_recv(c->chan, c->data);
                if (st >= 0) ready[nready++] = i;
            } else if (c->op == SEL_SEND) {
                int st = poll_send(c->chan, c->data);
                if (st == OP_CLOSED) closed_send = 1;
                else if (st >= 0) ready[nready++] = i;
            }
        }

        if (nready > 0) {
            /* Prefer non-default if any; else default. Random among ready. */
            int pick = ready[0];
            int nondef = 0;
            for (i = 0; i < nready; i++) {
                if (side->cases[ready[i]].op != SEL_DEFAULT) nondef++;
            }
            if (nondef > 0) {
                int r = (int)(next_rng() % (uint32_t)nondef);
                for (i = 0; i < nready; i++) {
                    if (side->cases[ready[i]].op == SEL_DEFAULT) continue;
                    if (r == 0) {
                        pick = ready[i];
                        break;
                    }
                    r--;
                }
            } else {
                pick = ready[next_rng() % (uint32_t)nready];
            }
            {
                Case *selected = &side->cases[pick];
                int status = OP_OK;
                if (selected->op == SEL_RECV) {
                    status = poll_recv(selected->chan, selected->data);
                    if (status < 0) status = OP_OK;
                    if (status == OP_CLOSED) status = OP_OK; /* closed recv delivers zero + ok for select body */
                } else if (selected->op == SEL_SEND) {
                    status = poll_send(selected->chan, selected->data);
                    if (status < 0) status = OP_OK;
                }
                side->sel_status = status;
                side->sel_chosen = pick;
                return (int32_t)pick;
            }
        }

        if (has_default) {
            for (i = 0; i < side->case_count; i++) {
                if (side->cases[i].op == SEL_DEFAULT) {
                    side->sel_status = OP_OK;
                    side->sel_chosen = i;
                    return (int32_t)i;
                }
            }
        }

        if (closed_send && nready == 0) {
            side->sel_status = OP_CLOSED;
            side->sel_chosen = -2;
            return -2;
        }

        if (timeout_ms == 0) {
            for (i = 0; i < side->case_count; i++) {
                if (side->cases[i].op == SEL_TIMEOUT) {
                    side->sel_status = OP_OK;
                    side->sel_chosen = i;
                    return (int32_t)i;
                }
            }
        }

        /* Park on all chan cases. */
        side->sel_claimed = 0;
        for (i = 0; i < side->case_count; i++) {
            Case *c = &side->cases[i];
            Waiter *w = &side->sel_waiters[i];
            if (c->op != SEL_RECV && c->op != SEL_SEND) continue;
            mem_set(w, 0, sizeof(*w));
            w->coro = jogrt_coro_id();
            w->kind = c->op;
            w->select_case = i;
            w->data = c->data;
            w->size = c->size;
            w->chan = c->chan;
            waiter_push(c->op == SEL_SEND ? &c->chan->send_q : &c->chan->recv_q, w);
        }

        if (timeout_ms > 0) {
            /* Cooperative: sleep in small slices while yielding. */
            int64_t left = timeout_ms;
            while (left > 0 && !side->sel_claimed) {
                int64_t slice = left > 5 ? 5 : left;
                if (jogrt_coro_id() == 0) {
                    jogrt_sleep_ms(slice);
                    /* re-poll after sleep */
                    break;
                }
                jogrt_sleep_ms(slice);
                left -= slice;
                if (coro_cancelled()) break;
            }
            if (!side->sel_claimed) {
                unlink_select(side);
                for (i = 0; i < side->case_count; i++) {
                    if (side->cases[i].op == SEL_TIMEOUT) {
                        side->sel_status = OP_OK;
                        side->sel_chosen = i;
                        return (int32_t)i;
                    }
                }
            }
        } else {
            if (jogrt_coro_id() == 0) {
                while (!side->sel_claimed) {
                    if (coro_cancelled()) break;
                    jogrt_yield();
                }
            } else {
                jogrt_coro_park();
            }
        }

        unlink_select(side);
        if (side->sel_claimed) {
            return (int32_t)side->sel_chosen;
        }
        if (coro_cancelled()) {
            side->sel_status = OP_CANCELLED;
            side->sel_chosen = -1;
            return -1;
        }
        /* Retry poll loop (timeout slice expired without claim, or spurious). */
    }
}

int32_t jogrt_select_status(void) {
    return (int32_t)current_side()->sel_status;
}

int32_t jogrt_sleep(int64_t ms) {
    if (coro_cancelled()) return OP_CANCELLED;
    if (ms > 0) jogrt_sleep_ms(ms);
    if (coro_cancelled()) return OP_CANCELLED;
    return OP_OK;
}

void *jogrt_err_cancelled(void) {
    return (void *)g_err_cancelled;
}

void *jogrt_err_closed(void) {
    return (void *)g_err_closed;
}

/* ---- Task ---- */

int64_t jogrt_task_begin(int64_t cid) {
    CoroSide *side;
    if (cid <= 0 || cid >= JOGRT_CORO_MAX) panic_msg("bad task id\n");
    side = side_of(cid);
    side->task_live = 1;
    side->task_done = 0;
    side->result = 0;
    side->result_size = 0;
    /* Preserve has_joiner / join_waiter if parent already called join. */
    return cid;
}

void jogrt_task_set(const void *ptr, uint64_t size) {
    CoroSide *side = current_side();
    if (!ptr || size == 0) return;
    side->result = jogrt_alloc((int64_t)size);
    if (!side->result) panic_msg("out of memory\n");
    mem_cpy(side->result, ptr, size);
    side->result_size = size;
}

void jogrt_task_finish(void) {
    CoroSide *side = current_side();
    side->task_done = 1;
    if (side->has_joiner) {
        int64_t w = side->join_waiter;
        side->has_joiner = 0;
        wake_coro(w);
    }
}

void jogrt_task_join(int64_t task, void *out, uint64_t size) {
    CoroSide *target;
    uint64_t n;
    if (task <= 0 || task >= JOGRT_CORO_MAX) panic_msg("task join is missing a task\n");
    target = side_of(task);
    if (!target->task_done) {
        target->join_waiter = jogrt_coro_id();
        target->has_joiner = 1;
        if (jogrt_coro_id() == 0) {
            while (!target->task_done) jogrt_yield();
        } else {
            jogrt_coro_park();
        }
    }
    if (!out || size == 0) return;
    n = target->result_size;
    if (n > size) n = size;
    if (n > 0 && target->result) mem_cpy(out, target->result, n);
    if (size > n) mem_set((unsigned char *)out + n, 0, size - n);
}
