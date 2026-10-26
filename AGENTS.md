# Agents

- The Jog specification lives in the [JogDocs](https://github.com/Jog-lang/JogDocs) repository. Start at its `AGENTS.md` and `guide/JogQuickRef.md`. The language notes there (`design/language/`) are the specification. A local checkout is the sibling folder `../JogDocs`, or `../Jog/JogDocs` when the notes sit next to a `Jog` checkout.
- The V1 plans stay in this repo, in `docs/plans/`. The V1 compiler architecture is `docs/plans/V1Compiler.md`. Parallel work is organized by `docs/plans/V1Parallel.md`: find your stream there, edit only the files it owns, and change contract headers only through the process it describes.
- Log friction (anything in the language, compiler, std, or tooling that got in the way) in JogDocs `bootstrap/Friction.md`.
- A change is done only when the check command prints `check: OK`. On Windows that is `pwsh scripts/check.ps1`. On Linux that is `scripts/check.sh`.
- C17, warning-free on gcc and clang. Prefixes: `ct_` tools, `jog_` compiler, `jir_` IR, `jogrt_` runtime.
