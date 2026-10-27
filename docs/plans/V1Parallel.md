---
status: active
tags:
  - plan
  - compiler
  - agents
---

# V1 compiler: parallel work plan

This splits the V1 compiler from [[plans/V1Compiler]] into **streams** that separate agents can work on at the same time. Each stream owns its files, builds against frozen **contracts**, and proves its work with its own tests before anything else exists. Read this note, [[plans/V1Compiler]], and the contract headers before starting.

## Before anyone starts

1. **One branch, or one worktree, per stream:** `v1/<stream>-<step>`, for example `v1/parser-1-decls`. Keep pull requests small, one step from a stream's list below.
2. **Merge gate:** `pwsh scripts/check.ps1` on Windows, or `scripts/check.sh` on Linux, must print `check: OK` before merging. It configures and builds and fails on any warning, runs every ctest, and syntax-checks every C file with clang using `-Werror`.

## Ground rules

- **Only edit files your stream owns.** The ownership table is below. A file nobody owns yet goes to the stream that creates it. If you need a change in another stream's file, ask for it in the hand-off notes (see below), or add a new file of your own.
- **Contracts are frozen.** The headers in the contract table may only change through a *contract change*: one small pull request that touches only the contract and whatever must change with it (stubs, `contracts_test.c`), with the reason added to the change log at the end of this note. Adding a field at the end of a struct, or a new function, is fine. Renaming or changing meaning is not, without that process.
- **CMake needs no edits.** Every `compiler/src/*.c` except `main.c` is globbed into `jogc`. Every `tests/<name>_test.c` becomes its own ctest; `runtime_*_test.c` link the runtime library, the others link `jogc`. `runtime/*.c` except `jogrt_main.c` becomes `jogrt`. Re-run cmake after adding files; `scripts/check.ps1` does that.
- **Each `jog` command is its own file:** `compiler/src/cmd_<name>.c`, declared in `commands.h`. `main.c` only dispatches.
- **Stubs:** every phase entry point in `phases.h` already has a stub that reports "not implemented" and fails. The stream that owns it replaces the whole file. Everything links from day one.
- **Code style:** follow `tools/` and `lexer.c`.
  - C17, no extensions. `coro_switch.c` is the only exception.
  - Prefixes: `ct_`/`Ct` for tools, `jog_`/`Jog` for the compiler, `jir_`/`Jir` for the IR, `jogrt_` for the runtime.
  - Memory comes from the program arena. `CtDa` is for lists while they are being built.
  - Names are interned. Errors go through `CtDiag`, with a position whenever there is one.
  - Comments state contracts and constraints, not narration.
- **Tests:** use the `CHECK` macro style from `tests/util_test.c`. Every stream proves its work in isolation. Nobody waits for the full pipeline to test.
- **Docs:** the language notes are the specification. When an implementation has to make a decision the notes do not cover, write the decision into the relevant note in the same pull request, and add it to your stream's notes at the end of this file.

## Contracts

| File | What it fixes | Owner of changes |
|---|---|---|
| `compiler/src/token.h` | token kinds and `JogToken` | P (lexer is done) |
| `compiler/src/ast.h` | every AST node kind and its fields | P, with T's agreement |
| `compiler/src/types.h` | `JogType`, interning, C layout | T |
| `compiler/src/ir.h` | JIR: operands, instructions, terminators, functions, globals, builder | L, with C's and I's agreement |
| `compiler/src/program.h` | `JogProgram`, packages, source files, build options | D |
| `compiler/src/phases.h` | the entry point of every phase | the coordinator |
| `compiler/src/commands.h` | one function per `jog` subcommand | D |
| `runtime/jogrt.h` | the ABI between generated C and the runtime | R, with C's agreement |

`ast.c`, `types.c`, and `ir.c` (the builder and `jir_verify`) are already implemented and covered by `tests/contracts_test.c`.

### Conventions shared across streams

- **Symbol names in generated C.** Lowering sets every `JirFunc.name` and `JirGlobal.name`. The C backend emits them verbatim.
  - `jog_<pkg>__<name>`: `<pkg>` is the package path with `.` replaced by `_`, or the module name for the root package.
  - Methods: `jog_<pkg>__<Type>__<method>`.
  - Operators: the method name spelled out, as in `op_add`, `op_cmp`, `op_index`, or `op_index_set`.
  - Generic instances: the name followed by `__` and the mangled type arguments.
  - `extern` functions and `@export` functions use their C name unchanged.
- **Names the C backend makes up.** Locals are `l<index>`, labels `b<index>`, structural types `jt<id>`, and nominal types `jog_<pkg>__<Name>`. Struct fields are `f_<name>`, which never collides with a C keyword.
- **Entry points.** C emits `jog_entry_init` and `jog_entry_main` from `JirModule.init_func` and `main_func`. The runtime's `jogrt_main.c` calls them. See `runtime/jogrt.h`.
- **Positions.** `JirInst.pos` and `JirTerm.pos` come from the AST node they were lowered from. C turns them into `#line` directives, and passes the file and line to `jogrt_panic_*`.
- **The `std` layout.** `std/c/c.jog` is `import std.c`, `std/io/io.jog` is `import std.io`, and `std/mem/mem.jog` is `import std.mem`. The loader finds std at `<jog.exe>/../std` or `$JOG_ROOT/std`.
- **`jog.mod`.** V1 uses the line-based format in JogDocs: design/Modules.md, that is `module <name>`, then optional `link <lib>` lines.

## File ownership

| Stream | Owns |
|---|---|
| **P** Parser | `parser.c`, `parser_*.c`, `ast_print.c`, `cmd_parse.c`, `tests/parser_test.c`, `tests/parse/` |
| **R** Runtime and std | `runtime/*`, `std/*`, `tests/runtime_*_test.c` |
| **C** C backend | `emit_c.c`, `emit_c_*.c`, `ir_dump.c`, `tests/emit_c_test.c`, `tests/ir/` |
| **D** Driver | `loader.c`, `cc.c`, `cmd_build.c`, `cmd_test.c`, `commands.h`, `main.c`, `tools/proc.*`, `tests/loader_test.c`, `tests/e2e_test.c`, `tests/jog/`, `scripts/` |
| **T** Checker | `symbols.h`, `resolve*.c`, `check*.c`, `const_eval.c`, `tests/check_test.c`, `tests/check/` |
| **L** Lowering | `lower*.c`, `tests/lower_test.c` |
| **I** Interpreter | `interp*.c`, `tests/interp_test.c` |

## Schedule

```
wave 1 (start now, fully parallel)   P  R  C  D  T(foundation)  [I optional]
wave 2 (after P step 2 merges)       T(full M2 subset)  L  ; P, C, D continue
milestone 2 gate                     D wires the pipeline: hello world runs
wave 3 (milestone 3 onward)          feature slices across T, L, C   (see below)
```

## Wave 1 streams

### P — Parser (milestone 1)

**Goal:** `jog_parse` builds the AST in `ast.h` for the whole grammar in JogDocs: design/language/Grammar.md, and `jog parse <file>` prints it.

**Steps:**
1. **Declarations and types.** Imports, `fn` signatures with receivers, type params, params, and contexts. `var`/`val`/`context`, `type`, `struct`, `interface`, `enum`, `extern { }`. Attributes and visibility. Every type form. `jog_ast_print` and `jog parse`. Error recovery: on a syntax error, report it and skip to the next `SEP` or `}` at the same depth.
2. **Statements and expressions.** Blocks, assignments and destructuring, `defer`/`errdefer`, `return`, `for` in all three forms, `with`. Precedence climbing that follows the precedence table exactly. Calls with named arguments, trailing lambdas, and `with (...)`. Index, slice, member, `?.`, `!!`, `as`/`as?`, `is`, `if` as an expression, interpolation (`STR_HEAD` ... `STR_TAIL` become `INTERP`), array literals.
3. **The rest.** `when` with nested patterns, lambdas (rule 3: scan to the matching `)` and check for `:` or `->`), `go`, builtins such as `@comptime(...)`, and generic arguments versus `<` (rule 2, with `>>` split).

**Rules to apply exactly:** disambiguation rules 1–9 in JogDocs: design/language/Grammar.md, plus two `SEP` rules: one between attributes and their declaration, and one before `else`, both of which the parser ignores. Struct literals are calls; do not try to tell them apart in the parser.

**Tests:** `tests/parser_test.c` checks the shape of each construct. `tests/parse/*.jog` with `*.ast` golden files covers whole programs.

**Done when** every Jog code block in the language notes parses with no errors, except post-V1 `quote` blocks, and each disambiguation rule has a test. Write a small script in `tests/parse/` that extracts the blocks. Fences tagged `ebnf` are not Jog.

### R — Runtime and std (milestone 2 runtime)

**Goal:** implement `runtime/jogrt.h` exactly, and write the first std packages in Jog.

**Steps:**
1. **`jogrt.c` and `jogrt_main.c`.** Startup and finish, the panics, arguments, the `malloc`-backed allocator, the root error arena with mark and release, the shims, and `jogrt_write`. A panic prints `panic: <what>` and then `  at <file>:<line>` to stderr, then calls `abort()`.
2. **`tests/runtime_*_test.c`.** Test every function. For panics, run the test binary again as a child process with an argument that triggers one panic, and check its stderr and exit status.
3. **std in Jog.** `std/c/c.jog` holds `extern` bindings for the libc set in [[plans/V1Compiler]] plus the runtime shims, and the C type aliases (`type int = i32` and so on, chosen per target). Also write `std/io/io.jog` (`print`, `println`, `eprint`, `eprintln` over `jogrt_write`) and `std/mem/mem.jog`, which holds the `Allocator` interface sketch, used from milestone 5. The files must pass `jog lex` now, and `jog parse` once P step 2 lands.

**Done when** the runtime tests pass under gcc and clang, and a hand-written C file that defines `jog_entry_init` and `jog_entry_main` links with the runtime and prints through `jogrt_write`.

### C — C backend (milestone 2 backend)

**Goal:** `jog_emit_c` turns any valid JIR module into one C17 file that compiles warning-free with clang and gcc, and `jir_dump` prints JIR as text.

**Steps:**
1. **`ir_dump.c`:** a stable, readable text form of JIR. Lowering and interpreter tests will compare against it.
2. **Types.** Map every `JogType` kind to C following [[plans/V1Compiler]]:
   - Integers map to `<stdint.h>` types.
   - `bytes[N]` becomes a wrapped array.
   - Slices, `bytes`, and `str` become `{ptr, len}`.
   - Tuples become structs.
   - An option is a nullable pointer for the niche case, and `{has, value}` otherwise.
   - Enums become a tag plus a union.
   - Distinct types become their base type.

   Emit the definitions in dependency order, deduplicated by `JogType.id`.
3. **Functions and instructions.** Prototypes first. Locals become C locals, blocks become labels, terminators become `goto`/`if`/`switch`/`return`.
   - Overflow checks use `__builtin_*_overflow`, and division by zero is checked.
   - `BOUNDS_CHECK` and `NULL_CHECK` call `jogrt_panic_*`.
   - Positions become `#line` directives.
   - `extern` functions become prototypes; variadic ones end in `...`.
   - In `release-fast` builds, bounds checks are left out.
4. **Globals and constants:** string constants as static read-only data, and the global data bytes.

**Tests:** `tests/emit_c_test.c` builds IR by hand with the `ir.h` builder, emits C, compiles it with the C compiler on `PATH`, links it with the runtime, runs it, and checks stdout and the exit status. Until R's runtime merges, the test may link a minimal stub runtime of its own from `tests/ir/`. Keep one test per instruction and terminator, at least.

**Done when** every `JirOp` and every terminator has an execution test, and the output compiles warning-free under `-Wall -Wextra` with both compilers.

### D — Driver (milestones 2 and 4 plumbing)

**Goal:** everything around the phases: process spawning, finding and loading packages, invoking the C compiler, the commands, and the end-to-end test harness.

**Steps:**
1. **`tools/proc.h/.c`:** run a program with an argv array, wait for it, and return its exit code. Quote arguments correctly on Windows (`CreateProcess`) and use `posix_spawn` elsewhere. Optionally capture stdout and stderr to files. Cover it in `util_test.c`; D may add to that file.
2. **`cc.c`:** find clang, then gcc, on `PATH` (honor `$JOG_CC`). Build the command for each `JogBuildMode`, compile `runtime/jogrt.c` and `jogrt_main.c` with the program, and add `-l` for each `link` library and the builtin-mismatch warning flags. Test it by building a hand-written C program that uses `jogrt.h`.
3. **`loader.c`:**
   - Parse `jog.mod`, searching upward from the start path.
   - Map import paths to folders: `std` first, then local folders.
   - Read and lex the `*.jog` files with `ct_fs_list`, then parse them. `*.test.jog` files are included only when `include_tests` is set.
   - Detect import cycles and report the chain.
   - Order packages so every package comes after the packages it imports.

   Test with fixture folders under `tests/jog/loader/`. Until the parser lands, check the package graph and the file lists. `jog_parse` is the stub then, so ignore its error in this test.
4. **Commands and harness.** `cmd_build.c` runs the pipeline `jog_load` → `jog_check` → `jog_lower` → `jog_emit_c` → `jog_cc_build` for `build`, `run`, `check`, `emit-c`, and `emit-ir`, with the flags in [[plans/V1Compiler]]. `tests/e2e_test.c` compiles and runs every `tests/jog/*.jog` program, compares its output with the file's `// expect: ...` lines, and reports skips until the pipeline exists.

**Done when** `jog build` on `examples/hello` reaches the first unimplemented phase with a clean message, the loader tests pass, and `cc.c` builds and runs a C test program.

### T — Checker (wave 1: foundation, wave 2: milestone 2 subset)

**Goal:** name resolution and type checking that fill `JogNode.type` and `JogNode.symbol`, in files split by area so feature slices can add files later.

**Wave 1 steps:** none of these need the parser.
1. **`symbols.h`:** `JogSymbol` with its kind (package, type, function, variable, constant, field, method, variant, parameter, context), declaring node, type, and owning package; package scopes; and block scope chains on `CtMap`. Lowering depends on this header, so publish it first, as T's first pull request.
2. **The universe scope:** the std scalar names (`i8` … `u64`, `isize`, `usize`, `f32`, `f64`, `bool`, `str`, `bytes`) mapped to `JogTypes` built-ins, plus `true`, `false`, `null`, and `void`.
3. **`const_eval.c`:** constant folding over literal and operator nodes with exact integer arithmetic, overflow as a compile error, untyped constants defaulting to `i64`, and hex literals interpreted per JogDocs: design/language/Types.md. Test it with nodes built by hand.

**Wave 2 steps**, once P step 2 merges:
4. **Package scopes** from top-level declarations, imports, and visibility.
5. **On-demand checking of top-level declarations.** Each declaration gets a coroutine from `tools/coro.h`, and cycles are reported with the wait chain, as described in [[plans/V1Compiler]]. If coroutine stacks prove too small, raise it as a hand-off note for a contract change to `coro.h`.
6. **The milestone 2 subset:** functions, `val`/`var`, the integer types, `bool`, `str`, `if`, `for`, calls, `extern` blocks, `c"..."`, comparisons and arithmetic, with no implicit conversions and with definite assignment.

**Tests:** `tests/check_test.c` covers the pure parts. `tests/check/*.jog` files have `// error: <text>` lines that `check_test` compares with the diagnostics.

## Wave 2 stream

### L — Lowering (starts when T step 6 is close)

**Goal:** `jog_lower` turns a checked program into JIR that passes `jir_verify`.

**Steps:**
1. Mangling, following the shared conventions above. Functions and parameters, locals for `val`/`var`, expressions into temporaries, `if`/`for`/`break`/`continue` as blocks, calls, and `extern` functions with `JIR_LINK_EXTERN`.
2. The init function for global initializers, in the order the checker computed, and `main_func`.
3. Leave room for milestone 3: plan `defer` and `errdefer` as cleanup blocks on every exit edge, and `when` as a `SWITCH` plus compare chains.

**Tests:** `tests/lower_test.c` runs parse, check, and lower on small snippets and compares the `jir_dump` output with golden files.

### I — Interpreter (optional in wave 1, needed for milestone 7)

**Goal:** `jir_interp_call` runs JIR at compile time with target layout. Memory is a byte arena, pointers are offsets into it, and there is a step budget. Panics become compile errors with a stack of `jog_name`s.

**Tests:** IR built by hand, like C's tests, comparing the result bytes. Then the same IR run through C and through I, with matching results. That is the best check that both backends agree.

## Milestone 2 gate

D owns the integration pull request. It makes `examples/hello/main.jog` and the first `tests/jog/*.jog` programs build and run end to end, and fixes small mismatches by opening hand-off notes to the owners. After the gate, `jog run examples/hello` prints `Hello from Jog!`.

## Wave 3: feature slices (milestone 3 onward)

Every feature after milestone 2 cuts across T, L, and C. Instead of three streams editing the same files, one agent owns a **slice**. It adds `check_<feature>.c` and `lower_<feature>.c`, plus small dispatch hooks in the core files, which are the only shared edits and should stay minimal. It adds C backend cases only if the feature needs new JIR. T and L must keep their core files dispatching by node kind, so slices plug in.

| Slice | Milestone | Covers |
|---|---|---|
| structs | 3 | struct declarations, literals, fields, methods, `self`, embedding, extensions |
| memory | 3 | pointers, `&`/`*`, arrays, slices, `bytes`, array and slice literals, bounds checks, pointer arithmetic |
| errors | 3 | tuples, multiple returns, destructuring, `error?`, `defer`, `errdefer`, panics |
| enums | 3 | enums, `when`, patterns, exhaustiveness, `Option`, `?.`, `?:`, `!!`, `is` |
| packages | 4 | imports across packages, visibility, global init order, `jog test` (D leads) |
| contexts | 5 | `with` clauses, hidden parameters, `context` bindings, inference for private functions |
| generics | 5 | type parameters, constraints, instantiation, and the mangling of instances |
| interfaces | 5 | method tables, conversion to an interface, `as?`, type ids |
| strings | 5 | interpolation through the formatter, `fmt` |
| coroutines | 6 | `go`, `scope`, `Task`, `Chan`, `select`; runtime scheduler (R) |
| comptime | 7 | top-level `val` through I, `@comptime`, `@embed` |

## Hand-off notes

When a stream needs something from another, it adds a line here: who needs what from whom, and why. The owner removes the line when the change merges.

- C, for lowering: a variadic prototype with no named parameter is emitted as `(int jog_va, ...)`, and a call passes `0` for `jog_va`. A parameter list of only `...` is not legal C. Jog function definitions use the JIR parameters; `void *env` belongs only to the closure struct that is the type of a function value.
- L: T step 6 types the milestone 2 subset. `c"..."` is `*const i8` (host `*const c.char`). `for i in a..b` binds `i` as a constant of the bounds' integer type.
- interfaces: method tables, structural method satisfaction in the declaring package, pointer-to-interface conversion (including a context parameter), calls through the table, `as` / `as?`, and type ids. A type id is the address of a distinct static byte (`jog_tid_<id>`), not a module-manifest TypeInfo entry. Universe `error` stays an empty unchecked interface and is not a satisfaction target. Embedding a method interface merges its requirements into satisfaction and into calls.
- interfaces: a property load reads the table's offset slot, then adds that byte offset to the data pointer. Offset slots are `size_t` fields after the method pointers. A property is a field, including one promoted from an embedded struct. `var` can be stored; `val` cannot. `*const T` does not satisfy an interface with a `var` property. `v is T` compares type ids. Inside `if`, on the right of `&&`, and in a `when` arm `is T`, a `val` or a parameter narrows to `*T`. A `var` does not. Type ids stay addresses of distinct static bytes.
- interfaces: `std.mem.Malloc` is the malloc-backed value. Its methods call `jogrt_alloc`, `jogrt_realloc`, and `jogrt_free`. `mem.heap()` returns that value as `Allocator`. Out of memory panics, because a concrete `error` value cannot be built. `Scope` and `Testing` are not declared in std, and a `Testing` context on `@test` is not wired. `std.io` `print`, `println`, `eprint`, and `eprintln` only call `jogrt_write`.
- generics, after step 6: constraints are checked. Generic interfaces are not a separate mechanism. `Allocator` is not generic, and `T: Ordered` is not `Ordered<T>`. `interface Foo<T>` stays rejected (`generic types are not checked yet`). A struct or enum literal does not infer type arguments. `type A<T>: B` is not checked: the notes show type parameters on the transparent form `type Pair<A, B> = (A, B)` and are silent on a distinct type with type parameters. A method is not declared on a generic alias. Interface method requirements still cannot have type parameters. `fn List<i32>.get` declares a type parameter named `i32` for every instance of `List`; it is not a method of the single instance `List<i32>`. The notes say the receiver declares the type parameters and do not show a method of one concrete list. The name hides universe `i32` inside that method.
- strings: `fmt` is the compiler builtin the notes call std's `fmt`. A user function named `fmt` stays an ordinary function. The template is a string literal or an interpolated string, and both are checked the same way. A hole is an integer, a bool, or `str`/`bytes`; any other type reports `type %s cannot be formatted`. The std `Format` interface and `Writer` are still absent, so a user type has no `format` method in this step. `fmt(buf, template)` returns `(str, error?)`. The error is present when a piece does not fit in `buf`. An interpolated string used as a value has type `str` and requires an `Allocator` context already in scope. Interpolation does not add a context during private-function inference, so the function declares `with (a: Allocator)`. The bytes come from `jogrt_alloc`, the same backing store as `mem.heap()`. The `Allocator` value is the proof that the function allocates; it is not asked to `alloc`.
- strings: a user type fills a fmt hole when its own package declares `fn T.format(buf: bytes): (str, error?)` or `fn *T.format(buf: bytes): (str, error?)`. The compiler passes the remaining room in the destination, a null pointer and a zero length when measuring, and the method writes that buffer. The returned `str` length advances the cursor. A present error sets the same overflow flag a scalar piece sets when it does not fit. A method body that returns `fmt(buf, ...)` measures and writes. Interpolation calls the method twice, once to measure and once to write; `fmt` calls it once. A pointer hole uses the pointee's method. A method on an embedded struct is used when that embed path is unique, and a method on the outer type wins. An extension declared in another package does not count. Integer, bool, and `str`/`bytes` still use the scalar formatters and do not call a method. The wrong shape reports `format method must be fn format(buf: bytes): (str, error?)`. Two promoted methods report `ambiguous format method`. A type with no such method still reports `type %s cannot be formatted`. `Writer` is still absent, so this method stands in for `fn format(w: *Writer)`.
- coroutines: `go`, `scope`, `Task<T>`, `Chan<T>`, and `select` run on one cooperative thread. `tests/jog/coro.jog` sends on one task and receives on another, then prints the value. `Chan.init`, `go`, and `sleep` need an `Allocator` context as proof only; channel bytes and stacks come from the OS allocator. Root `fn main` has the root `Scope` implicitly. Other functions need a `Scope` context. `scope { }` returns `error?`. A `go` or `scope` tail whose type is `error?` fails that scope when the tag is set. `select` is syntax inside its block (`recv`, `send`, `timeout`, `default`); those names are not values. `timeout` takes integer milliseconds. `ErrCancelled` and `ErrClosed` are universe `error` values. Still open: `for` over a channel, multi-thread scheduling, the I/O poller, `Executor`, and Wasm. `--starve-ms` is not a flag; a slice between suspends longer than 50 ms warns on stderr with the launch file and line.
- coroutines, capture: a compound assignment of a captured local reads the capture, then the right-hand side, then stores through it, matching a local `a = a + e`. `go f(x)` lowers the call in the child, so each argument runs when that child runs, after the launcher has continued. `tests/jog/coro_capture.jog` assigns a local after `go id(n + 1)`, calls `bump()` only from an argument, and prints the value the child saw, then prints the result of `+=` through `>>=` on a captured local. A captured pointer `p += n` or `p -= n` loads that pointer, then the offset, then stores the moved pointer. `return a, b` inside `go` is one tuple, so `join` yields that tuple and a later return must match it; a present `error?` in the last position fails the scope. `tests/jog/coro_ptr.jog` prints the element and index after `+=` and `-=`, the index when the right-hand side moves the same pointer, both tuple values, and that a closed receive returned from `go` fails the scope. `&` of a captured local still fails in memory lowering.
- comptime: pointer-free top-level `val` and `@comptime` run in the JIR interpreter and become const data with no `JIR_STORE`. `@embed("path")` takes one string literal, reads that file relative to the source file, and has type `const bytes`. `.` and `..` are collapsed lexically and the file must stay inside the module; a missing file or a directory is an error. `tests/jog/embed.jog` embeds `jog.mod` and prints `ok`. When `--comptime-steps` is absent the budget is 10 million instructions. A `val` whose type contains a pointer still initializes at startup (no relocations), including a `val` that holds embedded bytes.
- collection for: `for x in xs` and `for i, x in xs` walk an array, a slice, `str`, `bytes`, or `bytes[N]`. `x` is a constant copy of the element (`T` for `[N]T` and `[]T`, `bytes[1]` for `str`, `bytes`, and `bytes[N]`). `i` is a constant `usize`. The collection expression is evaluated once. An array or `bytes[N]` is copied, so writes to the original are not visible. A slice, `str`, or `bytes` header is copied, so later stores through that same memory are visible and reassigning the view is not. `continue` increments the index. A range `for i in a..b` is unchanged. `for` over a channel, and `for i, x in a..b`, are still not checked. The for node stores the index and element symbols in `bound_symbols`, NULL when the name is `_`.
- wasm: Step 2 is freestanding wasm32, not WASI. There is no wasi-libc sysroot, and the native scheduler uses OS stacks Wasm cannot link. `jog build` and `jog run` take `--target wasm32`; the type table's pointer size is 4. The output path ends in `.wasm`. clang compiles generated C with `--target=wasm32-unknown-unknown -ffreestanding -nostdlib` and wasm-ld (`--no-entry`, export `main` and `memory`, `--allow-undefined`, 1 MiB stack). `runtime/wasm/jogrt_wasm.c` is that runtime and does not use libc. Undefined `extern` symbols are imported from `env` (`puts` for hello). The runtime's own host imports are `jogrt_host_write`, `jogrt_host_abort`, and `jogrt_host_exit`. `jog run` executes the module with `node` and `runtime/wasm/host.js`. `jog run --target wasm32 examples/hello` prints `Hello from Jog!`. `go` runs its body to completion; any other scheduler call aborts. Still open: WASI and stack switching.
- wasm host: arguments after `--` reach the wasm program in the same positions as native `jog run`: argv[0] is the module path and the following words are the arguments. `@link("name")` on an extern block is the Wasm import module for those functions; `@link` on one function replaces the block. The driver records that map and the wasm link emits clang `import_module` and `import_name` attributes. Externs with no `@link` stay `env` imports, including hello's `puts` and the runtime imports `jogrt_host_write`, `jogrt_host_abort`, and `jogrt_host_exit`. The Node host defines `puts`, `jog_host_marker`, and `jog_host_ok` in the module that imports them. A `jog.mod` `link` line is still a native `-l` flag and is still rejected for wasm32. `--comptime-steps N` is a positive integer; when it is present the interpreter uses that budget, and when it is absent the budget stays 10 million. `go` on wasm still runs its body immediately. WASI and stack switching stay open.
- errors: destructuring assignment writes an lvalue: a name, `_`, `*p`, an index, a struct field, a tuple field, or a nested tuple of those. A tuple field dereferences one pointer, so `p.0` is `(*p).0`. `return a, b` lowers when the result is a tuple of that arity, including after generic substitution. A tuple literal on the right gives each element the slot's type, so `(a, b) = (1, 2)` accepts `i32` slots. Still open: top-level `val (a, b) = ...` reports `destructuring is not lowered yet` from `declare_global` in lower.c, which does not call this slice. `multiple return values are not lowered yet` remains when the result is not a tuple of that arity; the checker already rejects that. `tuple field is not lowered yet` remains when the base is neither a tuple nor a pointer to a tuple.
- contexts: a bare root `fn main()` with no with clause receives Allocator (`std.mem.heap()`), ErrorAllocator (distinct over Allocator; the value is `std.mem.error_heap()` bitcast; `ErrorArena` methods call `jogrt_error_alloc`; free is a no-op; resize allocates a new block and copies), Scope (`jogrt_scope_root`, first use only), and Logger (`std.io.logger()`; `Stderr.write` calls `eprint`). Installed only when the type is already loaded: Scope always, Allocator and ErrorAllocator when `std.mem` is loaded, Logger when `std.io` is loaded. They are locals on first use, not parameters of `jog_entry_main`. Explicit `as` between a distinct type and its immediate base is the wrap (`ea as Allocator`). Testing is still absent. No module-level injection. `check_body.c` calls `jog_check_context_cast` from `check_cast`. `lower.c` calls `jog_lower_context_default` from name lookup and `jog_lower_context_cast` from casts.
- io: `read_all(path)` returns `(bytes, bool)` and takes an `Allocator` context. It copies the `str` path into a temporary buffer with a trailing NUL, then reads the whole file. The bytes come from that allocator. `false` means open, read, or close failed, and the bytes are empty. `write_all(path, data)` returns `bool` and creates or replaces the file. `false` means open, write, or close failed. Failure is a `bool`, matching `print`. There is no `File`, no `error?`, and no wasm file import. A path that does not fit in the temporary buffer fails the open.
- enums: a variant field default is checked like a struct field default and is filled at the call when that field is omitted. A field with no default is still required. A default whose type mentions a type parameter stays rejected. `when` patterns nest: a payload may be an enum variant, a struct pattern (`Point(x: 0, y)`), a literal, or `in a..b`. A named field whose pattern is not itself a bind, such as `code: in 1..10`, binds that field name. `Some(value: v)` names the option field. Exhaustiveness walks every level, so `Wrap(A(n))` and `Wrap(B)` together cover `Wrap`, while `P(One, Two)` and `P(Two, One)` do not cover `P`. Still open: `in` on a collection reports `in pattern needs a range`. A tuple pattern written `(1, 2) ->` is parsed as a lambda before this slice sees it. Float and char patterns stay with the body slice. Interface `when` arms that are not `is T` still report `pattern is not lowered yet` from `lower_interface.c`. An or-pattern still cannot bind a name. `is` on an enum still does not narrow.
- errors: a function may return a concrete error, not only null. A pointer to a struct or enum that declares `message(): str` in the same package converts to `error?` and to `error`. The value is that address in both words of the interface, the same shape as `ErrCancelled`, wrapped as a present option. `error? ==` that pointer, or `error? ==` an error such as `ErrCancelled`, compares the data pointer. `error? == error?` stays rejected. `&expr` is boxed wherever the conversion is applied; a name of the pointer type is boxed on return. `std.testing.Testing` provides `expect`, `expectEq` for `str`, `fail`, `fatal`, and `expectPanic`. An `@test` `with (t: Testing)` receives `testing()` when no context global has that type. `read_all` stays `(bytes, bool)`. `message` is not called through the universe interface. Out of memory still panics.
- body: `f32` and `f64` literals and arithmetic typecheck and lower, and a character literal is one Unicode code point as an untyped integer that defaults to `i64`. An untyped float stays soft until a typed float wins; otherwise it is `f64`. A constant that does not fit, and constant float division by zero, are errors. `as` between an integer and a float follows the notes. A float literal global is const data. Float remainder is a generated `jog_fmod` / `jog_fmodf`, not libm. A non-literal float value still cannot run at compile time.
- body: `x?.len` and `x?.ptr` on an option of `str`, `bytes`, or a slice are `when x { Some(v) -> Some(v.len or v.ptr), None -> None }`. A struct field, including through one pointer, is still that same sugar and still lowers in the enum slice. `?.` on a value that is not an option is still `operator '?.' needs an option`.
- body: a non-capturing lambda checks and lowers, including a trailing lambda and implicit `it`, and a named function used as a value is the closure `{ thunk, null }`. The thunk is generated C (`jog_fnthunk_N`) because a Jog definition does not take `void *env`. `return` in a lambda returns from the lambda only. A non-escaping lambda captures an outer local by reference: the caller builds a stack tuple of pointers and the closure's env points at it. The lambda's C function takes that `void *` after its Jog parameters, so the closure code pointer is the lambda, not a thunk. A unit lambda discards a non-unit tail, so `() -> { xs.pop() }` has type `() -> ()`. An interface call accepts a trailing lambda in its last parameter. `expectPanic` passes that closure to `jogrt_panic_call`, which `setjmp`s and `longjmp`s out of `jogrt_panic` instead of aborting. Escaping copies and `f.free()` are still open. `go` inside a capturing lambda is still not lowered. `select` stays syntax. A generic function, and a function with a context parameter, are not values yet.
- body: `for v in ch` receives until `jogrt_chan_recv` reports the channel closed (status 2) and then stops. `v` is a constant of the channel's element type. A second name is `for-in over a channel has one name`. Status 1 cancels the current scope through `jogrt_scope_fail` and `jogrt_err_cancelled`, then the loop stops. `for` over any other non-collection is still `for-in over a collection is not checked yet`.
- body: a `val` whose type contains a pointer, including `@embed` bytes, is const data when the initializer is a constant image. Pointer slots stay zero and a relocation names the string, global, or function. Struct, enum, and tuple globals with initializers lower. Top-level `val (a, b) = ...` is one global per name; comptime does not evaluate one of those names as the whole tuple. A slice literal at global scope is still `slice literal in a global initializer does not outlive initialization`. Distinct, error, and interface globals stay rejected.
- errors: `t.0 = 1` stores the element. `jog_check_field_assign` accepts a nameless member after `jog_check_error_field`: the place is a `var`, a parameter, or one non-const pointer (`p.0` is `(*p).0`), and the right-hand side takes the element type. Integer compound operators match a struct field. `lower_assign` stores through `jog_lower_tuple_field_ptr` and does not call `jog_lower_field_assign`. `tests/jog/tuple_assign.jog` assigns `t.0` and prints the new value. Destructuring `(t.0, t.1) = ...` is unchanged.
- pack parameters: `...T: name` on a Jog function is a slice local `[]T`. The caller builds a stack array and passes the slice header `{ptr, len}`. Zero arguments is a zero slice. A parameter after the pack is a compile error. Bare `...` stays the extern C variadic. Interface methods, context parameters, and enum payloads cannot use a pack.

## Contract change log

- *(baseline)*: `token.h`, `ast.h`, `types.h`, `ir.h`, `program.h`, `phases.h`, `commands.h`, `jogrt.h`, with stubs and `contracts_test.c`.
- `program.h`: added `JogProgram.link_libs` and `link_lib_count` at the end of the struct. They record `link` lines from `jog.mod` for the C driver. No existing field changed meaning.
- `symbols.h`: added `JogPackageScope.file_scopes`, `file_scope_count`, and `universe` at the end of the struct. File scopes hold imports and file-private top-level names. `universe` is the builtin scope a file lookup falls through to. No existing field changed meaning, and `jog_package_scope_lookup` still searches only `bindings`.
- `ast.h`: added `JogNode.bound_symbols` and `JogNode.bound_symbol_count` at the end of the struct. A destructuring binding stores one symbol per name, or NULL when that name is discarded. No existing field changed meaning.
- `ast.h`: added `JogNode.context_args`, `context_arg_count`, `context_symbols`, and `context_symbol_count` at the end of the struct, after the destructuring fields. A call stores one expression per callee context parameter. A `with` statement stores one symbol per value. No existing field changed meaning.
- `ast.h`: added `JogNode.solved_type_args` and `solved_type_arg_count` at the end of the struct, after the context fields. A generic call stores one solved type argument per type parameter. No existing field changed meaning.
- `ast.h`: added `JogNode.as.member.type_args` at the end of the member struct. A method call `obj.method<T>(x)` stores those explicit type arguments on the member. No existing field changed meaning.
- `ir.h`: added `JirModule.iface_tables` and `iface_table_count` at the end of the struct. Lowering records one method table per concrete type and interface. No existing field changed meaning.
- `jogrt.h`: added the milestone-6 scheduler declarations at the end of the header, after the existing runtime functions. Channel and select status is 0 ok, 1 cancelled, 2 closed. `jogrt_go`'s `stack_bytes` 0 selects the 256 KiB stack. No existing declaration changed meaning.
- `ir.h`: added `JirReloc` and, at the end of `JirGlobal`, `relocs` and `reloc_count`. A pointer slot in a global image stays zero; the reloc names a string, cstring, global, or function. No existing field changed meaning.
- `ast.h`: added `JogNode.as.param.pack` at the end of the param struct. True for `...T: name`. The checked type of that parameter is a slice of T. No existing field changed meaning.
- `program.h`: added `JogOptions.start_package` at the end of the struct. When it is set, the loader also loads the package of the folder that contains the start path, so `jog check <module>/sub/x.jog` checks `sub` even when the root does not import it. Only `jog check` with a path argument sets it. No existing field changed meaning.
- `jogrt.h`: added `jogrt_stdin_read` and `jogrt_stdio_binary` after `jogrt_write`. `std.io.read_some` and `std.io.binary_stdio` call them. The wasm runtime returns -1 and true. No existing declaration changed meaning.
- `program.h` (P2.5, JogDocs: design/Packages.md): added `JogModuleArg` (`name`, `dir`); `JogPackage.module_name` and `module_root` at the end of the struct (the owning module, `"std"` and `std_root` for std); and `JogProgram.module_args`, `module_arg_count`, and `is_library` at the end of the struct. The driver sets `module_args` from `--module` before `jog_load`. `JogPackage.path` is now the canonical path: unchanged in the root module, `<module>` and `<module>.<rest>` in a dependency. `JogProgram.link_libs` now holds the root's link lines and then each mapped module's, `link.<os>` lines for the host only, with a repeated name kept at its last position. With no `--module` and a V1 `jog.mod`, every existing field has the same value as before, except that a link name written twice is now stored once.
- `jogrt.h`: added `jogrt_dirent_name`, `jogrt_mkdir`, `jogrt_spawn_actions_new`, `jogrt_spawn_actions_adddup2`, `jogrt_spawn_actions_free`, and `jogrt_wexitstatus` at the end of the header. std.fs and std.proc call them so POSIX code does not hard-code `struct dirent`, `mode_t`, `posix_spawn_file_actions_t`, or `WEXITSTATUS`. On Windows and wasm they return failure and do not spawn. No existing declaration changed meaning.
- `ast.h`: added `JogNode.as.fn.is_expect` and `is_actual` at the end of the fn struct. `is_expect` is an `expect fn` (no body). `is_actual` is an `actual fn` (the body; visibility comes from the expect). No existing field changed meaning.
- `program.h`: added `JogTargetSources` and, at the end of `JogPackage`, `targets`, `target_count`, `target_sources`, `platform_files`, and `platform_file_count`. They record a module's `targets` line and the OS-suffixed files of each listed target. Absent `targets` leaves the new fields null, and packages load as before. No existing field changed meaning.

## Starting an agent

Give each agent this prompt, with the stream filled in:

> You are implementing stream **X** of the Jog V1 compiler in this repository. Read `docs/plans/V1Parallel.md` (your stream's section, the ground rules, and the contracts), `docs/plans/V1Compiler.md`, and the language notes your stream touches in [JogDocs](https://github.com/Jog-lang/JogDocs) (`design/language/`). A local checkout is `../JogDocs`, or `../Jog/JogDocs`. Work on branch `v1/<stream>-<step>`, one step per pull request. Edit only files your stream owns. Do not change contract headers except through the contract change process. Before you finish a step, the check command must print `check: OK`: `pwsh scripts/check.ps1` on Windows, or `scripts/check.sh` on Linux. Record decisions the language notes did not cover in those notes, and add hand-off notes to the plan when you need something from another stream.

## Stream notes

### D

- `JOG_ROOT`, when set and non-empty, is the directory that contains `std/` and `runtime/`. Otherwise those directories are `<directory of the jog executable>/../std` and `../runtime`.
- `JogProgram.std_root` is the `std` directory itself, so `import std.io` loads `std_root/io`.
- `jog.mod` `link` names are stored, in order, on `JogProgram.link_libs`. With `--module`, the mapped modules' lines follow the root's; see the P2.5 note below.
- An import cycle is reported as a chain of import paths. The back-edge is omitted from `JogPackage.imports`, so `JogProgram.packages` stays in dependency order for every edge that was kept.
- A package folder with no `.jog` files in the build (test files count only when `include_tests` is set) is an error.
- With no package argument the start path is `.`. Generated C is `<module root>/out/<module name>.c`. The executable is that directory and base name, with `.exe` on Windows, unless `-o` sets it. `--target wasm32` writes `<name>.wasm` instead, and `-o` for that target must end in `.wasm`. A native `-o` path must not end in `.wasm`. `--keep-c` keeps the C file; a successful link without the flag deletes it, and a failed link leaves it. `jog run` returns the program's exit status. The type table uses pointer size 4 for `--target wasm32` and 8 otherwise.
- `build`, `run`, `check`, `emit-c`, and `emit-ir` share `jog_load` → `jog_check` → `jog_lower` → `jog_emit_c` → `jog_cc_build`. `check` stops after the checker, `emit-ir` prints `jir_dump` after lowering, and `emit-c` prints the C after `jog_emit_c`. A phase that reports an error is the last one. The driver prints at most 50 diagnostics, then a count of any left out.
- `examples/hello/jog.mod` declares `module hello`, so `jog build examples/hello` loads that folder.
- `tests/e2e_test.c` runs each flat `tests/jog/*.jog` file (`*.test.jog` excluded) as its own temporary module. `// expect:` lines are the whole stdout. A compile that says a phase is not implemented is reported as a skip.
- `jog check <path>` with a file or folder below the module root also checks the package of that folder and its imports, even when the root does not import it. A folder on that path that is not an identifier is an error. `jog check` with no argument, or with the root, checks the root package and its imports, as before. Other commands ignore the start folder beyond finding `jog.mod`.
- Paths, arguments, and environment values are UTF-8 inside the tools. On Windows, `main` reads the UTF-16 command line, and `tools/fs.h` (`ct_fs_fopen`, `ct_fs_remove`, `ct_fs_mkdir`, `ct_fs_getenv`, `ct_fs_widen`, `ct_fs_narrow`) and `tools/proc.c` convert at the OS boundary and call the W APIs. Generated programs also take argv from the UTF-16 command line, in `jogrt_main.c`.
- `*.test.jog` files are loaded only for packages of the module being tested, never for std packages or mapped modules.
- P2.5 (see JogDocs: design/Packages.md): `jog.mod` v2 is parsed in `loader.c`. `build`, `run`, `test`, `check`, `emit-c`, and `emit-ir` accept a repeatable `--module <name>=<dir>` or `--module=<name>=<dir>`. A value without `=`, or with an empty side, is usage (exit 2); the loader checks the name and the folder. The first segment of an import in module M is std, then a module M's `jog.mod` requires (through its mapping), then a folder under M's root. A dependency's packages have canonical paths `<name>` and `<name>.<rest>`, and the resolver binds an import through `JogPackage.imports`. Packages of two modules whose C prefixes would match (`lib.util` and `lib_util`) are an error. `jog build` and `jog run` on a `kind library` module fail; `check` and `test` load its root and every exported package. The compiler version is `JOG_VERSION` in `compiler/src/version.h`.

### T — checker

- `const_eval.c` folds literal and operator trees exactly, as described in JogDocs: design/language/Types.md. Untyped integers default to `i64`. `<=>` is left unfolded until `Ordering` is a type the checker can name. Float constants are not folded in this step.
- Package scopes hold public and `private(package)` top-level names. Imports, `private(file)`, and top-level `private(scope)` live in a per-file block scope that falls through to the package scope, then to the universe scope. The same file-private name may be declared in another file. A file-private name that matches a package-visible name in the same package is a duplicate. Methods are recorded on their nodes and are not package names. Unused imports are left for the step that resolves uses.
- `void` is a universe type, an incomplete nominal struct with no fields and no layout, distinct from `()`. It must not be passed to `jog_type_layout`. `*void` and `*const void` are the spellings in JogDocs: design/language/CInterop.md.
- Milestone 2 bodies are checked after the signature is published, so a call waits on the callee's type and not on its body. Numeric conversions are explicit (`as` between integers). A `var` with no zero value must be assigned on every path before a read; a top-level one must have an initializer. `c"..."` is `*const i8`. `for i in a..b` is a half-open integer range and `i` is a constant. Top-level `val` initializers are typechecked but not required to be foldable constants. A pointer-free `val` is evaluated at compile time; see the comptime note.

### C — C backend

- `JIR_OPND_STRING` and `JIR_OPND_CSTRING` are `static const uint8_t jog_sN[]` and `jog_cN[]`. `N` is the index in one table. A cstring array includes the trailing NUL. Identical bytes are shared.
- A `JirGlobal` is `static`, and `const` when `is_const` and no instruction stores it. `init_func` stores a non-constant top-level `val` once, so that object is writable and the assignment is well-formed C. Folded integer and bool data with no store stays `const`. `name` is emitted as a C identifier.
- `data` is little-endian target layout (x64 and wasm32). A zero length is a zero initializer. A non-zero pointer or function pointer in that image is an error; relocations come later. Non-zero padding is an error.
- `jog_entry_init` takes the address of every emitted global so an unused `static` is not a `-Wall` failure.
- Universe `void` has no C object and is not passed to `jog_type_layout`. `*void` and `*const void` are `void *` and `const void *`, not pointers to an incomplete struct.

### L — lowering

- Top-level `val` and `var` of `bool`, an integer, or `str` are globals named `jog_<pkg>__<name>`. `val` is `is_const`. An integer or bool initializer that `jog_const_eval` folds is little-endian data and does not run again. A pointer-free `val` that is not one of those folds is interpreted and stored as const data with no store. Every other initializer runs in `init_func` (`jog_<module>__0init`, internal, `() -> ()`). The visit order is the checker's: source order, round-robin (packages in dependency order, files in file order, declarations in source order). An initializer runs on the visit where every global it names is already done, and it does not jump ahead when that dependency finishes, so a later independent initializer can run first. A use loads the global. An assignment stores, and a compound assignment reads the global before the right-hand side. A global with no initializer is a zero image. `init_func` stays `-1` when nothing needs to run. `main_func` is still only a non-extern root function named `main`.
- The checker does not order an initializer after globals that are read only inside a function it calls. Lowering uses that same direct-use order. Distinct types are not lowered. Unconstrained function type parameters are lowered in `lower_generic.c`; the template is not emitted. Context parameters, `with` blocks, and `context` bindings are lowered in `lower_context.c`. Enums and `when` are lowered in `lower_enum.c` as a tag `SWITCH` plus compare chains.
- Integer and bool constant trees are folded with `jog_const_eval` before they become instructions, so `200 + 100 - 250` as `u8` is the constant `50` and not a checked add that would panic.
- `str` and `bytes` `==` / `!=` compare lengths, then bytes (`extract`, `elem_addr`, `load`). Unequal lengths do not read memory. `&&` and `||` are short-circuit blocks. `JIR_AND` and `JIR_OR` stay bitwise.
- `for i in a..b` evaluates both bounds once, keeps the end in a temporary, and mutates a local named `i`. `continue` targets the checked increment. A condition `for` reevaluates its condition. A `for` with no condition uses its head block as the body.
- A variadic function with no named parameter has no JIR parameter and the call does not pass `jog_va`. The C backend inserts `(int jog_va, ...)` and the leading `0`. Ordinary Jog functions do not gain a `void *env` parameter. Using a Jog function as a value is an error in this step.
- Public names are `JIR_LINK_PUBLIC`. `private`, `private(file)`, and `private(scope)` are `JIR_LINK_INTERNAL`. `@export` and `@export("name")` are `JIR_LINK_EXPORT` and keep that C name. `extern` is `JIR_LINK_EXTERN` and keeps the declared C name. Operator methods spell `op+` as `op_add`, `op-` `op_sub`, `op*` `op_mul`, `op/` `op_div`, `op%` `op_rem`, `op&` `op_and`, `op|` `op_or`, `op^` `op_xor`, `op<<` `op_shl`, `op>>` `op_shr`, `op==` `op_eq`, `op!=` `op_ne`, `op<` `op_lt`, `op<=` `op_le`, `op>` `op_gt`, `op>=` `op_ge`, `op<=>` `op_cmp`, `op[]` `op_index`, `op[]=` `op_index_set`, `op!` `op_not`, `op~` `op_bitnot`.
- `+`, `-`, `*`, `/`, `%`, shifts, unary `-`, and integer `as` are checked. Bitwise ops and comparisons are not. A `var` whose type has no zero value (a pointer or a function) is left uninitialized. Compound assignment reads the local before the right-hand side. Named arguments are evaluated in source order and passed in parameter order; omitted defaults are evaluated at the call site.

### enums

- `T?` stays the structural option type. There is no implicit conversion from `T` to `T?`, and this slice does not build a nominal `Option` enum. `null`, `T? == null`, and niche `*T?` stay in `check_error.c` and `lower_error.c`.
- A `when` on an enum is `JIR_ENUM_TAG` plus `JIR_TERM_SWITCH`. Integer literal arms, including comma lists, are a switch on the value; the first duplicate value wins. Ranges, option patterns, condition-only `when`, and refutable payloads are compare chains. An `in a..b` pattern is half-open, the same rule as `for i in a..b`.
- An or-pattern cannot bind a name. `is` tests an enum tag and does not narrow the subject. Generic enums, enum methods, interface `is`, enum equality, float patterns, and `in` on a collection are not in this slice.

### packages

- A package with a runtime initializer is lowered to `jog_<pkg>__0pkginit`. When any imported package has one, the root's own runtime initializers are `jog_<module>__0rootinit`, and `jog_<module>__0init` calls those functions in loader order. That dispatcher is `init_func`, so `jog_entry_init` runs a package after the packages it imports. A program whose runtime initializers are all in the root package keeps the single inlined init.
- `jog test` loads `*.test.jog`, calls each `@test` in source order, and does not call `fn main`. `// expect:` lines in the test files are the whole stdout. `@test` outside a test file is an error. A test with parameters or an extern is rejected. A test may declare a `with` clause; the harness passes top-level `context` globals of those exact types. `Testing` is not declared in std, so `@test` still has no Testing context.
- Public names are visible from any package. `private` and `private(package)` are visible in the declaring package. `private(file)` is not a package name, so another file reports it as undefined. `.ptr` and `.len` of `bytes`, `str`, and slices typecheck and lower. `as` between pointers, including a niche `*T?`, is a bitcast.
- A pointer-free top-level `val` is evaluated at compile time; a `val` whose type contains a pointer still runs at startup. See the comptime note. The packages slice left `Allocator` unchecked; the interfaces slice checks its methods. Interpolation, `go`, and comptime are not in the packages slice. `with` clauses and `context` bindings are checked and lowered; see the contexts note.

### contexts

- A context matches by exact type, or by conversion of a pointer to an interface that concrete type satisfies. The search is the explicit `with` on the call, then the innermost `context` binding or `with` block, then the function's own `with` parameters, then top-level `context` globals. Two matches at the same level are ambiguous. An exact match and a converting match at the same level are both matches.
- Context parameters lower to trailing JIR parameters, after the method receiver and the written parameters. Calls pass the expressions the checker stored. A `with` block evaluates each value once into a local. A top-level `context` lowers like `val`: `is_const`, and a folded integer or bool is a static const image with no store.
- A private function with a body, no written `with`, and no `@test`, `@export`, or generic clause infers its `with` clause. A top-level context already in scope is not inferred: the body resolves the call to that global. Public functions are the boundary and must write their clause. `fn main` cannot declare contexts. `ErrorAllocator`, `Scope`, and `Logger` are not declared in std, so the runtime defaults are not provided. An exported or extern function cannot declare contexts.
- `@test` may declare a `with` clause. The test harness passes visible top-level `context` globals of the same types. It does not build a `Testing` value.

### generics

- Step 1 monomorphizes unconstrained function type parameters. A type parameter is stored, copied, and passed; it has no zero value and no operations of its own. A call infers arguments from the value parameters, or takes explicit ones on the callee name (`id<i32>(x)`). An untyped integer stays soft: a later integer type wins, and if every candidate is an untyped integer the parameter becomes `i64`. Two uses that disagree are an error. A parameter that never appears in a value parameter cannot be inferred.
- Each concrete list is one function. The C name is `jog_<pkg>__<name>__<arg>` with another `__` between arguments. Spellings are `i32`, `bool`, `str`, `ptr_`, `cptr_`, `slice_`, `cslice_`, `arrN_`, `opt_`, and `tup_`. The jog name is `pkg.name<i32, bool>`. The template is not emitted. A call inside a generic body is instantiated when the outer instance is lowered.
- An extern function, an `@export` function, an `@test` function, and root `fn main` cannot declare type parameters. The parser already rejects the extern case; the checker repeats it. A default value whose type mentions a type parameter is rejected. A type parameter hides a same-named type inside that function.
- Step 2 monomorphizes unconstrained struct type parameters. `Box<i32>` is one nominal struct; the template is not laid out and not emitted. An argument list that still names a type parameter stays open until the outer function is monomorphized. A struct literal writes the arguments on the name (`Box<i32>(value: 1)`); they are not inferred. `Box<i32>` and `Box<bool>` are different types. A field default whose type mentions a type parameter is rejected. A type parameter hides a same-named type inside the struct. `*Node<T>` may appear in `struct Node`.
- The C name of a struct or enum instance stays the backend's `jog_<pkg>__<Name>__<type id>...`. Function mangling appends those arguments (`Box_i32`) so two instances do not share a C name. A method on a generic struct or enum, including `fn Box<i32>.get` and `fn Box<T>.get`, is rejected. The parser's receiver form does not bind `T`.
- Step 3 monomorphizes unconstrained enum type parameters. `Result<i32>` is one nominal enum; the template is not laid out and not emitted. A variant writes the arguments on the name (`Result<i32>.Ok(value: 1)`); they are not inferred. `Result<i32>` and `Result<bool>` are different types. A payload default whose type mentions a type parameter is rejected. `*Node<T>` may appear in a payload of `enum Node`. An enum aggregate lowers as `JIR_AGGREGATE` with the variant index in `aux`, and the C backend writes the tag and payload.
- Step 4 monomorphizes unconstrained alias type parameters. `type Pair<A, B> = (A, B)` is a name for its target: `Pair<i32, bool>` is the tuple `(i32, bool)`, not a new nominal type, so `Pair<i32, bool>` and `(i32, bool)` are the same type. The template is not a value and is not emitted. Arguments are written on the name and are not inferred. A use whose arguments still name a type parameter stays that open target until the outer generic is monomorphized. A generic alias that names itself is a declaration cycle; it does not become a recursive nominal type. Diagnostics name the target type. `type A<T>: B` is rejected. Constraints, generic interfaces, and generic methods remain.
- Step 5 monomorphizes unconstrained methods. `fn List<T>.get` and `fn *List<T>.push` declare `T` for every instance; the names are binders, in order, and they do not have to match the struct or enum's parameter names. `fn List<T>.map<R>` adds `R`. A call infers `R` from the value arguments, or writes `obj.method<R>(x)`. Receiver parameters are not written at the call. Each concrete receiver plus the method's own type arguments is one function, mangled `jog_<pkg>__<Type>__<method>__<arg>...`. The template is not emitted. An unconstrained type parameter still has no operations. `fn List<i32>.get` binds a type parameter named `i32` for every `List`; it does not capture `List<i32>` alone. A method on a generic alias is rejected. A method binder does not copy a constraint from the nominal type; the constraint is the one written on that binder.
- Step 6 checks constraints. A constraint is an interface. `Ordered`, `Comparable`, `Signed`, `Unsigned`, `Integer`, `Float`, `Number`, and `Ordering` live in `import std.cmp`. Inside the body, `T` gets the operations every member of the constraint shares: the intersection of a type set's builtin operations, plus methods and properties the interface requires. An unconstrained `T` still has none. A type-set interface is not a value type. `~T` is the underlying type with every distinct layer removed, so it matches `T` and every distinct type built on it. A one-term embed of a non-interface (`interface Exact { i32 }`) is an exact type set. `|` of type-set interfaces unions those sets. Embedding a method interface merges its requirements. In an operator requirement the constraint name stands for the type argument: `fn op<=>(o: Ordered): Ordering` is satisfied by `fn T.op<=>(o: T): Ordering` and by the integer, bool, and str intrinsics. `<` `<=` `>` `>=` derive from `op<=>`, and `!=` derives from `op==`. A property on a constraint is a direct field access after specialization. `var` is not satisfied by `val`. `Number` includes `Float`; a use of `f32` or `f64` as a value is still rejected. An open interface entails another open interface when it lists each required method and property. A type-set constraint entails another when every alternative satisfies it, so `Signed` entails `Integer` and `Ordered`. It does not entail a different open interface that asks for a method the alternatives lack. Interface method requirements still cannot have type parameters. Generic interfaces stay rejected.

### coroutines

- The scheduler is single-threaded and cooperative. `go` only enqueues. A channel operation, `sleep`, `yield`, `select`, or scope join parks the running coroutine. Stacks are reserved with one guard page; V1 commits the usable range up front (256 KiB when the launch omits `stack:`). A fault in a recorded guard page panics with "stack overflow". That handler is best-effort, not commit-on-touch. Stacks are not freed until the process exits.
- `Chan<T>` and `Task<T>` are distinct `*void` values. `Scope` is one distinct `*void`. The C driver links `jogrt.c`, `jogrt_sched.c`, `jogrt_switch.c`, and `jogrt_main.c`. `jogrt_switch.c` is GNU C, the same exception as `tools/coro_switch.c`.
- A `go` body is a `void(void *)` function. Captured locals are addresses in a heap array. Plain assignment stores through that address. Compound assignment of a capture is not lowered. Nested `go` copies the pointer it already captured.
- `select` of only closed sends returns `ErrClosed`. A closed `recv` runs its body with the zero value and a null error. Cancellation returns `ErrCancelled`. At most 32 cases.

### comptime

- The interpreter runs JIR in a byte arena. Pointers are offsets and offset 0 is null. Each `jir_interp_call` has its own memory. The step budget counts instructions and terminators. A panic or a budget overrun is a compile error plus a note per `jog_name` on the stack.
- A top-level `val` whose type contains no pointer (integers, bools, and arrays of those) is evaluated before other function bodies and stored as const global data. Folded integer and bool literals stay on the fold path. `var`, `context`, and a `val` whose type contains a pointer (`str`, slices, pointers, functions, interfaces) still run in `init_func`. Struct, enum, and tuple globals are still rejected by the existing global gap.
- `@comptime(expr)` typechecks as `expr`. Lowering isolates it, interprets it, and materializes an integer or bool as `JIR_OPND_INT`. Any other pointer-free value becomes a const global load. A result that contains a pointer is an error. `@embed("path")` is one string literal. The path is relative to the source file, `.` and `..` collapse lexically (symlinks are not followed), and the file must stay inside the module root. When a program has no module root, the source file's directory is that boundary. The result type is `const bytes`, the same type as `str`. Lowering copies the bytes into a `JIR_OPND_STRING` local. A missing file or a directory is an error. `.len` of an embed is pointer-free and can run inside `@comptime`. `--comptime-steps` is not a driver flag; lowering passes 10000000. Floats, indirect calls, and extern calls are rejected when executed. `go` and channels lower to extern runtime calls, so executing them at compile time fails.
- The CRC example writes `for i in 0..256`. `i` is `i64` and an index must be `usize`. There is no implicit conversion, so the program that computes the table writes `0 as usize..256`.
- A callee can read a pointerful `val` that is not ready yet and see zeros. A direct use of that `val` from the initializer is an error. Thunks are emitted as static functions and are not stored. Same IR through the interpreter and through C is checked for arithmetic, a loop, an array store, and a const global load.

### wasm

- Freestanding, not WASI. `JogCcRequest` is unchanged: an output path ending in `.wasm` selects the wasm32 clang link. The driver uses that suffix only for `--target wasm32`.
- Pointer size is 4 on that target. Native builds stay 8. `usize` in the type table follows that size; generated integer types that are `intptr_t` still take their width from clang's wasm32 ABI.
- The native `jogrt.c`, `jogrt_sched.c`, and `jogrt_switch.c` are not compiled into the wasm module. `runtime/wasm/jogrt_wasm.c` implements `jogrt.h` with a static bump allocator. `jogrt_free` does not reuse blocks. `FILE*` shims return null. `errno` is 0.
- Node is the host (`runtime/wasm/host.js`). Arguments after `--` are rejected. `JOG_CC`, when set, must name clang.

## See also

- [[plans/V1Compiler]]
- JogDocs: design/language/Grammar.md
- JogDocs: design/Compiler.md
- JogDocs: design/Runtime.md
