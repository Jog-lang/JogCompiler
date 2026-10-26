# Run scripts/check.sh --both inside WSL Ubuntu. The Linux build directories
# are build-linux-gcc/ and build-linux-clang/, not build/. This script does
# not install a distro.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Fail($message) {
    Write-Host "check-wsl: FAILED - $message" -ForegroundColor Red
    exit 1
}

function Get-WslNames {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        return @()
    }
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = 'wsl.exe'
    $start.Arguments = '-l -q'
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.StandardOutputEncoding = [System.Text.Encoding]::Unicode
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $start
    if (-not $proc.Start()) {
        return @()
    }
    $text = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit()
    $names = @()
    foreach ($line in ($text -split "`r`n|`n|`r")) {
        $name = ($line -replace [char]0, '').Trim()
        if ($name.Length -gt 0) {
            $names += $name
        }
    }
    return $names
}

$names = @(Get-WslNames)
$hasUbuntu = $false
foreach ($name in $names) {
    if ($name -eq 'Ubuntu') {
        $hasUbuntu = $true
    }
}
if (-not $hasUbuntu) {
    Write-Host 'check-wsl: Ubuntu is not installed. Run: wsl --install -d Ubuntu'
    exit 1
}

if ($root.Length -lt 3 -or $root[1] -ne ':') {
    Fail "cannot map $root into WSL"
}
$drive = $root.Substring(0, 1).ToLower()
$rest = $root.Substring(2) -replace '\\', '/'
$wslPath = "/mnt/$drive$rest"
& wsl.exe -d Ubuntu -- bash -lc "cd '$wslPath' && scripts/check.sh --both"
if ($LASTEXITCODE -ne 0) {
    Fail 'scripts/check.sh --both'
}
