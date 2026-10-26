#!/usr/bin/env bash
# Install the Linux build tools and print their versions. x86-64 only.
# aarch64 is not supported: runtime/jogrt_switch.c rejects it with #error.
set -euo pipefail

arch=$(uname -m)
if [[ "$arch" != "x86_64" ]]; then
    echo "setup-linux: x86-64 is required; uname -m is ${arch}. aarch64 is not supported." >&2
    exit 1
fi

if [[ "$(id -u)" -eq 0 ]]; then
    apt-get update
    apt-get install -y build-essential clang cmake ninja-build git
else
    sudo apt-get update
    sudo apt-get install -y build-essential clang cmake ninja-build git
fi

echo "gcc: $(gcc --version | head -n 1)"
echo "clang: $(clang --version | head -n 1)"
echo "cmake: $(cmake --version | head -n 1)"
echo "ninja: $(ninja --version)"
echo "git: $(git --version)"
echo "uname: $(uname -srm)"
