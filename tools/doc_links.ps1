# Annota - tools/doc_links.ps1 : verify that every relative link and image in the markdown
# documentation points at a file that exists.
#
#   powershell -ExecutionPolicy Bypass -File tools\doc_links.ps1
#
# Exits non-zero when a link is broken, so it can be part of CI.
param(
    [string]$Root = "",
    [string[]]$Pending = @("LICENSE")   # files the maintainer still has to add
)

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }

$files = Get-ChildItem -Path $Root -Filter *.md -Recurse -File |
    Where-Object { $_.FullName -notmatch "\\build\\" -and $_.FullName -notmatch "\\\.git\\" }

# Fenced code blocks are documentation of code, not links: `[](VM& vm, ValueList& a)` is a C++
# lambda, and a path inside a fence may well not exist.  Blank them out before matching.
function Remove-FencedCode([string]$text) {
    $lines = $text -split "`n"
    $out = New-Object System.Collections.Generic.List[string]
    $inFence = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*(```|~~~)') {
            $inFence = -not $inFence
            $out.Add("")
            continue
        }
        $out.Add($(if ($inFence) { "" } else { $line }))
    }
    return ($out -join "`n")
}

$broken = @()
$pendingHits = @()
foreach ($f in $files) {
    $text = Remove-FencedCode (Get-Content $f.FullName -Raw -Encoding UTF8)
    # the patterns are deliberately line-bound: a cross-line match would swallow prose (and
    # markdown emphasis) into the captured path and produce nonsense "illegal path" errors
    $matches = [regex]::Matches($text, '!?\[[^\]\r\n]*\]\(([^)\r\n]+)\)') +
               [regex]::Matches($text, '<img[^>]+src="([^"\r\n]+)"')
    foreach ($m in $matches) {
        $target = $m.Groups[1].Value.Trim()
        if ($target -match '^(https?:|mailto:|#)') { continue }
        $path = ($target -split '#')[0]
        if (-not $path) { continue }
        $rel = $path.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if ($Pending -contains $rel) { $pendingHits += "$($f.Name) -> $rel"; continue }
        # a relative link is resolved against the file that contains it (GitHub behaviour),
        # with the repository root accepted as a fallback
        $full = Join-Path $f.DirectoryName $rel
        if (-not (Test-Path $full)) { $full = Join-Path $Root $rel }
        if (-not (Test-Path $full)) {
            $broken += "$($f.Name): $target"
        }
    }
}

foreach ($b in $broken) { Write-Host "  BROKEN $b" -ForegroundColor Red }
if ($pendingHits.Count -gt 0) {
    Write-Host "  note: $($pendingHits.Count) link(s) point at files the maintainer still provides:"
    $pendingHits | Sort-Object -Unique | ForEach-Object { Write-Host "        $_" -ForegroundColor Yellow }
}
if ($broken.Count -gt 0) {
    Write-Host "doc links: $($broken.Count) broken link(s) in $($files.Count) markdown files" -ForegroundColor Red
    exit 1
}
Write-Host "doc links: all relative links in $($files.Count) markdown files resolve" -ForegroundColor Green
exit 0
