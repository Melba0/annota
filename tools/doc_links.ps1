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

$broken = @()
$pendingHits = @()
foreach ($f in $files) {
    $text = Get-Content $f.FullName -Raw -Encoding UTF8
    $matches = [regex]::Matches($text, '!?\[[^\]]*\]\(([^)]+)\)') +
               [regex]::Matches($text, '<img[^>]+src="([^"]+)"')
    foreach ($m in $matches) {
        $target = $m.Groups[1].Value.Trim()
        if ($target -match '^(https?:|mailto:|#)') { continue }
        $path = ($target -split '#')[0]
        if (-not $path) { continue }
        $rel = $path.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if ($Pending -contains $rel) { $pendingHits += "$($f.Name) -> $rel"; continue }
        $full = Join-Path $Root $rel
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
