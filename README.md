# Jog

Jog is a low-level language. It works with functions, data whose layout is explicit, and pointers. Memory is managed by hand. On top of that it has coroutines and a few conveniences in the style of Go. Source files use `.jog`.

The specification is [JogDocs](https://github.com/Jog-lang/JogDocs). Start at its `AGENTS.md` and `guide/JogQuickRef.md`. The language overview is `design/Language.md`, and the language notes in `design/language/` are the spec. A local checkout is the sibling folder `../JogDocs`, or `../Jog/JogDocs` when the notes sit next to a `Jog` checkout.

This repository is the C seed compiler, `jog`. It is stage 0: the reference a later compiler written in Jog is checked against.

## Status

The seed reads a Jog module, checks it, lowers it to one intermediate representation, generates C, and builds that C with gcc or clang. WebAssembly is a second target through `clang --target=wasm32` and `wasm-ld` (`jog run --target wasm32`). The architecture and milestones are in `docs/plans/V1Compiler.md`. How the work is split is in `docs/plans/V1Parallel.md`.

## Platforms

- Windows x86-64, with MinGW gcc or clang
- Linux x86-64, with gcc or clang
- aarch64 is not supported

You need CMake, Ninja, gcc, and clang.

## Build and check

On Windows:

```
cmake -S . -B build -G Ninja
cmake --build build
pwsh scripts/check.ps1
```

`scripts/check.ps1` configures, builds (a compiler warning fails the check), runs `ctest --test-dir build`, and syntax-checks the C sources with clang. It prints `check: OK` when that passes.

On Linux:

```
cmake -S . -B build-linux -G Ninja
cmake --build build-linux
scripts/check.sh
```

`scripts/check.sh` does the same steps into `build-linux/` and prints `check: OK`. `CC=gcc` uses `build-linux-gcc/`, `CC=clang` uses `build-linux-clang/`, and `scripts/check.sh --both` runs the check with both compilers.

## Example

```
jog run examples/hello
```

After a Windows build that binary is `build/jog.exe`. After the Linux build above it is `build-linux/jog`. The program prints `Hello from Jog!`.

## Layout

- `compiler/` — the V1 compiler
- `tools/` — shared C utilities
- `runtime/` — the runtime linked into generated programs
- `std/` — the standard library, written in Jog
- `tests/` — C tests and end-to-end Jog programs
- `examples/` — small programs; `examples/hello` is hello world
- `docs/plans/` — the V1 compiler plans for this repository

## Self-hosting

A compiler written in Jog will be checked against this one. This tree stays stage 0. The plan is JogDocs `bootstrap/Roadmap.md`.

## License

[JogCompiler](https://github.com/Jog-lang/JogCompiler) carries the LICENSE. This working tree does not include a license file yet.
