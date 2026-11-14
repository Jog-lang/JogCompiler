# Agents (develop / v2 Zig)

## Jog knowledge

- Spec layers (substrate → prelude `core` → importable `std`): JogDocs `design/language/Types.md` and `design/v2/README.md`. Start at JogDocs `AGENTS.md` and `guide/JogQuickRef.md`.
  Fedora `~/Documents/Projects/Jog/JogDocs` (or `/home/plague_doc/Documents/Projects/Jog/JogDocs`); Windows `D:/Projects/Jog/JogDocs`.
- Agent skills canon: JogDocs `.agents/skills/` (pointer: `.agents/skills/README.md` here). See JogDocs `guide/UsingJogDocs.md`.
- Log friction in JogDocs `bootstrap/Friction.md`.

## This branch (`develop` = Zig V2 product)

- After the branch cut, **`develop`** is the Zig native compiler tip. Historical Jog self-host is **`develop-jogc`**. Branch **`v2`** may remain as an alias; prefer `develop` for new work. Do not merge `seed` or `develop-jogc` into this line.
- Host: Zig 0.14+. Gate: `zig build` and `zig build test`; `jog build examples/hello.jog` must produce a runnable ELF with no C step.
- Layout: `src/lex`, `src/parse`, `src/ast`, `src/check`, `src/ir`, `src/codegen`, `src/driver`, `src/capi`; shipped **`core/`** and **`std/`**.
- Libraries / FFI (JOG-97): `zig build` also installs `libjogc_abi.so` (`jogc_abi_version`). Zig tools should prefer future Zig modules (`jogc_parse` / `jogc_check`); Jog `@link("jogc_abi")` is for the C façade smoke (`examples/abi_smoke.jog`). Design note lives in the project store `docs/jog-zig-libs-ffi.md` — not a JogDocs rewrite.
- Discovery: `jog roots` — `$JOG_ROOT/{core,std}` or next to the binary (`<jog>/../{core,std}`), else cwd. Prefer loading the Jog prelude over growing `defineBuiltin`; keep thin Zig builtins only where IR/runtime requires.
- Ivan writes Jog by hand unless he asked for implementation (std/core port and Zig wiring are assigned when Phase A says so). Prefer explain/review over large rewrites.
