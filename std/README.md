The standard library is written in Jog. Each directory is one package. The compiler loads it from the `std` directory next to the directory that contains the `jog` executable, or from `$JOG_ROOT/std` when `JOG_ROOT` is set.

- `arena` — bump allocator that implements `mem.Allocator`
- `c` — libc bindings and C type aliases (`c.windows.jog`, `c.posix.jog`)
- `cmp` — `Ordering`, comparison interfaces, and numeric type sets
- `error` — sentinel errors `ErrNotFound`, `ErrIO`, and `ErrOutOfMemory`
- `fs` — paths, directory listing, and file read and write
- `io` — `print`, `println`, `eprint`, and file read and write
- `list` — growable `List<T>`
- `map` — open-addressed `Map<K, V>`
- `math` — `floor`, `ceil`, `log2`, and `exp2` from libm
- `mem` — `Allocator`, the malloc heap, and the error arena
- `os` — `argc`, `argv`, and environment variables
- `proc` — start a process and collect its output
- `strbuilder` — growable byte buffer
- `testing` — `expect`, `expectEq`, `fail`, `fatal`, and `expectPanic`

`*.test.jog` files are the package tests. `jog test` in a package directory runs them.
