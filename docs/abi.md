# Jog v2 ABI (linux x64)

## Jog ABI v0

For scalars and pointers, Jog↔Jog calls use the **System V AMD64** convention:

- Arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Return: `rax`
- Callee-saved: `rbx`, `r12`–`r15`, `rbp`
- Stack 16-byte aligned before `call`

Aggregates / multi-value returns are not defined yet.

## C ABI / `extern`

`extern { fn name(...): T }` declares a **C SysV** symbol. The compiler emits `.extern name` and uses the same argument registers as Jog ABI v0 for scalar/pointer parameters. Linking pulls in libc when any extern is present (`zig cc` without `-nostdlib`).

## `export fn`

Marks a Jog function `.globl` so C can call it under SysV. `main` is always exported.

## Runtime (`runtime/linux_x64*.s`)

| Symbol | Role |
|---|---|
| `_start` | Freestanding entry → `main` → `exit` (omitted when linking libc) |
| `jogrt_print` | `write(1, ptr, len)` |
| `jogrt_panic` | `write(2, …)` + `exit(1)` |
| `jogrt_alloc` | bump allocator (Allocator stub) |
| `jogrt_dispatcher_stub` | No-op stub (pool is `jogrt_dispatcher_pool`) |
| `jogrt_dispatcher_single` / `jogrt_dispatcher_pool` | Primary Dispatcher handle (1 thread vs N-worker pool) |
| `jogrt_dispatcher_new` / `jogrt_dispatcher_get` | Extra pools / handle lookup |
| `jogrt_dispatcher_enter` / `jogrt_dispatcher_leave` | `with (Dispatcher)` cross-pool park/resume |
| `jogrt_spawn` | Sync call `fn(arg)` on this thread |
| `jogrt_go` / `jogrt_yield` / `jogrt_coro_*` | Stackful M:N scheduler + steal (`linux_x64_coro.s`) |
| `jogrt_print_tid` / `jogrt_gettid` / `jogrt_worker_id` | Multi-thread proof helpers |

Coroutines: see `docs/coroutines.md`. These are the linux **actuals** for process entry / syscalls; other OS runtimes will live beside them.
