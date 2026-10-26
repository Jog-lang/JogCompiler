---
status: draft
tags:
  - plan
  - compiler
---

# V1 compiler

The V1 compiler is a C17 program called `jog`. It reads a Jog module, checks it, lowers it to one intermediate representation (IR), and generates C from that IR. Then it calls clang, or gcc as a fallback, to produce a native executable. The first goal is a small language prototype that works: functions, structs, packages, and direct calls into libc, with Jog-typed bindings and some convenience on top of the C functions.

WebAssembly is step 2. The same generated C goes through `clang --target=wasm32`, and `wasm-ld` links it.

## Why generate C first

- **A working prototype quickly.** A C compiler already handles register allocation, instruction selection, the calling conventions, debug info, and optimization.
- **C interop for free.** An `extern` function becomes a C prototype, and Jog structs already use C layout. libc, SDL3, and the System V and Win64 ABIs need no code in the compiler. See JogDocs: design/language/CInterop.md.
- **Wasm almost for free.** clang compiles the same C to `wasm32`.
- **The IR is not wasted.** The frontend lowers to the IR, not straight to C. The compile-time interpreter runs that IR. A native x64 backend can later replace the C backend on the same IR without touching the frontend. See JogDocs: design/Compiler.md.

The cost is that a C compiler must be installed. See JogDocs: plans/Scaffold.md.

## Pipeline

```
jog.mod + folders
   │  loader      find the module root, map import paths to folders, list *.jog files
   ▼
source files
   │  lexer       tokens, with newline-to-SEP insertion and string interpolation parts
   ▼
tokens
   │  parser      recursive descent for declarations and statements, precedence climbing for expressions
   ▼
AST, one per file
   │  resolver    package scopes, imports, declarations, then names in bodies
   │  checker     types, generic instantiation, contexts, definite assignment, exhaustiveness, constant folding
   ▼
typed AST
   │  lowering    defer/errdefer, when and patterns, Option, error returns, contexts as hidden arguments
   ▼
IR
   │  interp      compile-time execution (top-level val, @comptime)
   │  emit_c      one C17 file for the whole program
   ▼
out/<name>.c + runtime
   │  cc          clang (or gcc) -std=gnu17, linked with the runtime
   ▼
executable
```

V1 compiles the whole program from source in one run: the root package, every imported package, and std. Separate compilation and `.jogz` archives come later. See JogDocs: design/Modules.md.

## Compiler internals

| Concern | Choice |
|---|---|
| Memory | One `CtArena` for the whole run. Nothing is freed until the process exits. Growable lists use `CtDa` while being built. |
| Names | Every identifier is interned with `CtIntern`, so names compare by pointer. |
| Symbol tables | `CtMap`, keyed by interned name: one per package scope, plus a chain of block scopes. |
| Positions | Tokens and nodes keep a file, line, and column. Diagnostics go through `CtDiag`. |
| Errors | Each phase runs to the end of its input and collects errors. The driver stops before the next phase if there were any, and prints at most 50. |
| Types | Interned in a type table: one `Type*` for each distinct type, so type equality is pointer equality. Structural types (`*T`, `[]T`, `[N]T`, tuples, function types, `T?`) are keyed by a canonical string in a `CtMap`. |
| Generics | Monomorphized. Each instantiation is checked and lowered once and cached by its type arguments. See JogDocs: design/language/Generics.md. |
| Declaration order | Top-level declarations can refer to each other in any order. The checker resolves them on demand: each top-level declaration is checked in its own coroutine (`tools/coro.h`). When it needs the type of a declaration that is not checked yet, it parks until that one is done. If every remaining coroutine is parked, that is a cycle, and the wait chain is the error message. The same mechanism orders global initializers and, later, annotation rounds. |
| Output | `CtSb` string builders, flushed to `out/<name>.c`. |

The host coroutine tool has 64 KiB stacks. The checker recurses over expressions inside one declaration, so the stack size will become a parameter of `coroutine_go` if 64 KiB turns out to be too small.

### IR

The IR is typed, three-address code over basic blocks. It is not in SSA form.

- A function has typed **locals**, including temporaries, and a list of **blocks**. Each block ends in exactly one terminator: `br`, `cond_br`, `switch`, `ret`, or `unreachable`.
- Instructions read and write locals and constants. `dst = add a, b`, `dst = load p`, `store p, v`, `dst = addr_of x`, `dst = field_addr p, k`, `dst = index_addr p, i`, `call`, `call_indirect`.
- Checks are explicit instructions, so every backend implements them the same way: overflow-checked arithmetic, `bounds_check`, `null_check` for `!!`, and `panic`.
- `defer`, `errdefer`, and `when` patterns are fully lowered to blocks before the IR. Backends never see them.
- Contexts are ordinary trailing parameters by the time code reaches the IR. See JogDocs: design/language/Contexts.md.

This shape maps directly to C: locals become C locals, blocks become labels, and terminators become `goto`, `if`, `switch`, or `return`. It is easy to interpret for compile-time execution. A native backend can allocate registers over the locals later. A direct Wasm backend would need a stackifier pass to recover structured control flow, but step 2 does not need one, because it goes through C.

## The C backend

- **One file.** The whole program goes into one C file: type definitions first, then prototypes, globals, and functions. The C compiler can inline across packages, and there are no header-ordering problems.
- **`#line` directives** map every statement back to its `.jog` file and line. C compiler errors and debugger steps then show Jog source. gdb and lldb work without a custom debug-info writer.
- **Names:** `jog_<package path>__<name>`. Methods are `jog_<pkg>__<Type>__<method>`. Generic instances get a suffix from their type arguments. `extern` functions keep their exact C name, and `@export` functions use the name they give.
- **Types:**
  - std scalars map to `<stdint.h>` types, and `bool` to `_Bool`.
  - `bytes[N]` is `struct { uint8_t b[N]; }`.
  - Slices, `bytes`, and `str` are `{ T *ptr; size_t len; }` structs.
  - Jog structs are C structs in the same field order. Enums are a tag plus a union.
  - `*T?` is a nullable pointer. Other `T?` are `{ tag; T value; }`.
  - Tuples are generated structs.
- **Checks:** overflow uses `__builtin_add_overflow` and its siblings, which clang and gcc both provide. Bounds checks are an inline compare that calls `jogrt_panic_bounds`.
- **No C headers for user code.** The generated file includes only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, and `jogrt.h`. C functions are declared from their Jog `extern` declarations, so a binding file is the only description of a C API. The compiler passes `-Wno-builtin-declaration-mismatch` (gcc) or `-Wno-incompatible-library-redeclaration` (clang) for the libc names the C compiler knows as builtins.
- **Compiler flags:** debug builds use `-O0 -g`. `release` uses `-O2`, and `release-fast` uses `-O2` without bounds checks. The generated C is kept with `--keep-c`.

## Runtime and std for V1

The runtime is C, in `runtime/`. It is compiled with every program:

- **Startup:** `main(argc, argv)` saves the arguments, sets up the default contexts, runs the global initializers in order, and calls the Jog `main`. See JogDocs: design/Runtime.md.
- **Panics:** `jogrt_panic(msg, file, line)` and the specialized check failures (bounds, overflow, null) print and then `abort()`.
- **Default contexts:** an `Allocator` backed by `malloc`/`free`, a root `ErrorAllocator` arena, and a stderr `Logger`.
- **Shims:** small C functions for libc features that are macros rather than functions, such as `stdout`, `stderr`, and `errno`. The Microsoft UCRT defines them as function calls behind macros.
- **Coroutines (milestone 6):** the single-threaded scheduler, using the same switch technique as `tools/coro_switch.c`. Moving it into `runtime/asm` as real assembly comes with the native backend.

std starts small, written in Jog:

- `std.c`: `extern` bindings to the portable part of libc: `malloc`, `free`, `realloc`, `memcpy`, `memset`, `strlen`, `exit`, `fopen`, `fread`, `fwrite`, `fclose`, `getenv`, plus the runtime's shims and the C type aliases (`c.int`, `c.char`, ...). See JogDocs: design/language/CInterop.md.
- `std.io`: `print`, `println`, `eprint`, and file reading and writing, as Jog functions over `std.c`, taking `str` instead of C strings.
- `std.mem`: the `Allocator` interface and the C allocator.

The `jog` executable finds std and the runtime in `<jog.exe>/../std` and `<jog.exe>/../runtime`, or under `JOG_ROOT` when that is set.

## Driver

```
jog build [package] [-o file] [--release | --release-fast] [--keep-c] [--target wasm32]
jog run [package] [--target wasm32] [-- args...]
jog test [package | --all]
jog lex <file>          print tokens (debugging)
jog parse <file>        print the AST (debugging)
jog check [package]     run the frontend only
jog emit-c [package]    print the generated C
```

## Source layout

```
compiler/src/
  main.c          driver and argument parsing
  token.h         token kinds
  lexer.c/.h      tokens, newline rule, interpolation          milestone 0
  ast.h parser.c  AST and parser                               milestone 1
  loader.c        jog.mod, packages, files                     milestone 2
  types.c/.h      type table                                   milestone 2
  resolve.c       scopes and names                             milestone 2
  check.c         type checker                                 milestone 2
  ir.h lower.c    IR and lowering                              milestone 2
  emit_c.c        C backend                                    milestone 2
  cc.c            running clang or gcc                         milestone 2
  interp.c        compile-time interpreter                     milestone 7
runtime/
  jogrt.h jogrt.c startup, panics, checks, allocator, shims
std/
  c/  io/  mem/   Jog sources
tools/            shared C utilities (arena, da, map, diag, file, coro, str, intern, fs)
tests/            C unit tests, and later tests/jog/ end-to-end programs
```

## Milestones

| # | Goal | Done when |
|---|---|---|
| 0 | Skeleton: driver, lexer, `jog lex`, unit tests | `jog lex` prints the tokens of any example from the language notes, and the lexer tests pass |
| 1 | Parser and AST for the whole grammar, `jog parse` | every code block in the language notes parses |
| 2 | **Hello world through C**: functions, `val`/`var`, integers, `bool`, `str`, `if`, `for`, calls, `extern`, `c"..."`, runtime startup | `jog run` prints "hello" through `std.c` bindings |
| 3 | Structs, methods, pointers, arrays, slices, `bytes`, tuples and multiple returns, `error?`, `defer`/`errdefer`, enums, `when`, `T?`, overflow and bounds checks | a small file-processing program works |
| 4 | Packages and imports, global initialization order, `std.io` and `std.mem`, `jog test` with `*.test.jog` | std builds as packages, and tests run |
| 5 | Contexts, `Allocator`, string interpolation, generics, interfaces | the examples in JogDocs: design/language/Contexts.md compile |
| 6 | Coroutines: `go`, `scope`, `Task`, `Chan`, `select` on the single-threaded runtime | a channel pipeline runs |
| 7 | Compile-time interpreter: top-level `val` beyond constant folding, `@comptime`, `@embed` | a CRC table is computed at compile time |
| Step 2 | Wasm: generated C through `clang --target=wasm32`, a Wasm runtime, `wasm-ld` | hello world runs in a Wasm host |

Until milestone 7, a top-level `val` must be a constant expression that the checker can fold.

### Status

- **Milestone 0 is done.** `jog lex` and `lexer_test` exist, and the new tools (`str`, `intern`, `fs`) are covered by `util_test`. Every Jog code block in the language notes lexes cleanly, with one exception: the `${...}` splices inside `quote { }` in JogDocs: design/language/Comptime.md. Quoted code comes after V1, and the lexer will learn it then.
- **Contracts are in place.** The AST, types, IR, program, phase, command, and runtime ABI headers are written. Every phase links as a stub, and `scripts/check.ps1` (Windows) or `scripts/check.sh` (Linux) is the merge gate.
- **Next:** milestones 1 and 2 in parallel streams, as laid out in [[plans/V1Parallel]].
- **Self-hosting** is planned in JogDocs: bootstrap/Roadmap.md. This compiler becomes stage 0 and the reference that each stage of the Jog-written compiler is checked against.

Step 2 uses a freestanding wasm32 runtime, not WASI. The native scheduler's stacks are OS calls Wasm cannot link. `extern` functions stay C prototypes; wasm-ld imports each undefined symbol from `env`. `runtime/wasm/jogrt_wasm.c` talks to the host through `jogrt_host_write`, `jogrt_host_abort`, and `jogrt_host_exit`. `jog run --target wasm32` executes the module with Node. Hello world's `puts` is the `env.puts` import. `go` runs its body to completion; any other scheduler call aborts. Program arguments and `@link` import-module names are not wired yet.

## Testing

- **C unit tests** in `tests/`, using the same `CHECK` macro style as `util_test.c`: `util_test` for `tools/`, and `lexer_test` and later `parser_test` for the compiler.
- **End-to-end tests** from milestone 2: `tests/jog/*.jog` programs with `// expect: ...` comments, which a ctest runner builds, runs, and compares against their output.

## See also

- JogDocs: design/Compiler.md
- JogDocs: design/Runtime.md
- JogDocs: plans/Bootstrap.md
- JogDocs: design/language/Grammar.md
- JogDocs: design/language/CInterop.md
