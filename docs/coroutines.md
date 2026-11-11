# Coroutines (v2 gate step 2)

Stackful coroutines with `rsp` swap. Gate step 1 proved single-thread yield; gate step 2 adds a multi-worker pool with work-steal and an ambient Dispatcher.

## Builtins

| Call | Runtime | Notes |
|---|---|---|
| `go(fn, arg)` | `jogrt_go` | Enqueue `fn(arg)` on a new 64 KiB stack (+ guard page) onto the ambient Dispatcher. Does **not** switch. |
| `yield()` | `jogrt_yield` | Park current to the home scheduler; home requeues (if still runnable) then runs other work. Safe across workers. |
| `coro_drain()` | `jogrt_coro_drain` | Run the pool until only main remains. |
| `coro_alive()` / `coro_id()` | counters | Live slot count; current slot id (main/home = 0). |
| `coro_park()` / `coro_unpark(i)` | stubs | Park current; clear parked and enqueue slot `i`. |
| `dispatcher_single()` | ambient | Single OS-thread Dispatcher (tests / deterministic demos). Call before any `go`. |
| `dispatcher_pool(n)` | ambient | Multi-worker pool (`n==0` → default 4, max 8). Default if unset. |
| `print_tid()` | `gettid` | Print `tid=… wid=… cid=…` (locked). Same `cid` + different `tid` ⇒ thread hop. |
| `worker_id()` / `gettid()` | i64 | Current worker index / Linux tid. |

`spawn(fn, arg)` remains **synchronous** (call on this thread).

## Runtime shape

- N OS workers via `clone` (`CLONE_THREAD|…`); worker 0 is main.
- Per-worker locked FIFO run queues; idle workers **steal** from others’ heads.
- TLS via `arch_prctl(ARCH_SET_FS)` (wid, current coro, home CCB, yield-requeue).
- Yield hops through the worker home so a coro is never runnable on two threads.
- Process exit uses `exit_group` so worker threads do not outlive main.

## Examples

```
zig build
./zig-out/bin/jog build examples/coro_yield.jog -o out/coro_yield && ./out/coro_yield
# a1 b1 a2 b2 a3 b3 done  (dispatcher_single)

./zig-out/bin/jog build examples/coro_pool.jog -o out/coro_pool && ./out/coro_pool
# multiple tid= lines; unique tids > 1; often same cid on different tids
```

## Limits (not in this step)

- No `go { }` blocks / captures, no `Scope`, no `Task` / join.
- No `with (Dispatcher)` cross-pool hop (gate step 3).
- Fixed max 32 coro slots, 8 workers; stacks via `mmap` (not charged to `alloc`).
- Queues are locked FIFOs (not lock-free Chase-Lev); fine for the gate.
- Freestanding linux x64 only.

## Gate step 3

Dispatcher override = park/resume across pools (Kotlin-like `with` API). See JogDocs `research/KotlinCoroutineThreads.md`.
