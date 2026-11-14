# Agents (v2)

## Jog knowledge

- Language direction: JogDocs `design/v2/`. Start at JogDocs `AGENTS.md` and `guide/JogQuickRef.md`.
  Fedora `~/Documents/Projects/Jog/JogDocs` (or `/home/plague_doc/Documents/Projects/Jog/JogDocs`); Windows `D:/Projects/Jog/JogDocs`.
- Agent skills canon: JogDocs `.agents/skills/` (pointer: `.agents/skills/README.md` here). See JogDocs `guide/UsingJogDocs.md`.
- Log friction in JogDocs `bootstrap/Friction.md`.

## This branch (`v2`)

- Branch `v2` is the Zig native compiler. Do not merge `develop` or `seed` into it.
- Host: Zig 0.14+. Gate: `zig build` and `zig build test`; `jog build examples/hello.jog` must produce a runnable ELF with no C step.
- Layout: `src/lex`, `src/parse`, `src/ast`, `src/check`, `src/ir`, `src/codegen`, `src/driver`.
- Ivan writes Jog by hand unless he asked for implementation. Prefer explain/review over large rewrites.
