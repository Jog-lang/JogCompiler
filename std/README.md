The standard library is written in Jog. Each directory is one package. The compiler loads it from the `std` directory next to the directory that contains the `jog` executable, or from `$JOG_ROOT/std` when `JOG_ROOT` is set.

- `arena` — bump allocator that implements `mem.Allocator`
- `c` — libc bindings and C type aliases (`c.windows.jog`, `c.posix.jog`)
- `cmp` — `Ordering`, comparison interfaces, and numeric type sets
- `conv` — parse/format integers and floats; hex encode/decode
- `error` — sentinel errors `ErrNotFound`, `ErrIO`, and `ErrOutOfMemory`
- `fs` — paths, directory listing, and file read and write
- `io` — `print`, `println`, `eprint`, and file read and write
- `json` — RFC 8259-ish `Value` tree (parse / stringify / release)
- `list` — growable `List<T>`
- `map` — open-addressed `Map<K, V>`
- `math` — libm plus `clamp` / `sign` helpers
- `mem` — `Allocator`, the malloc heap, and the error arena
- `os` — `argc` / `argv`, env, pid, hostname, paths
- `proc` — start a process and collect its output
- `strbuilder` — growable byte buffer
- `testing` — `expect`, `expectEq`, `fail`, `fatal`, and `expectPanic`
- `text` — string views, ASCII helpers, pad/join/repeat

`*.test.jog` files are the package tests. On the Zig v2 driver, prefer `jog check` plus smoke builds until `jog test` is restored (see JogDocs `bootstrap/Friction.md`).
