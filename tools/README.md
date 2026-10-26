`ct_util` is a static library of small C helpers used by the compiler:

- `arena.h` — bump allocator
- `diag.h` — diagnostics
- `file.h` — whole-file reader
- `map.h` — string-keyed hashmap
- `da.h` — realloc-backed dynamic array
- `coro.h` — single-threaded GCC/Clang x86_64 coroutine scheduler for the host compiler (`coroutine_init`, `coroutine_go`, `coroutine_yield`, `coroutine_id`, `coroutine_alive`, `coroutine_shutdown`). The switch itself is `coro_switch.c`
- `str.h` — string views and the `CtSb` string builder
- `intern.h` — string interning
- `fs.h` — directory listing and UTF-8 path helpers
- `proc.h` — start a program and wait for its exit code

The CTest target `util_test` covers them.
