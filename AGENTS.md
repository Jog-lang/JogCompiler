# Agents (v2)

- Branch `v2` is the Zig native compiler. Do not merge `develop` or `seed` into it.
- Language direction: JogDocs `design/v2/`.
- Host: Zig 0.14+. Gate: `zig build` and `zig build test`; `jog build examples/hello.jog` must produce a runnable ELF with no C step.
- Layout: `src/lex`, `src/parse`, `src/ast`, `src/check`, `src/ir`, `src/codegen`, `src/driver`.
