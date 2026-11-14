# Agents

## Jog knowledge

- The Jog specification lives in [JogDocs](https://github.com/Jog-lang/JogDocs). Start at its `AGENTS.md` and `guide/JogQuickRef.md`.
  Local checkout: Fedora `~/Documents/Projects/Jog/JogDocs` (or `/home/plague_doc/Documents/Projects/Jog/JogDocs`, sibling `../JogDocs`); Windows `D:/Projects/Jog/JogDocs`.
- Agent skills canon: JogDocs `.agents/skills/` (pointer: `.agents/skills/README.md` here). See JogDocs `guide/UsingJogDocs.md`.
- Log friction in JogDocs `bootstrap/Friction.md`.

## This branch (`develop`)

- The V1 plans stay in this repo, in `docs/plans/`. Architecture: `docs/plans/V1Compiler.md`. Parallel work: `docs/plans/V1Parallel.md`.
- This branch is jogc only. There is no CMake seed tree and no `scripts/check.sh` here. Stage 0 is an external `jog` from the `seed` branch (set `JOG`, or put `jog` on `PATH`).
- Build the CLI from `cmd/`: `"$JOG" build -o ../out/jogc --module jogc=../.` (or `jog-build build -o ../out/jogc`). The root module is the `jogc` library. Judge a stage with `pwsh scripts/diff_stage.ps1 -Jog $env:JOG -Jogc out/jogc.exe` (pass `-Jog` explicitly).
- Do not implement a jogc stage unless asked. Do not merge this branch with `seed`.
- Ivan writes the Jog sources by hand. Prefer explaining and pointing at the spec over rewriting large amounts of Jog unless asked.

P3 landed on `p3/jogc` (seed + jogc together). Ongoing jogc work is here on `develop`. The live C seed is branch `seed`; `c-seed-compiler` is history only. Zig native compiler policy lives on branch `v2` (local tree often `../JogCompiler-v2`).
