# The merge gate for every stream: configure, build with gcc (no warnings
# allowed), run all tests, and syntax-check every C file with clang as well.
# Usage: pwsh scripts/check.ps1        (from anywhere in the repo)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($message) {
    Write-Host "check: FAILED - $message" -ForegroundColor Red
    exit 1
}

cmake -S . -B build -G Ninja | Out-Null
if ($LASTEXITCODE -ne 0) { Fail 'cmake configure' }

$buildLog = cmake --build build 2>&1
$buildLog | Write-Host
if ($LASTEXITCODE -ne 0) { Fail 'build' }
if ($buildLog | Select-String -Pattern 'warning:') { Fail 'gcc warnings (the build must be warning-free)' }

ctest --test-dir build --output-on-failure
if ($LASTEXITCODE -ne 0) { Fail 'tests' }

$sources = @(Get-ChildItem tools/*.c, compiler/src/*.c, tests/*.c, runtime/*.c -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'coro_switch.c' -and $_.Name -ne 'jogrt_switch.c' } | ForEach-Object { $_.FullName })
# CT_SOURCE_DIR only needs to be some string literal for a syntax check.
clang -std=c17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -Itools -Icompiler/src -Iruntime -DCT_SOURCE_DIR=__FILE__ $sources
if ($LASTEXITCODE -ne 0) { Fail 'clang -fsyntax-only' }

Write-Host 'check: OK' -ForegroundColor Green
