/* JOG-233: debug race detector (first slice).
   Vector clocks keyed by coroutine id detect unsynchronized conflicting
   accesses across coroutines (cooperative interleaving and multi-worker).
   Enabled by jogrt_race_enable() — compiler --race calls it from main and
   instruments loads/stores. Chan send/recv publish/acquire happens-before.
   Freestanding: no libc; reports via jogrt_panic. */

#include <stdint.h>

enum { RACE_CORO_MAX = 32, RACE_SHADOW = 2048, RACE_SYNC = 256 };

typedef struct {
    uint64_t c[RACE_CORO_MAX];
} VClock;

typedef struct {
    uintptr_t addr; /* 0 = empty; word-aligned key */
    VClock w_vc;
    int32_t w_cid;
    int w_valid;
    VClock r_vc;
    int32_t r_cid;
    int r_valid;
} Shadow;

typedef struct {
    uintptr_t key; /* 0 = empty */
    VClock vc;
} SyncSlot;

static int race_enabled;
static int race_expect; /* selftest: record instead of panic */
static int race_hit;
static int32_t race_override_cid = -1; /* >=0 forces actor id (selftest) */

static VClock coro_vc[RACE_CORO_MAX];
static Shadow shadows[RACE_SHADOW];
static SyncSlot syncs[RACE_SYNC];
static volatile int64_t race_lock;

extern void jogrt_panic(const void *msg, int64_t len);
extern int64_t jogrt_coro_id(void);

static void lock(void) {
    while (__atomic_exchange_n(&race_lock, (int64_t)1, __ATOMIC_ACQUIRE)) {
        /* spin */
    }
}

static void unlock(void) {
    __atomic_store_n(&race_lock, (int64_t)0, __ATOMIC_RELEASE);
}

static void vc_clear(VClock *v) {
    int i;
    for (i = 0; i < RACE_CORO_MAX; i++) v->c[i] = 0;
}

static void vc_copy(VClock *dst, const VClock *src) {
    int i;
    for (i = 0; i < RACE_CORO_MAX; i++) dst->c[i] = src->c[i];
}

static void vc_join(VClock *dst, const VClock *src) {
    int i;
    for (i = 0; i < RACE_CORO_MAX; i++) {
        if (src->c[i] > dst->c[i]) dst->c[i] = src->c[i];
    }
}

/* prev HB curr when every component of prev is <= curr's. */
static int vc_hb(const VClock *prev, const VClock *curr) {
    int i;
    for (i = 0; i < RACE_CORO_MAX; i++) {
        if (prev->c[i] > curr->c[i]) return 0;
    }
    return 1;
}

static void panic_msg(const char *msg) {
    uint64_t n = 0;
    while (msg[n]) n++;
    jogrt_panic(msg, (int64_t)n);
}

static void report_race(void) {
    static const char msg[] = "DATA RACE: unsynchronized shared access\n";
    race_hit = 1;
    if (race_expect) return;
    jogrt_panic(msg, (int64_t)(sizeof msg - 1));
}

static int32_t actor_cid(void) {
    int64_t cid;
    if (race_override_cid >= 0) return race_override_cid;
    cid = jogrt_coro_id();
    if (cid < 0) cid = 0;
    if (cid >= RACE_CORO_MAX) panic_msg("race: coro id out of range\n");
    return (int32_t)cid;
}

static uint32_t hash_ptr(uintptr_t p) {
    /* splitmix64-ish */
    uint64_t x = (uint64_t)p;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return (uint32_t)x;
}

static Shadow *shadow_get(uintptr_t key) {
    uint32_t h = hash_ptr(key);
    uint32_t i;
    for (i = 0; i < RACE_SHADOW; i++) {
        uint32_t idx = (h + i) % RACE_SHADOW;
        if (shadows[idx].addr == key) return &shadows[idx];
        if (shadows[idx].addr == 0) {
            shadows[idx].addr = key;
            shadows[idx].w_valid = 0;
            shadows[idx].r_valid = 0;
            return &shadows[idx];
        }
    }
    return 0; /* table full — drop tracking (documented leftover) */
}

static SyncSlot *sync_get(uintptr_t key, int create) {
    uint32_t h = hash_ptr(key);
    uint32_t i;
    for (i = 0; i < RACE_SYNC; i++) {
        uint32_t idx = (h + i) % RACE_SYNC;
        if (syncs[idx].key == key) return &syncs[idx];
        if (syncs[idx].key == 0) {
            if (!create) return 0;
            syncs[idx].key = key;
            vc_clear(&syncs[idx].vc);
            return &syncs[idx];
        }
    }
    return 0;
}

static void check_conflict(Shadow *sh, int is_write, int32_t cid, const VClock *mine) {
    if (sh->w_valid && sh->w_cid != cid && !vc_hb(&sh->w_vc, mine)) {
        report_race();
        return;
    }
    if (is_write && sh->r_valid && sh->r_cid != cid && !vc_hb(&sh->r_vc, mine)) {
        report_race();
    }
}

static void access_words(void *addr, uint64_t size, int is_write) {
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;
    int32_t cid;
    VClock *mine;

    if (!race_enabled || !addr || size == 0) return;

    cid = actor_cid();
    mine = &coro_vc[cid];

    start = (uintptr_t)addr & ~(uintptr_t)7;
    end = ((uintptr_t)addr + (uintptr_t)size + 7) & ~(uintptr_t)7;

    lock();
    /* Advance this coro’s clock so two actors never share an empty stamp. */
    mine->c[cid] += 1;
    for (p = start; p < end; p += 8) {
        Shadow *sh = shadow_get(p);
        if (!sh) continue;
        check_conflict(sh, is_write, cid, mine);
        if (is_write) {
            vc_copy(&sh->w_vc, mine);
            sh->w_cid = cid;
            sh->w_valid = 1;
            sh->r_valid = 0;
        } else {
            /* Keep one reader slot (first slice). Multiple readers without a
               prior write from another coro are fine if write HB holds. */
            if (!sh->r_valid || sh->r_cid == cid) {
                vc_copy(&sh->r_vc, mine);
                sh->r_cid = cid;
                sh->r_valid = 1;
            } else if (!vc_hb(&sh->r_vc, mine)) {
                /* Concurrent readers are OK; only refresh if we don't race. */
                vc_copy(&sh->r_vc, mine);
                sh->r_cid = cid;
            } else {
                vc_copy(&sh->r_vc, mine);
                sh->r_cid = cid;
            }
        }
    }
    unlock();
}

void jogrt_race_enable(void) {
    int i;
    lock();
    if (!race_enabled) {
        race_enabled = 1;
        for (i = 0; i < RACE_CORO_MAX; i++) vc_clear(&coro_vc[i]);
        for (i = 0; i < RACE_SHADOW; i++) {
            shadows[i].addr = 0;
            shadows[i].w_valid = 0;
            shadows[i].r_valid = 0;
        }
        for (i = 0; i < RACE_SYNC; i++) {
            syncs[i].key = 0;
            vc_clear(&syncs[i].vc);
        }
    }
    unlock();
}

void jogrt_race_read(void *addr, uint64_t size) {
    access_words(addr, size, 0);
}

void jogrt_race_write(void *addr, uint64_t size) {
    access_words(addr, size, 1);
}

void jogrt_race_release(void *sync) {
    int32_t cid;
    SyncSlot *slot;
    if (!race_enabled || !sync) return;
    cid = actor_cid();
    lock();
    coro_vc[cid].c[cid] += 1;
    slot = sync_get((uintptr_t)sync, 1);
    if (slot) vc_copy(&slot->vc, &coro_vc[cid]);
    unlock();
}

void jogrt_race_acquire(void *sync) {
    int32_t cid;
    SyncSlot *slot;
    if (!race_enabled || !sync) return;
    cid = actor_cid();
    lock();
    slot = sync_get((uintptr_t)sync, 0);
    if (slot) vc_join(&coro_vc[cid], &slot->vc);
    unlock();
}

void jogrt_race_set_context(int32_t cid) {
    race_override_cid = cid;
}

/* Deterministic selftest for zig build test / scripts. Returns 0 on pass. */
int64_t jogrt_race_selftest(void) {
    static uint64_t cell;
    int fails = 0;

    race_enabled = 0;
    race_expect = 1;
    race_hit = 0;
    race_override_cid = -1;
    jogrt_race_enable();

    /* 1) Same coro write+read: OK */
    race_hit = 0;
    jogrt_race_set_context(1);
    cell = 0;
    jogrt_race_write(&cell, 8);
    jogrt_race_read(&cell, 8);
    if (race_hit) fails++;

    /* 2) Unsynchronized write then other-coro write: RACE */
    race_enabled = 0;
    jogrt_race_enable();
    race_hit = 0;
    jogrt_race_set_context(1);
    jogrt_race_write(&cell, 8);
    jogrt_race_set_context(2);
    jogrt_race_write(&cell, 8);
    if (!race_hit) fails++;

    /* 3) Unsynchronized write then other-coro read: RACE */
    race_enabled = 0;
    jogrt_race_enable();
    race_hit = 0;
    jogrt_race_set_context(1);
    jogrt_race_write(&cell, 8);
    jogrt_race_set_context(2);
    jogrt_race_read(&cell, 8);
    if (!race_hit) fails++;

    /* 4) write + release / acquire + write: OK */
    race_enabled = 0;
    jogrt_race_enable();
    race_hit = 0;
    jogrt_race_set_context(1);
    jogrt_race_write(&cell, 8);
    jogrt_race_release(&cell); /* use cell addr as sync object */
    jogrt_race_set_context(2);
    jogrt_race_acquire(&cell);
    jogrt_race_write(&cell, 8);
    if (race_hit) fails++;

    race_expect = 0;
    race_override_cid = -1;
    race_enabled = 0;
    return fails == 0 ? 0 : 1;
}
