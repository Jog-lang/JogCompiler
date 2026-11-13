# Coroutines (v2 / JOG-78)

Stackful coroutines with `rsp` swap. Gate 1: single-thread yield. Gate 2: multi-worker pool + steal. Gate 3: `with (Dispatcher)` cross-pool migration. Gate 4: elastic blocking/IO Dispatcher pool. JOG-78: Chase-Lev lock-free work-stealing deques.

## Builtins

| Call | Runtime | Notes |
|---|---|---|
| `go(fn, arg)` | `jogrt_go` | Enqueue `fn(arg)` on a new 64 KiB stack (+ guard page) onto the **current** Dispatcher. Does **not** switch. |
| `yield()` | `jogrt_yield` | Park current to the home scheduler; home requeues onto the coro’s Dispatcher pool. Safe across workers. |
| `coro_drain()` | `jogrt_coro_drain` | Run the pool until only main remains. |
| `coro_alive()` / `coro_id()` | counters | Live slot count; current slot id (main/home = 0). |
| `coro_park()` / `coro_unpark(i)` | stubs | Park current; clear parked and enqueue slot `i` on its Dispatcher. |
| `dispatcher_single()` | `Dispatcher` | Single OS-thread primary Dispatcher (tests). Call before any `go`. Returns handle `0`. |
| `dispatcher_pool(n)` | `Dispatcher` | Primary multi-worker pool (`n==0` → default 4, max 16). Returns handle `0`. Default if unset. |
| `dispatcher_new(n)` | `Dispatcher` | Create an additional fixed Dispatcher pool (workers from the remaining budget, max 16 total / 4 pools). |
| `dispatcher_io()` | `Dispatcher` | Singleton **elastic** blocking/IO pool (Kotlin `Dispatchers.IO` analogue). Starts at 1 worker; grows toward min(4, remaining budget) when queued work ≥ live workers. Create last so grow can extend the high end of the wid space. Shrink-beyond-min deferred. |
| `dispatcher_get(id)` | `Dispatcher` | Validate and return handle `id` (for `with` without captures). |
| `with (disp) { ... }` | enter/leave | If `disp` is a `Dispatcher`, park and resume on that pool; leave hops back. Same Dispatcher ⇒ no hop. Nested. |
| `sleep_ms(n)` | `nanosleep` | Block the **current OS worker** for `n` milliseconds (no coro yield). Use on the IO pool so default workers stay free. |
| `print_tid()` | `gettid` | Print `tid=… wid=… cid=…` (locked). Same `cid` + different `tid`/`wid` ⇒ hop. |
| `worker_id()` / `gettid()` | i64 | Current worker index / Linux tid. |

`spawn(fn, arg)` remains **synchronous** (call on this thread).

Main’s OS stack (cid 0) is **not** migratable: `with` on main only updates ambient Dispatcher for subsequent `go`.

## Runtime shape

- N OS workers via `clone` (`CLONE_THREAD|…`); worker 0 is main. Cap 16 workers / 4 Dispatcher pools.
- Multiple **Dispatcher pools** partition workers; steal stays in-pool.
- Per-worker **Chase-Lev** deques (owner push/pop bottom; thieves CAS-steal top). In-pool `go`/yield push locally; cross-pool submits use a locked per-pool inbox. Idle workers take: local pop → inbox → steal.
- TLS via `arch_prctl(ARCH_SET_FS)` (wid, current coro, home CCB, yield-requeue).
- Yield / Dispatcher enter-leave hop through the worker home so a coro is never runnable on two threads.
- **IO elastic pool:** `dispatcher_io()` marks a pool elastic; enqueue may `clone` an extra worker when backlog ≥ live count (up to `pool_max`). Once grown, workers stay for the process (shrink deferred).
- Process exit uses `exit_group` so worker threads do not outlive main.

## Examples

```
zig build
./zig-out/bin/jog build examples/coro_yield.jog -o out/coro_yield && ./out/coro_yield
# a1 b1 a2 b2 a3 b3 done  (dispatcher_single)

./zig-out/bin/jog build examples/coro_pool.jog -o out/coro_pool && ./out/coro_pool
# multiple tid= lines; unique tids often > 1; often same cid on different tids

./zig-out/bin/jog build examples/coro_pool_stress.jog -o out/coro_pool_stress && ./out/coro_pool_stress
# N>>workers + yields; multi-tid expected under load (scripts/coro_stress.sh)

./zig-out/bin/jog build examples/coro_steal_stress.jog -o out/coro_steal_stress && ./out/coro_steal_stress
# 8 workers × many yields; exercises Chase-Lev steal under contention

./zig-out/bin/jog build examples/coro_with_dispatcher.jog -o out/coro_with && ./out/coro_with
# before wid∈{0,1}; inside wid∈{2,3}; after wid∈{0,1}; same cid

./zig-out/bin/jog build examples/coro_io_dispatcher.jog -o out/coro_io && ./out/coro_io
# cpu wid∈{0,1} interleaves with block-enter/leave; blocker wid≥2; ≥2 IO wids under load
```

Rare single-`tid` under light load on gate-2 pool demos is valid. Treat hangs / lost wakeups as failures.

## Limits (not in this step)

- No `go { }` blocks / captures, no `Scope`, no `Task` / join.
- Fixed max 32 coro slots, 16 workers, 4 Dispatcher pools; stacks via `mmap` (not charged to `alloc`).
- Chase-Lev arrays are fixed-capacity (no grow); full queue panics.
- IO pool shrink-beyond-min is deferred (grow only for now).
- Freestanding linux x64 only.
