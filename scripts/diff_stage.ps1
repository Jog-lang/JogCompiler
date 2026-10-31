# Compare stage 0 jog with jogc. JogDocs bootstrap/Checks.md, section P3.
#
# -Filter is optional. It is a PowerShell -like wildcard matched against
# the repo-relative path, with forward slashes. * and ? are wildcards,
# and * matches across '/'. A value with no wildcard is an exact path.
#   pwsh scripts/diff_stage.ps1 -Stage lex -Jogc out/jogc.exe -Filter examples/hello/main.jog
#   pwsh scripts/diff_stage.ps1 -Stage lex -Jogc out/jogc.exe -Filter examples/hello/*
#
# -Jog defaults to bootstrap/stage0/jog.exe, relative to the repository
# root (the parent of this script's directory).
#
# lex and parse compare stdout and the exit code on every .jog file under
# std/, tests/jog, tests/parse, tests/check, playground/, examples/, and
# src/, including the stage stubs. An exact -Filter path is also compared
# when that .jog file exists outside those trees (core/ is not scanned
# unless the filter names it). Line endings are normalized to LF.
# check compares the first diagnostic's line and column on each tests/check
# case (a directory that has jog.mod, or a loose .jog file staged into a
# temporary module). ir runs emit-ir on each top-level tests/jog program
# (not *.test.jog). ir_dump.c already numbers functions, blocks, locals,
# and globals (fN, bN, lN, gN), so this script compares that text raw after
# the newline normalization. Rewriting those indices would hide real
# differences. run builds each of those programs with both compilers and
# compares the program's stdout and exit code. A compiler invocation is
# killed after 3 minutes, and a built program after 1 minute.
#
# Outputs go under out/diff/<stage>/. The process exits non-zero on any
# mismatch, and exits 2 when the filter matches nothing or a compiler is
# missing.

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('lex', 'parse', 'check', 'ir', 'run')]
    [string]$Stage,

    [string]$Jog,

    [Parameter(Mandatory = $true)]
    [string]$Jogc,

    [string]$Filter
)

$ErrorActionPreference = 'Continue'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$Repo = Split-Path -Parent $PSScriptRoot
if (-not $Jog) {
    $Jog = Join-Path $Repo 'bootstrap\stage0\jog.exe'
}
if (-not [System.IO.Path]::IsPathRooted($Jog)) {
    $Jog = Join-Path $Repo $Jog
}
if (-not [System.IO.Path]::IsPathRooted($Jogc)) {
    $Jogc = Join-Path $Repo $Jogc
}
$Jog = [System.IO.Path]::GetFullPath($Jog)
$Jogc = [System.IO.Path]::GetFullPath($Jogc)

$OutRoot = Join-Path $Repo "out\diff\$Stage"
if (Test-Path $OutRoot) {
    Remove-Item -Recurse -Force $OutRoot
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

function Fail-Setup([string]$message) {
    Write-Host $message
    exit 2
}

if (-not (Test-Path -LiteralPath $Jog)) {
    Fail-Setup "jog not found: $Jog"
}
if (-not (Test-Path -LiteralPath $Jogc)) {
    Fail-Setup "jogc not found: $Jogc"
}

function Get-RelPath([string]$full) {
    $root = $Repo.TrimEnd('\', '/')
    $path = [System.IO.Path]::GetFullPath($full)
    if ($path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        $rel = $path.Substring($root.Length).TrimStart('\', '/')
        return ($rel -replace '\\', '/')
    }
    return ($path -replace '\\', '/')
}

function Convert-Lf([string]$text) {
    if ($null -eq $text) {
        return ''
    }
    return (($text -replace "`r`n", "`n") -replace "`r", "`n")
}

function Clip([string]$text) {
    if ($null -eq $text) {
        return ''
    }
    $text = $text.TrimEnd("`r")
    if ($text.Length -gt 160) {
        return $text.Substring(0, 160) + '...'
    }
    return $text
}

function Split-Lines([string]$text) {
    if ($null -eq $text -or $text -eq '') {
        return @('')
    }
    return @($text.Split([char]10))
}

function Get-FirstDiff([string]$left, [string]$right) {
    $a = Split-Lines $left
    $b = Split-Lines $right
    $n = [Math]::Max($a.Count, $b.Count)
    for ($i = 0; $i -lt $n; $i++) {
        $la = if ($i -lt $a.Count) { $a[$i] } else { '<missing>' }
        $lb = if ($i -lt $b.Count) { $b[$i] } else { '<missing>' }
        if ($la -ne $lb) {
            return [pscustomobject]@{ Line = ($i + 1); Left = $la; Right = $lb }
        }
    }
    return $null
}

function Get-Diag([string]$stderr, [string]$stdout) {
    foreach ($src in @($stderr, $stdout)) {
        foreach ($line in (Split-Lines $src)) {
            $trimmed = $line.TrimEnd("`r")
            if ($trimmed -match ':(\d+):(\d+):') {
                return [pscustomobject]@{
                    Line   = [int]$Matches[1]
                    Column = [int]$Matches[2]
                    Text   = $trimmed
                }
            }
        }
    }
    return $null
}

function Save-Text([string]$path, [string]$text) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($path, $(if ($null -eq $text) { '' } else { $text }), $utf8)
}

function Out-Path([string]$rel, [string]$suffix) {
    $dest = Join-Path $OutRoot (($rel -replace '/', '\') + $suffix)
    $dir = Split-Path -Parent $dest
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    return $dest
}

function Invoke-Captured {
    param(
        [string]$Exe,
        [string[]]$ArgumentList,
        [string]$WorkDir,
        [int]$TimeoutMs,
        [bool]$SetRoot
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($arg in $ArgumentList) {
        [void]$psi.ArgumentList.Add($arg)
    }
    if ($SetRoot) {
        $psi.EnvironmentVariables['JOG_ROOT'] = $Repo
    }
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $finished = $proc.WaitForExit($TimeoutMs)
    if (-not $finished) {
        try { $proc.Kill($true) } catch { try { $proc.Kill() } catch { } }
        [void]$proc.WaitForExit()
    }
    $stdout = $outTask.GetAwaiter().GetResult()
    $stderr = $errTask.GetAwaiter().GetResult()
    $code = $proc.ExitCode
    if (-not $finished) {
        $code = 124
    }
    return [pscustomobject]@{ Stdout = $stdout; Stderr = $stderr; Code = $code }
}

function Test-Filter([string]$rel) {
    if (-not $Filter) {
        return $true
    }
    return ($rel -like $Filter)
}

function Get-JogUnder([string]$dir) {
    $found = New-Object System.Collections.Generic.List[string]
    $path = Join-Path $Repo ($dir -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path)) {
        return @()
    }
    Get-ChildItem -LiteralPath $path -Recurse -File -Filter *.jog | ForEach-Object {
        [void]$found.Add((Get-RelPath $_.FullName))
    }
    return @($found | Sort-Object)
}

function New-StagedModule([string]$sourceFull, [string]$workName, [string]$manifest, [string]$destName) {
    $work = Join-Path $OutRoot ('work\' + $workName)
    if (Test-Path -LiteralPath $work) {
        Remove-Item -Recurse -Force -LiteralPath $work
    }
    $mod = Join-Path $work 'mod'
    New-Item -ItemType Directory -Force -Path $mod | Out-Null
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText((Join-Path $mod 'jog.mod'), $manifest, $utf8)
    Copy-Item -LiteralPath $sourceFull -Destination (Join-Path $mod $destName)
    return $mod
}

$script:MismatchCount = 0
$script:FileCount = 0
$mismatchLog = Join-Path $OutRoot 'mismatches.txt'

function Add-Mismatch([string]$rel, [string]$detail) {
    $script:MismatchCount++
    $line = "mismatch ${rel}: $detail"
    Write-Host $line
    Add-Content -LiteralPath $mismatchLog -Value $line -Encoding utf8
}

function Compare-Streams([string]$rel, $left, $right) {
    $lOut = Convert-Lf $left.Stdout
    $rOut = Convert-Lf $right.Stdout
    Save-Text (Out-Path $rel '.jog.stdout') $lOut
    Save-Text (Out-Path $rel '.jogc.stdout') $rOut
    Save-Text (Out-Path $rel '.jog.stderr') (Convert-Lf $left.Stderr)
    Save-Text (Out-Path $rel '.jogc.stderr') (Convert-Lf $right.Stderr)
    Save-Text (Out-Path $rel '.exit') "jog $($left.Code)`njogc $($right.Code)`n"
    $diff = Get-FirstDiff $lOut $rOut
    if ($null -eq $diff -and $left.Code -eq $right.Code) {
        return
    }
    if ($null -ne $diff) {
        Add-Mismatch $rel ("jog exit $($left.Code): $(Clip $diff.Left) | jogc exit $($right.Code): $(Clip $diff.Right)")
    } else {
        Add-Mismatch $rel ("exit $($left.Code) vs $($right.Code); stdout matches")
    }
}

# lex / parse
if ($Stage -eq 'lex' -or $Stage -eq 'parse') {
    $rels = New-Object System.Collections.Generic.List[string]
    foreach ($dir in @('std', 'tests/jog', 'tests/parse', 'tests/check', 'playground', 'examples', 'src')) {
        foreach ($rel in @(Get-JogUnder $dir)) {
            if (-not $rel) {
                continue
            }
            if (Test-Filter $rel) {
                [void]$rels.Add($rel)
            }
        }
    }
    # -Filter can name a .jog file outside those trees, such as core/scalar.jog.
    # The default scan stays the same when -Filter is omitted.
    if ($Filter -and ($Filter -notmatch '[\*\?]')) {
        $named = Join-Path $Repo ($Filter -replace '/', '\')
        if ((Test-Path -LiteralPath $named -PathType Leaf) -and $named.EndsWith('.jog', [System.StringComparison]::OrdinalIgnoreCase)) {
            $rel = Get-RelPath $named
            if (-not $rels.Contains($rel)) {
                [void]$rels.Add($rel)
            }
        }
    }
    if ($rels.Count -eq 0) {
        Fail-Setup "$Stage`: filter matched no files"
    }
    foreach ($rel in $rels) {
        $script:FileCount++
        $full = Join-Path $Repo ($rel -replace '/', '\')
        try {
            $left = Invoke-Captured -Exe $Jog -ArgumentList @($Stage, $full) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            $right = Invoke-Captured -Exe $Jogc -ArgumentList @($Stage, $full) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            Compare-Streams $rel $left $right
        } catch {
            Add-Mismatch $rel $_.Exception.Message
        }
    }
}

# check: first diagnostic line and column
if ($Stage -eq 'check') {
    $cases = New-Object System.Collections.Generic.List[object]
    $checkRoot = Join-Path $Repo 'tests\check'
    if (Test-Path -LiteralPath $checkRoot) {
        Get-ChildItem -LiteralPath $checkRoot -File -Filter *.jog | ForEach-Object {
            $rel = Get-RelPath $_.FullName
            if (Test-Filter $rel) {
                [void]$cases.Add([pscustomobject]@{ Kind = 'file'; Rel = $rel; Full = $_.FullName })
            }
        }
        Get-ChildItem -LiteralPath $checkRoot -Directory | ForEach-Object {
            $mod = Join-Path $_.FullName 'jog.mod'
            if (Test-Path -LiteralPath $mod) {
                $rel = Get-RelPath $_.FullName
                if (Test-Filter $rel) {
                    [void]$cases.Add([pscustomobject]@{ Kind = 'dir'; Rel = $rel; Full = $_.FullName })
                }
            } else {
                Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Filter *.jog | ForEach-Object {
                    $rel = Get-RelPath $_.FullName
                    if (Test-Filter $rel) {
                        [void]$cases.Add([pscustomobject]@{ Kind = 'file'; Rel = $rel; Full = $_.FullName })
                    }
                }
            }
        }
    }
    if ($cases.Count -eq 0) {
        Fail-Setup 'check: filter matched no files'
    }
    $n = 0
    foreach ($case in $cases) {
        $script:FileCount++
        $n++
        $target = $case.Full
        $work = $null
        try {
            if ($case.Kind -eq 'file') {
                $work = New-StagedModule $case.Full ("check-{0:d4}" -f $n) "module diffcase`n" (Split-Path -Leaf $case.Full)
                $target = $work
            }
            $left = Invoke-Captured -Exe $Jog -ArgumentList @('check', $target) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            $right = Invoke-Captured -Exe $Jogc -ArgumentList @('check', $target) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            $lOut = Convert-Lf $left.Stdout
            $rOut = Convert-Lf $right.Stdout
            $lErr = Convert-Lf $left.Stderr
            $rErr = Convert-Lf $right.Stderr
            Save-Text (Out-Path $case.Rel '.jog.stdout') $lOut
            Save-Text (Out-Path $case.Rel '.jogc.stdout') $rOut
            Save-Text (Out-Path $case.Rel '.jog.stderr') $lErr
            Save-Text (Out-Path $case.Rel '.jogc.stderr') $rErr
            $ld = Get-Diag $lErr $lOut
            $rd = Get-Diag $rErr $rOut
            $same = $false
            if ($null -eq $ld -and $null -eq $rd) {
                $same = ($left.Code -eq $right.Code)
            } elseif ($null -ne $ld -and $null -ne $rd) {
                $same = ($ld.Line -eq $rd.Line -and $ld.Column -eq $rd.Column)
            }
            if (-not $same) {
                $lt = if ($ld) { $ld.Text } else { "<no diagnostic> exit $($left.Code)" }
                $rt = if ($rd) { $rd.Text } else { "<no diagnostic> exit $($right.Code)" }
                Add-Mismatch $case.Rel ("jog: $(Clip $lt) | jogc: $(Clip $rt)")
            }
        } catch {
            Add-Mismatch $case.Rel $_.Exception.Message
        } finally {
            if ($work) {
                $parent = Split-Path -Parent $work
                if (Test-Path -LiteralPath $parent) {
                    Remove-Item -Recurse -Force -LiteralPath $parent
                }
            }
        }
    }
}

function Get-Programs {
    $found = New-Object System.Collections.Generic.List[string]
    $dir = Join-Path $Repo 'tests\jog'
    if (-not (Test-Path -LiteralPath $dir)) {
        return @()
    }
    Get-ChildItem -LiteralPath $dir -File -Filter *.jog | ForEach-Object {
        if ($_.Name.EndsWith('.test.jog')) {
            return
        }
        $rel = Get-RelPath $_.FullName
        if (Test-Filter $rel) {
            [void]$found.Add($rel)
        }
    }
    return @($found | Sort-Object)
}

# ir: raw emit-ir, no temporary-name rewrite. See the header comment.
if ($Stage -eq 'ir') {
    $programs = @(Get-Programs)
    if ($programs.Count -eq 0) {
        Fail-Setup 'ir: filter matched no files'
    }
    $n = 0
    foreach ($rel in $programs) {
        $script:FileCount++
        $n++
        $full = Join-Path $Repo ($rel -replace '/', '\')
        $work = $null
        try {
            $work = New-StagedModule $full ("ir-{0:d4}" -f $n) "module e2e`n" 'main.jog'
            $left = Invoke-Captured -Exe $Jog -ArgumentList @('emit-ir', $work) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            $right = Invoke-Captured -Exe $Jogc -ArgumentList @('emit-ir', $work) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            Compare-Streams $rel $left $right
        } catch {
            Add-Mismatch $rel $_.Exception.Message
        } finally {
            if ($work) {
                $parent = Split-Path -Parent $work
                if (Test-Path -LiteralPath $parent) {
                    Remove-Item -Recurse -Force -LiteralPath $parent
                }
            }
        }
    }
}

# run: build, then compare the program's stdout and exit code.
if ($Stage -eq 'run') {
    $programs = @(Get-Programs)
    if ($programs.Count -eq 0) {
        Fail-Setup 'run: filter matched no files'
    }
    $bin = Join-Path $OutRoot 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    $n = 0
    foreach ($rel in $programs) {
        $script:FileCount++
        $n++
        $full = Join-Path $Repo ($rel -replace '/', '\')
        $work = $null
        try {
            $work = New-StagedModule $full ("run-{0:d4}" -f $n) "module e2e`n" 'main.jog'
            $safe = ($rel -replace '[\\/:]', '_')
            $exeL = Join-Path $bin "$safe.jog.exe"
            $exeR = Join-Path $bin "$safe.jogc.exe"
            $builtL = Invoke-Captured -Exe $Jog -ArgumentList @('build', $work, '-o', $exeL) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            $builtR = Invoke-Captured -Exe $Jogc -ArgumentList @('build', $work, '-o', $exeR) -WorkDir $Repo -TimeoutMs 180000 -SetRoot $true
            Save-Text (Out-Path $rel '.jog.build.stderr') (Convert-Lf $builtL.Stderr)
            Save-Text (Out-Path $rel '.jogc.build.stderr') (Convert-Lf $builtR.Stderr)
            if ($builtL.Code -eq 0 -and (Test-Path -LiteralPath $exeL)) {
                $left = Invoke-Captured -Exe $exeL -ArgumentList @() -WorkDir $work -TimeoutMs 60000 -SetRoot $false
            } else {
                $left = [pscustomobject]@{ Stdout = ''; Stderr = $builtL.Stderr; Code = $builtL.Code }
            }
            if ($builtR.Code -eq 0 -and (Test-Path -LiteralPath $exeR)) {
                $right = Invoke-Captured -Exe $exeR -ArgumentList @() -WorkDir $work -TimeoutMs 60000 -SetRoot $false
            } else {
                $right = [pscustomobject]@{ Stdout = ''; Stderr = $builtR.Stderr; Code = $builtR.Code }
            }
            Compare-Streams $rel $left $right
        } catch {
            Add-Mismatch $rel $_.Exception.Message
        } finally {
            if ($work) {
                $parent = Split-Path -Parent $work
                if (Test-Path -LiteralPath $parent) {
                    Remove-Item -Recurse -Force -LiteralPath $parent
                }
            }
        }
    }
}

$summary = "$Stage`: $script:FileCount files, $script:MismatchCount mismatches"
Save-Text (Join-Path $OutRoot 'summary.txt') ($summary + "`n")
Write-Host $summary
if ($script:MismatchCount -gt 0) {
    exit 1
}
exit 0
