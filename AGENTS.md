# Agents

- The Jog specification lives in the [JogDocs](https://github.com/Jog-lang/JogDocs) repository. Start at its `AGENTS.md` and `guide/JogQuickRef.md`. The language notes there (`design/language/`) are the specification. A local checkout is the sibling folder `../JogDocs`, or `../Jog/JogDocs` when the notes sit next to a `Jog` checkout.
- The V1 plans stay in this repo, in `docs/plans/`. The V1 compiler architecture is `docs/plans/V1Compiler.md`. Parallel work is organized by `docs/plans/V1Parallel.md`: find your stream there, edit only the files it owns, and change contract headers only through the process it describes.
- Log friction (anything in the language, compiler, std, or tooling that got in the way) in JogDocs `bootstrap/Friction.md`.
- This branch (`develop`) is jogc only. There is no CMake seed tree and no `scripts/check.sh` here. Stage 0 is an external `jog` from the `seed` branch (set `JOG`, or put `jog` on `PATH`).
- Build the CLI from `cmd/`: `"$JOG" build -o ../out/jogc --module jogc=../.` (or `jog-build build -o ../out/jogc`). The root module is the `jogc` library. Judge a stage with `pwsh scripts/diff_stage.ps1 -Jog $env:JOG -Jogc out/jogc.exe` (pass `-Jog` explicitly).
- Do not implement a jogc stage unless asked. Do not merge this branch with `seed`.
- Ivan writes the Jog sources by hand. Prefer explaining and pointing at the spec over rewriting large amounts of Jog unless asked.

P3 landed on `p3/jogc` (seed + jogc together). Ongoing jogc work is here on `develop`. The live C seed is branch `seed`; `c-seed-compiler` is history only.
