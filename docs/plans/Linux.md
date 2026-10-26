---
status: active
tags:
  - plan
  - linux
---

# Linux

x86-64 Linux with glibc, built with gcc or clang. aarch64 is not supported: `runtime/jogrt_switch.c` keeps its `#error` for any target that is not x86-64. Windows x86-64 (MinGW gcc or clang) stays the other supported host. The check on Windows is `pwsh scripts/check.ps1`.

## Setup

On Ubuntu, from the repo root:

```bash
scripts/setup-linux.sh
```

That runs `apt-get install` of `build-essential`, `clang`, `cmake`, `ninja-build`, and `git`, refuses anything other than x86-64, and prints the tool versions. It uses `sudo` when it is not already root.

From Windows, Ubuntu in WSL has to be installed by hand. This machine does not have it (`wsl -l` lists only `docker-desktop`). Do not install it from a script:

```powershell
wsl --install -d Ubuntu
```

That needs an administrator and may reboot. `scripts/check-wsl.ps1` exits non-zero and prints that command when the `Ubuntu` distro is missing.

## Check

Inside Linux, from the repo root:

```bash
scripts/check.sh              # build-linux/, default CC
CC=gcc scripts/check.sh       # build-linux-gcc/
CC=clang scripts/check.sh     # build-linux-clang/
scripts/check.sh --both       # gcc, then clang
```

Each run configures with Ninja, builds, fails if the log contains `warning:`, runs `ctest --output-on-failure`, syntax-checks the C sources with `clang -fsyntax-only -Werror` (the same flags and exclusions as `scripts/check.ps1`: not `coro_switch.c`, not `jogrt_switch.c`), and prints `check: OK`.

From Windows, after Ubuntu is installed:

```powershell
pwsh scripts/check-wsl.ps1
```

That is `scripts/check.sh --both` in WSL. The Linux build directories stay out of `build/`.

## Known gaps

- **aarch64.** The context switch is x86-64 only. There is no port.
- **`/proc/self/exe`.** On Linux the compiler finds its own executable with `readlink("/proc/self/exe")` and looks for `std` next to that path. A system without `/proc` mounted cannot locate the standard library that way. macOS uses `_NSGetExecutablePath` instead.

## Linux works

Run these after `scripts/setup-linux.sh`. A failure that cannot be fixed here goes in JogDocs `bootstrap/Friction.md`.

- [ ] `scripts/check.sh --both` prints `check: OK` for gcc and for clang.
- [ ] `jog run examples/hello` prints the hello line.
- [ ] `jog test std/os`, `jog test std/fs`, `jog test std/proc`, and `jog test` for each other package under `std/` pass.
- [ ] The playground builds (`make -C playground`).
- [ ] If `wasm-ld` is on `PATH`, `jog build --target wasm32 examples/hello` produces a `.wasm`.
