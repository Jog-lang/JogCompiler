# Coroutines (v2 gate step 1)

Single-OS-thread stackful coroutines with `rsp` swap. Proves cooperative switch before multi-worker steal (gate step 2).

## Builtins

| Call | Runtime | Notes |
|---|---|---|
| `go(fn, arg)` | `jogrt_go` | Enqueue `fn(arg)` on a new 64 KiB stack (+ guard page). Does **not** switch. |
| `yield()` | `jogrt_yield` | Round-robin to the next alive, non-parked coro. No-op if alone. |
| `coro_drain()` | `jogrt_coro_drain` | Yield until only the caller is alive (main helper; no `while` yet). |
| `coro_alive()` / `coro_id()` | counters | Live slot count; current slot id (main = 0). |
| `coro_park()` / `coro_unpark(i)` | stubs | Park current + yield; clear parked on slot `i`. Enough for yield; not channels. |

`spawn(fn, arg)` remains **synchronous** (call on this thread) for the Dispatcher stub.

## Limits (not in this step)

- No `go { }` blocks / captures, no `Scope`, no `Task` / join.
- No multi-worker pool, work-steal, or `with (Dispatcher)` hop.
- Fixed max 16 slots; stacks via `mmap` + `mprotect` guard; not charged to `alloc`.
- Freestanding linux x64 only (same as the rest of v2 runtime).

## Example

```
zig build
./zig-out/bin/jog build examples/coro_yield.jog -o out/coro_yield
./out/coro_yield
```

Expect interleaved `a1`/`b1`/… then `done`.

## Gate step 2

Multi-worker pool + steal; schedule `go` onto ambient `Dispatcher`. See JogDocs `research/KotlinCoroutineThreads.md` and `design/v2/Concurrency.md`.
