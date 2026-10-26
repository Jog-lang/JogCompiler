The V1 Jog compiler, `jog`, written in C17. It checks a module, lowers it to one intermediate representation, generates C, and builds that C with gcc or clang. Assembly does not live in this directory.

Supported hosts are Windows x86-64 (MinGW gcc or clang) and Linux x86-64 (gcc or clang). aarch64 is not supported.

The language specification is [JogDocs](https://github.com/Jog-lang/JogDocs). Start at its `AGENTS.md` and `guide/JogQuickRef.md`. A local checkout sits beside the repository root, at `../JogDocs` or `../Jog/JogDocs`.

The architecture and milestones are in `docs/plans/V1Compiler.md`. Sources are in `src/`. The compiler uses the shared utilities in `tools/`.

From the repository root, `pwsh scripts/check.ps1` on Windows, or `scripts/check.sh` on Linux, prints `check: OK` when the build and tests pass. See the repository README for the build commands and `jog run examples/hello`.
