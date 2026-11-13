# Jog v2 ABI (linux x64 + wasm32-emscripten)

## wasm32-emscripten (JOG-90)

`--target wasm32-emscripten` lowers IR → C → **emcc** (not freestanding wasm32).
Slots stay `int64_t`; extern calls cast through wasm32-friendly `intptr_t` /
`int64_t` / `double` / `void` based on the Jog `extern` signature (`wide_args`,
`wide_ret`, `void_ret`, `float_*`). Runtime: `runtime/emscripten/jogrt.c` +
`jogrt_mainloop.c` (`jog_host_run_loop` → `emscripten_set_main_loop`).
Output: `.html` / `.js` / `.wasm`. Coroutines/`go` abort (no stack switch).

## Jog ABI v0

For scalars and pointers, Jog↔Jog calls use the **System V AMD64** convention:

- Arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Return: `rax`
- Callee-saved: `rbx`, `r12`–`r15`, `rbp`
- Stack 16-byte aligned before `call`

Aggregates / multi-value returns are not defined yet.

## C ABI / `extern`

`extern { fn name(...): T }` declares a **C SysV** symbol. The compiler emits `.extern name`. Integer/pointer args use `rdi`…`r9`; **`f32`/`f64` args and returns use `xmm0`…** (System V AMD64). Linking pulls in libc when any extern is present (`zig cc` without `-nostdlib`).

Jog↔Jog float calls still pass bit patterns in GPRs (internal convention). Only `extern` uses the xmm SysV path.

## `export fn` / `@export`

Marks a Jog function `.globl` so C can call it under SysV. `main` is always exported. `@export` is accepted (keyword `export` after `@`).

## Package `val` / `var`

Top-level `val` with a literal init is a compile-time constant (folded). Top-level `var` is a `.data` global (`jog_global_N`).

## Runtime (`runtime/linux_x64*.s`)

| Symbol | Role |
|---|---|
| `_start` | Freestanding entry → `main` → `exit` (omitted when linking libc) |
| `jogrt_print` | `write(1, ptr, len)` |
| `jogrt_panic` | `write(2, …)` + `exit(1)` |
| `jogrt_alloc` | bump allocator (4 MiB heap) |
| `jog_host_run_loop` / `jogrt_host_run_loop` | desktop returning mainloop (`while (tick())`) |
| `jogrt_dispatcher_stub` | No-op stub (pool is `jogrt_dispatcher_pool`) |
| `jogrt_dispatcher_single` / `jogrt_dispatcher_pool` | Primary Dispatcher handle (1 thread vs N-worker pool) |
| `jogrt_dispatcher_new` / `jogrt_dispatcher_get` | Extra fixed pools / handle lookup |
| `jogrt_dispatcher_io` | Singleton elastic blocking/IO Dispatcher |
| `jogrt_dispatcher_enter` / `jogrt_dispatcher_leave` | `with (Dispatcher)` cross-pool park/resume |
| `jogrt_sleep_ms` | Blocking `nanosleep` on the current OS worker |
| `jogrt_spawn` | Sync call `fn(arg)` on this thread |
| `jogrt_go` / `jogrt_yield` / `jogrt_coro_*` | Stackful M:N scheduler + steal (`linux_x64_coro.s`) |
| `jogrt_print_tid` / `jogrt_gettid` / `jogrt_worker_id` | Multi-thread proof helpers |

Coroutines: see `docs/coroutines.md`. These are the linux **actuals** for process entry / syscalls; other OS runtimes will live beside them.
