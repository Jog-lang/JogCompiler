# Extract Jog code blocks from the language notes in JogDocs
# (https://github.com/Jog-lang/JogDocs), design/language/*.md.
# JogDocs is found through $env:JOG_DOCS, then ../JogDocs next to this
# repo, then ../Jog/JogDocs. When none exists, the output is one
# @@@MISSING line and the caller skips the test. A missing drive letter
# is treated as "not found" so the caller still skips.
# Fences tagged ebnf are not Jog. Blocks that use the quote keyword are
# post-V1 and are reported as skips. Other fences are written raw.
param(
    [Parameter(Mandatory = $true)]
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dir = Split-Path -Parent $OutFile
if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Path $dir | Out-Null
}
$utf8 = New-Object System.Text.UTF8Encoding $false

function Test-Container([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $false }
    try {
        return Test-Path -LiteralPath $path -PathType Container -ErrorAction Stop
    } catch {
        return $false
    }
}

$candidates = @()
if ($env:JOG_DOCS) { $candidates += $env:JOG_DOCS }
$parent = Split-Path -Parent $root
if ($parent) {
    $candidates += [System.IO.Path]::Combine($parent, 'JogDocs')
    $candidates += [System.IO.Path]::Combine($parent, 'Jog', 'JogDocs')
}
$docs = $null
foreach ($c in $candidates) {
    $lang = [System.IO.Path]::Combine($c, 'design', 'language')
    if (Test-Container $lang) {
        $docs = $lang
        break
    }
}
if (-not $docs) {
    $searched = $candidates -join '; '
    [System.IO.File]::WriteAllText($OutFile, "@@@MISSING JogDocs not found (searched: $searched); set JOG_DOCS`n", $utf8)
    exit 0
}

$sb = New-Object System.Text.StringBuilder
$notes = 0
$skips = 0
Get-ChildItem -LiteralPath $docs -Filter '*.md' | Sort-Object Name | ForEach-Object {
    $name = $_.Name
    $text = [System.IO.File]::ReadAllText($_.FullName)
    $regex = [regex]::new('(?ms)^```([^\r\n]*)\r?\n(.*?)```')
    foreach ($m in $regex.Matches($text)) {
        $tag = $m.Groups[1].Value.Trim()
        if ($tag -eq 'ebnf') { continue }
        $body = $m.Groups[2].Value
        $before = $text.Substring(0, $m.Index)
        $line = ([regex]::Matches($before, "`n")).Count + 1
        if ($body -cmatch '(^|[^A-Za-z0-9_])quote([^A-Za-z0-9_]|$)') {
            [void]$sb.AppendLine("@@@SKIP quote ${name}:${line}")
            $skips++
            continue
        }
        [void]$sb.AppendLine("@@@NOTE ${name}:${line}")
        [void]$sb.Append($body)
        if (-not $body.EndsWith("`n")) { [void]$sb.Append("`n") }
        [void]$sb.AppendLine("@@@END")
        $notes++
    }
}
[void]$sb.AppendLine("@@@COUNT notes=$notes skips=$skips")
[System.IO.File]::WriteAllText($OutFile, $sb.ToString(), $utf8)
