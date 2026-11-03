# Seed cleanup inventory

Branch: `seed` (cut from `p3/jogc` at `d147c63`, then stripped of jogc sources).
Gate for every cleanup commit: `bash scripts/check.sh` prints `check: OK` (21/21, wasm included). Cleanups are behavior-preserving: same diagnostics, same emitted C, same tests. Do not restyle files you are not editing. Do not change diagnostic text, IR dump shape, or language rules in a cleanup commit.

Lasting rules for what belongs on this branch live in JogDocs `bootstrap/Seed.md` (added with the vault pass). Dual-commit rule for shared `std/`: fix on `seed` and, when jogc needs the same change, a separate commit on `develop` (or a cherry-pick of a path that still exists on both). Never cherry-pick a commit that deletes the other tree’s sources.

## Scope walked

| Area | Count / notes |
|---|---|
| `compiler/src` | 104 files (`.c` / `.h`) |
| `runtime/` | `jogrt*.c/h`, `wasm/`, `asm/README.md` |
| `tools/` | `ct_util` sources + README |

## Prefixes (`AGENTS.md`)

| Prefix | Owner | Status |
|---|---|---|
| `jog_` | compiler (check, load, emit, …) | Public headers use `jog_*`. OK. |
| `jir_` | IR | Present in IR headers. OK. |
| `ct_` | `tools/` | Headers and symbols use `ct_*`. OK. |
| `jogrt_` | `runtime/` | Symbols use `jogrt_*`. OK. |

File-local `static` helpers in `.c` files are often unprefixed (`is_separator`, `push_arg`, …). That matches the rest of the tree. **Mass-renaming statics is deferred** — high churn, no behavior win, easy to break. If a new *exported* symbol lacks a prefix, fix that symbol in its own commit.

CMake still builds a static library target named `jogc` that holds the C compiler objects (everything except `main.c`). The name is historical (from the mixed tree). Renaming the target is optional cosmetic cleanup; it is not required for correctness.

## Dead files / unused headers

- No unused `.h` under `compiler/src` (every header is `#include`d from some `.c` or peer header).
- No `#if 0` dead blocks found in `compiler/`, `runtime/`, or `tools/`.
- No `TODO` / `FIXME` markers in those trees at inventory time.
- After the C-only strip, `scripts/diff_stage.ps1` remains but only judges jogc against an external `jog`. It belongs on `develop`, not on `seed`. **Remove on seed.**

## Duplicated helpers

No clear “two owners of the same helper” that is safe to collapse without a dedicated pass. `tools/` (`ct_*`) is the shared owner for arena/da/map/fs/proc; compiler code calls into it. Leave duplicates alone until a specific pair is named with a before/after test.

## Comments and docs that still assume a mixed tree

| Location | Issue | Action |
|---|---|---|
| `AGENTS.md` | Says P3 work is on `p3/jogc`, stage 0 is `bootstrap/stage0/jog.exe`, and `c-seed-compiler` is the live reference. | Rewrite for `seed`: this branch is stage 0; jogc is on `develop`; `c-seed-compiler` is history. |
| `compiler/README.md` | “Sources are in `src/`” is ambiguous after the strip (`compiler/src` vs removed repo `src/`). | Say `compiler/src/`. |
| `tools/README.md` | Still accurate (`ct_util`, `util_test`). | Keep. |
| `docs/plans/V1Parallel.md` | Describes globbing into CMake target `jogc` — still true for this branch’s static lib name. | Keep; optional note that the executable is `jog`. |
| `README.md` | Already updated in the strip commit with the branch map. | Keep. |

## Themes for follow-up commits (checklist)

Use these as the Seed cleanup boxes in JogDocs Checks later.

- [x] Inventory committed (`docs/plans/SeedCleanup.md`).
- [x] Agent / compiler README wording matches the C-only `seed` branch.
- [x] Remove jogc-only scripts left on `seed` (`scripts/diff_stage.ps1`).
- [ ] (Optional, later) Rename CMake static library target `jogc` → a seed-neutral name; update any docs that mention the target.
- [ ] (Deferred) Prefix pass for newly exported symbols only; no mass rename of `static` helpers.
- [ ] (Deferred) Named duplicate-helper collapses, one pair per commit, with `check: OK`.

## Out of scope for cleanup commits

- Language, IR dump format, or diagnostic string changes.
- `std/` behavior changes that are not seed-only (if jogc needs them, dual-commit).
- Merging `seed` with `develop`.
- Restoring jogc sources onto this branch.
