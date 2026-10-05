# Annota interpreter - lightweight build script (MinGW g++ with optional Qt 6 GUI)
param(
    [string]$Config = "release",
    [switch]$Clean,
    [switch]$NoQt,
    [switch]$Verify,
    [string]$QtRoot = "D:\Qt\6.10.1\mingw_64"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build"
if ($Clean -and (Test-Path $outDir)) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$sources = Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { $_.FullName }
$exe = Join-Path $outDir "annota.exe"

$gpp = "D:\Qt\Tools\mingw1310_64\bin\g++.exe"
if (-not (Test-Path $gpp)) { $gpp = (Get-Command g++ -ErrorAction SilentlyContinue).Source }
if (-not $gpp) { throw "no g++ found: install MinGW-w64 (or Qt's bundled MinGW) and put it on PATH" }

# ---- Qt 6 (optional): enables `--gui` (a window) and `annota studio` (the IDE)
$qtInc = @()
$qtLib = @()
$qtDef = @()
$useQt = $false
$qtRoots = @($QtRoot)
# also look in the usual Qt install locations, so `-QtRoot` is only needed for custom setups
Get-ChildItem "D:\Qt","C:\Qt","$env:USERPROFILE\Qt" -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
        Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                foreach ($kit in @("mingw_64", "msvc2022_64", "msvc2019_64")) {
                    $cand = Join-Path $_.FullName $kit
                    if (Test-Path (Join-Path $cand "include\QtWidgets")) { $qtRoots += $cand }
                }
            }
    }
foreach ($candidate in $qtRoots) {
    if ($NoQt) { break }
    if (Test-Path (Join-Path $candidate "include\QtWidgets")) {
        $QtRoot = $candidate
        $useQt = $true
        $qtInc = @("-I", (Join-Path $QtRoot "include"),
                   "-I", (Join-Path $QtRoot "include\QtCore"),
                   "-I", (Join-Path $QtRoot "include\QtGui"),
                   "-I", (Join-Path $QtRoot "include\QtWidgets"))
        $qtLib = @("-L", (Join-Path $QtRoot "lib"), "-lQt6Widgets", "-lQt6Gui", "-lQt6Core")
        $qtDef = @("-DANNOTA_QT")
        break
    }
}

if ($gpp) {
    $flags = @("-std=c++17", "-finput-charset=UTF-8", "-fexec-charset=UTF-8", "-Wall",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-I", (Join-Path $root "src"), "-pthread")
    if ($Config -eq "release") { $flags += @("-O2", "-DNDEBUG") } else { $flags += @("-g", "-O0") }
    $flags += $qtInc + $qtDef
    Write-Host "[build] g++ $Config (Qt: $(if ($useQt) { "yes - $QtRoot" } else { 'no' })) -> $exe"
    if (-not $useQt -and -not $NoQt) {
        Write-Host "[build] NOTE: Qt 6 was not found, so the windowed GUI and the IDE are disabled." -ForegroundColor Yellow
        Write-Host "[build]       install Qt 6 or pass -QtRoot <path\to\6.x\mingw_64>, then rebuild." -ForegroundColor Yellow
        Write-Host "[build]       (pass -NoQt to silence this notice)" -ForegroundColor Yellow
    }
    & $gpp @flags @sources "-o" $exe @qtLib "-lws2_32"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    # remember what this binary can do, so `--features` / `--gui` can give exact advice
    Set-Content -Path (Join-Path $outDir "build-info.txt") -Encoding UTF8 `
        -Value ("qt=" + $(if ($useQt) { "yes" } else { "no" }) + "`nqtRoot=" + $QtRoot + "`nconfig=" + $Config)

    # make the Qt runtime available next to the executable (a self-contained build/)
    if ($useQt) {
        $mingwBin = Split-Path $gpp
        $copies = @(
            (Join-Path $QtRoot "bin\Qt6Core.dll"),
            (Join-Path $QtRoot "bin\Qt6Gui.dll"),
            (Join-Path $QtRoot "bin\Qt6Widgets.dll"),
            (Join-Path $mingwBin "libstdc++-6.dll"),
            (Join-Path $mingwBin "libgcc_s_seh-1.dll"),
            (Join-Path $mingwBin "libwinpthread-1.dll")
        )
        foreach ($f in $copies) { if (Test-Path $f) { Copy-Item $f $outDir -Force } else { Write-Host "[build] missing $f" } }
        $platDst = Join-Path $outDir "platforms"
        New-Item -ItemType Directory -Force -Path $platDst | Out-Null
        # qwindows for normal runs, qoffscreen for head-less CI (QT_QPA_PLATFORM=offscreen)
        foreach ($plat in @("qwindows.dll", "qoffscreen.dll", "qminimal.dll")) {
            $platSrc = Join-Path $QtRoot "plugins\platforms\$plat"
            if (Test-Path $platSrc) { Copy-Item $platSrc $platDst -Force }
            elseif ($plat -eq "qwindows.dll") { Write-Host "[build] missing $platSrc" }
        }
        $styleDst = Join-Path $outDir "styles"
        New-Item -ItemType Directory -Force -Path $styleDst | Out-Null
        Get-ChildItem (Join-Path $QtRoot "plugins\styles\*.dll") -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName $styleDst -Force }
        # qt.conf keeps Qt from looking into the build machine's Qt installation
        Set-Content -Path (Join-Path $outDir "qt.conf") -Encoding ASCII -Value "[Paths]`nPlugins = ."
    }
} else {
    Write-Host "[build] falling back to CMake"
    cmake -S $root -B $outDir -DCMAKE_BUILD_TYPE=Release
    cmake --build $outDir --config Release
}
Write-Host "[build] ok: $exe"

# ---- optional end-to-end verification: examples, analysis fixtures, benchmark, LSP
if ($Verify) {
    $failed = 0
    $examples = @("selfcheck.ant", "stdlib.ant", "algorithms.ant", "smoke.ant",
                  "buffer.ant", "gui_counter.ant", "modules.ant", "files.ant", "system.ant",
                  "syntax.ant")
    Write-Host "[verify] running $($examples.Count) examples"
    foreach ($e in $examples) {
        $path = Join-Path $root "examples\$e"
        if (-not (Test-Path $path)) { Write-Host "  FAIL $e (missing)"; $failed++; continue }
        & $exe $path > $null 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "  FAIL $e (exit $LASTEXITCODE)"; $failed++ } else { Write-Host "  ok   $e" }
    }
    Write-Host "[verify] analysis fixtures"
    & $exe analyze-suite (Join-Path $root "examples\analysis")
    if ($LASTEXITCODE -ne 0) { $failed++ }
    Write-Host "[verify] analyzer is silent on the examples"
    foreach ($e in $examples) {
        $path = Join-Path $root "examples\$e"
        if (-not (Test-Path $path)) { Write-Host "  FAIL $e (missing)"; $failed++; continue }
        $report = & $exe analyze $path 2>&1 | Select-Object -Last 1
        if ($report -notmatch "0 errors, 0 warnings") { Write-Host "  FAIL $e :: $report"; $failed++ } else { Write-Host "  ok   $e" }
    }
    Write-Host "[verify] benchmark"
    & $exe bench
    if ($LASTEXITCODE -ne 0) { $failed++ }
    Write-Host "[verify] lsp smoke"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\lsp_smoke.ps1") -Exe $exe
    if ($LASTEXITCODE -ne 0) { $failed++ }
    if ($useQt) {
        Write-Host "[verify] input: console reads stdin, GUI asks through a dialog"
    $piped = "annota`nworld" | & $exe (Join-Path $root "examples\input.ant") 2>&1 | Out-String
    if ($piped -notmatch "0 failures" -or $piped -notmatch "annota") {
        Write-Host "  FAIL input from stdin"; $failed++
    } else { Write-Host "  ok   input from stdin" }
    if ($useQt) {
        $env:ANNOTA_INPUT = "hello"
        $gui = & $exe studio (Join-Path $root "examples\input.ant") --run --echo `
                             --shot (Join-Path $outDir "ide-input.png") 2>&1 | Out-String
        Remove-Item Env:\ANNOTA_INPUT -ErrorAction SilentlyContinue
        if ($gui -notmatch "hello") { Write-Host "  FAIL input through the GUI provider"; $failed++ }
        else { Write-Host "  ok   input through the GUI provider" }
    }
    Write-Host "[verify] documentation links"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\doc_links.ps1") -Root $root
    if ($LASTEXITCODE -ne 0) { $failed++ }
    Write-Host "[verify] IDE (annota studio) renders headlessly"
    # CI runners have no interactive desktop: render through Qt's offscreen platform.
    # Qt prints font/plugin diagnostics on stderr, which PowerShell would turn into a
    # terminating error under -ErrorActionPreference Stop, so relax it for these calls.
    $savedQpa = $env:QT_QPA_PLATFORM
    $savedEap = $ErrorActionPreference
    $env:QT_QPA_PLATFORM = "offscreen"
    $ErrorActionPreference = "Continue"
        $shot = Join-Path $outDir "ide-verify.png"
        # Qt warnings go to stderr; keep them out of the capture so PowerShell does not treat
        # them as a command failure
        & $exe studio (Join-Path $root "examples\analysis\01_basics.ant") --run --shot $shot 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $shot)) { Write-Host "  FAIL studio --shot"; $failed++ }
        else { Write-Host "  ok   studio --shot ($([int]((Get-Item $shot).Length / 1024)) KB)" }
        # the IDE must be able to open a program's own `view` window (F7)
        $preview = & $exe studio (Join-Path $root "examples\gui_counter.ant") --preview `
                                 --shot (Join-Path $outDir "ide-preview.png") 2>$null | Out-String
        if ($preview -notmatch "Counter") { Write-Host "  FAIL studio --preview (no view window)"; $failed++ }
        else { Write-Host "  ok   studio --preview (view window opened)" }
        if ($savedQpa) { $env:QT_QPA_PLATFORM = $savedQpa } else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
        $ErrorActionPreference = $savedEap
    } else {
        Write-Host "[verify] IDE skipped (no Qt in this build)" -ForegroundColor Yellow
    }
    if ($failed -gt 0) { Write-Host "[verify] $failed check(s) failed" -ForegroundColor Red; exit 1 }
    Write-Host "[verify] all checks passed" -ForegroundColor Green
}
