C tests and end-to-end Jog programs. The check command runs them.

- On Windows, `pwsh scripts/check.ps1` builds into `build/` and runs `ctest --test-dir build`.
- On Linux, `scripts/check.sh` builds into `build-linux/` and runs ctest there. `CC=gcc` uses `build-linux-gcc/`, and `CC=clang` uses `build-linux-clang/`.

The C tests cover `tools/`, the compiler, and the runtime. `tests/jog/` holds end-to-end programs with `// expect:` comments. See `docs/plans/V1Compiler.md`.

`parser_test` also parses every Jog code block in the language notes, `design/language/*.md` in [JogDocs](https://github.com/Jog-lang/JogDocs). `tests/parse/extract-notes.ps1` looks for a local checkout in `$JOG_DOCS`, then `../JogDocs` next to this repo, then `../Jog/JogDocs`. When none exists, that test prints `skip: language notes test: ...` and the rest of `parser_test` still runs. A missing drive letter is treated as "not found", so the test skips instead of failing.
