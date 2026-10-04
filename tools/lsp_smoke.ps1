# Annota - tools/lsp_smoke.ps1 : drive the language server over stdio and check the answers.
#
#   powershell -ExecutionPolicy Bypass -File tools\lsp_smoke.ps1 [-Exe build\annota.exe]
#
# The script writes framed JSON-RPC requests to a temporary file, runs
# `annota lsp < requests > responses` and verifies that every request was answered.
param(
    [string]$Exe = "build\annota.exe",
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = if ([System.IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $root $Exe }
if (-not (Test-Path $exe)) { throw "cannot find $exe (build it first)" }

$source = @'
new items = [1, 2, 3]
new zero = 0

[[require: d != 0]]
divide(a, d) = a / d

check(n)(
    print items[7]
    print 10 / zero
    divide(1, 0)
    new unused = n
)
'@
$sourceJs = $source -replace '\\', '\\' -replace '"', '\"' -replace "`r`n", "\n" -replace "`n", "\n"

function Frame([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    return "Content-Length: $($bytes.Length)`r`n`r`n$json"
}

$uri = "file:///C:/tmp/sample.ant"
$requests = @()
$requests += Frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
$requests += Frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
$requests += Frame ('{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"' + $uri + '","languageId":"annota","version":1,"text":"' + $sourceJs + '"}}}')
$requests += Frame ('{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"' + $uri + '"},"position":{"line":4,"character":4}}}')
$requests += Frame ('{"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"' + $uri + '"},"position":{"line":7,"character":11}}}')
$requests += Frame ('{"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"' + $uri + '"},"position":{"line":7,"character":6}}}')
$requests += Frame ('{"jsonrpc":"2.0","id":5,"method":"textDocument/rename","params":{"textDocument":{"uri":"' + $uri + '"},"position":{"line":7,"character":11},"newName":"counter"}}')
$requests += Frame ('{"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":"' + $uri + '"}}}')
$requests += Frame '{"jsonrpc":"2.0","id":9,"method":"shutdown","params":null}'
$requests += Frame '{"jsonrpc":"2.0","method":"exit","params":null}'

$reqFile = Join-Path $env:TEMP "annota-lsp-requests.bin"
$resFile = Join-Path $env:TEMP "annota-lsp-responses.bin"
[System.IO.File]::WriteAllText($reqFile, ($requests -join ""), (New-Object System.Text.UTF8Encoding($false)))

$rc = 0
& cmd /c "`"$exe`" lsp < `"$reqFile`" > `"$resFile`""
if ($LASTEXITCODE -ne $null) { $rc = $LASTEXITCODE }
$out = [System.IO.File]::ReadAllText($resFile, [System.Text.Encoding]::UTF8)

$responses = ([regex]::Matches($out, "Content-Length:")).Count
$failures = @()
$expect = @{
    "initialize"    = '"capabilities"'
    "hover"         = '"contents"'
    "definition"    = '"uri"'
    "completion"    = '"items"'
    "rename"        = '"changes"'
    "didOpen/pub"   = 'publishDiagnostics'
    "divide"        = 'division-by-zero'
    "bounds"        = 'out-of-bounds'
    "contract"      = 'contract-violation'
}
foreach ($k in $expect.Keys) {
    if ($out -notmatch [regex]::Escape($expect[$k])) { $failures += "$k (missing $($expect[$k]))" }
}
if ($responses -lt 9) { $failures += "only $responses framed messages" }

Write-Host "lsp smoke: exit=$rc messages=$responses"
foreach ($k in $expect.Keys) {
    $mark = if ($out -match [regex]::Escape($expect[$k])) { "ok  " } else { "FAIL" }
    Write-Host "  $mark $k"
}
if (-not $Keep) { Remove-Item $reqFile, $resFile -ErrorAction SilentlyContinue }
if ($failures.Count -gt 0) {
    Write-Host "FAILED: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "lsp smoke passed" -ForegroundColor Green
exit 0
