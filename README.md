# JogCompiler (v2)

Zig-hosted Jog compiler that emits **native linux x64 ELF** with no C intermediate.

v1 lives on branches `develop` (jogc) and `seed` (C `jog`). This branch (`v2`) is a greenfield rewrite. Design: JogDocs `design/v2/`.

## Build

```
zig build
./zig-out/bin/jog version
```

## Commands

```
jog version
jog parse <path>
jog check <path>
jog build <path> -o <out>
jog deps [path]
```

`path` may be a `.jog` file, a directory with `jog.mod`, or a `jog.mod` file. Module sources listed in `jog.mod` (`root` + optional `source` lines) are parsed and typechecked together.

## Pipeline

```
.jog → lex → parse → check → IR → GAS asm → zig cc + runtime/*.s → ELF
```

No C source is generated from Jog. Freestanding programs use `runtime/linux_x64_start.s`; `extern` pulls in libc. ABI notes: `docs/abi.md`.

## Examples

```
zig build
./zig-out/bin/jog build examples/hello.jog -o out/hello
./out/hello
./zig-out/bin/jog build examples/arith.jog -o out/arith && ./out/arith
./zig-out/bin/jog build examples/extern_puts.jog -o out/puts && ./out/puts
./zig-out/bin/jog build examples/structs.jog -o out/structs && ./out/structs
./zig-out/bin/jog build examples/null_flow.jog -o out/null_flow && ./out/null_flow
./zig-out/bin/jog build examples/cast_narrow.jog -o out/cast && ./out/cast
./zig-out/bin/jog build examples/context_iface.jog -o out/ctx && ./out/ctx
./zig-out/bin/jog build examples/subset.jog -o out/subset && ./out/subset
./zig-out/bin/jog build examples/mod_demo -o /tmp/mod_demo && /tmp/mod_demo
./zig-out/bin/jog deps examples/mod_demo
./zig-out/bin/jog build examples/coro_yield.jog -o out/coro_yield && ./out/coro_yield
./zig-out/bin/jog build examples/spawn.jog -o out/spawn && ./out/spawn
```

Language subset (milestone): structs/fields, pointers, `T?` + `?.`, widen/`as` narrow, `with` contexts, `interface struct` fat ptrs. Minimal `jog.mod` module graph (`root` + `source`). Stackful `go`/`yield` on one OS thread: `docs/coroutines.md`.
