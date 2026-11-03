# Jog

Jog is a low-level language. It works with functions, data whose layout is explicit, and pointers. Memory is managed by hand. On top of that it has coroutines and a few conveniences in the style of Go. Source files use `.jog`.

The specification is [JogDocs](https://github.com/Jog-lang/JogDocs). Start at its `AGENTS.md` and `guide/JogQuickRef.md`.

## Branches

| Branch | Contents |
|---|---|
| **`develop` (this branch)** | The Jog compiler `jogc` only. No C seed sources. |
| **`seed`** | Stage 0: the C compiler `jog` only. Build that binary, then point `JOG` at it. |
| **`p3/jogc`** | Bootstrap record: both trees together at the self-host landing (`d147c63`). |
| **`c-seed-compiler`** | Older public C seed from before jogc. History only. |
| **`main`** | Tiny initial commit until the default branch moves. |

Do not merge `seed` and `develop` into each other. A fix that belongs in both (for example a shared `std/` bug) is two commits, one per branch, or a cherry-pick of a path that still exists on both.

## Library and CLI

The root module **`jogc`** is a **library**. It exports the compiler packages. The CLI driver is the nested module **`jogc_cmd`** under `cmd/`.

| | |
|---|---|
| Module | `jogc` |
| Require | `require jogc <path-to-this-checkout>` |
| Frontend imports | `import jogc.src.lex`, `jogc.src.parse`, `jogc.src.load`, `jogc.src.check`, and/or `jogc.src.frontend` |
| Do **not** import from tools | `jogc.src.emit`, `jogc.src.lower`, `jogc.src.ir`, or the `cmd/` driver |

`jogc.src.frontend.run(start, module_flags)` loads and typechecks only (no lowering or C emit).

## Build jogc

Stage 0 is an **external** `jog` binary. This checkout has no `compiler/` or CMake tree.

```
export JOG=/path/to/seed/build-linux/jog   # or a backup such as ../bin-backup/d147c63/jog
cd cmd
"$JOG" build -o ../out/jogc --module jogc=../.
# or: jog-build build -o ../out/jogc
../out/jogc version
```

On Windows the seed binary is `jog.exe` and the output is typically `../out/jogc.exe`.

Compare stages against the seed with:

```
pwsh scripts/diff_stage.ps1 -Stage check -Jog $env:JOG -Jogc out/jogc.exe
```

Pass `-Jog` explicitly (or put the seed on `PATH`). Do not look for `build/jog` or `build-linux/jog` inside this tree.

### Frontend smoke (no emit)

```
export JOG=/path/to/jog
cd examples/frontend_smoke
"$JOG" build -o out/smoke --module jogc=../..
./out/smoke ok
# prints: frontend_smoke: check ok
```

## Layout

- `jog.mod`, `src/` — `jogc` library (frontend + lower/emit)
- `cmd/` — `jogc_cmd` executable driver (`jogc` binary)
- `examples/frontend_smoke/` — require + `frontend.run` proof
- `core/`, `std/` — prelude and standard library (shared with `seed`)
- `runtime/` — C runtime linked into programs jogc builds
- `tests/jog`, `tests/check`, `tests/parse`, … — fixtures for stage diffs
- `scripts/diff_stage.ps1` — compare jogc to an external `jog`
- `bootstrap/` — notes for the frozen stage-0 binary used on Windows bootstrap
- `docs/plans/` — V1 process plans (masters for the parallel streams)

## License

This tree is licensed under GPL-3.0. See [LICENSE](LICENSE).
