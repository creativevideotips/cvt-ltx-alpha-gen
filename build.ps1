# ---------------------------------------------------------------------------
# cvtLTXAlphaGen build.ps1 (Windows; build.sh is the Mac one)
#
# From Creative Video Tips (CVT) - LTX Alpha Gen for DaVinci Resolve and
# Fusion Studio, a port of Flame_LTXAlphaGen_OFX (MPL 2.0, see src/LICENSE
# and src/CVT_PORT.md).
#
# What it does:
#   1. Makes a small test Python with numpy in %USERPROFILE%\.cvt\ltx-alphagen-testpy
#      (only once), for the unit tests and the dummy worker.
#   2. Builds the OFX bundle into build\LTXAlphaGen.ofx.bundle (Visual Studio's
#      C++ compiler, x64), with ltxag_ui.exe beside the .ofx.
#   3. Puts the pinned uv.exe beside it (src/helper/model_manifest.json: checked
#      against its SHA-256). Windows has no system Python, so Setup starts with it.
#   4. Runs the unit tests; with -Systest also the fast system test, on a
#      separate test build in build-dev\.
#
#   .\build.ps1 [-Dev] [-Systest]
#   -Dev      show the Prototype section (Use Dummy test mode) in the bundle
#
# You need Visual Studio 2022 (or its Build Tools) with "Desktop development
# with C++", CMake, and Python 3.10 or later (or uv) on PATH.
# Nothing is installed: run .\install.ps1 afterwards.
# ---------------------------------------------------------------------------
param([switch]$Dev, [switch]$Systest)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $here 'build'
$devBuild = Join-Path $here 'build-dev'
$cvt = Join-Path $env:USERPROFILE '.cvt'
$testPy = Join-Path $cvt 'ltx-alphagen-testpy'
$testPython = Join-Path $testPy 'Scripts\python.exe'
$portVersion = 'cvt1.50'   # the version: Version label, and (as 1.50) the plugin version hosts store

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake is missing: install it from cmake.org (or: winget install Kitware.CMake)' }

if (-not (Test-Path $testPython)) {
  Write-Host "== making the test Python ($testPy)"
  New-Item -ItemType Directory -Force $cvt | Out-Null
  if (Get-Command uv -ErrorAction SilentlyContinue) {
    & uv venv -q --python 3.14 $testPy
    & uv pip install -q --python $testPython numpy
  } else {
    & python -m venv $testPy
    & $testPython -m pip install -q numpy
  }
  if (-not (Test-Path $testPython)) { throw 'could not make the test Python: is Python 3.10 or later (or uv) on PATH?' }
}

# The pinned uv for Windows, from the manifest Setup itself uses.
function Add-Uv([string]$bundle) {
  $man = Get-Content (Join-Path $here 'src\helper\model_manifest.json') -Raw | ConvertFrom-Json
  $asset = $man.uv.win32
  $zip = Join-Path $cvt "uv-$($man.uv.version)-windows.zip"
  if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $asset.sha256) {
    Write-Host "== downloading uv $($man.uv.version)"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $asset.url -OutFile $zip
  }
  if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $asset.sha256) { throw "uv download does not match the manifest's SHA-256: $zip" }
  $tmp = Join-Path $cvt 'uv-unpack'
  if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
  Expand-Archive -Path $zip -DestinationPath $tmp
  Copy-Item -Force (Join-Path $tmp 'uv.exe') (Join-Path $bundle 'Contents\Win64\uv.exe')
  Remove-Item -Recurse -Force $tmp
}

function Build([string]$dir, [string]$devFlag) {
  & cmake -S (Join-Path $here 'src') -B $dir -A x64 "-DLTXAG_DEV_BUILD=$devFlag" "-DLTXAG_GIT=$portVersion" `
    "-DLTXAG_VERSION=$($portVersion.Substring(3))" "-DWORKER_TEST_PYTHON=$($testPython -replace '\\', '/')" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'cmake could not configure the build (is Visual Studio with the C++ tools installed?)' }
  & cmake --build $dir --config Release -j 2>&1 | Select-String -Pattern 'error|warning C' | ForEach-Object { $_.Line }
  if (-not (Test-Path (Join-Path $dir 'LTXAlphaGen.ofx.bundle\Contents\Win64\LTXAlphaGen.ofx'))) { throw 'build FAILED' }
  Add-Uv (Join-Path $dir 'LTXAlphaGen.ofx.bundle')
}

$devFlag = if ($Dev) { 'ON' } else { 'OFF' }
Write-Host "== building (Prototype section: $devFlag)"
Build $build $devFlag
Write-Host '== unit tests'
Push-Location $build
try {
  & ctest -C Release --output-on-failure | Select-Object -Last 4
  if ($LASTEXITCODE -ne 0) { throw 'unit tests FAILED' }
} finally { Pop-Location }

if ($Systest) {
  if ($Dev) { $sys = $build } else { $sys = $devBuild; Write-Host '== test build'; Build $sys 'ON' }
  Write-Host '== fast system test (about 90 s)'
  $work = Join-Path $sys 'systest_work'
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'src\systest\run_systest.ps1') -Tier fast `
    -Bundle (Join-Path $sys 'LTXAlphaGen.ofx.bundle') -TestHost (Join-Path $sys 'Release\ltxag_systest.exe') `
    -Work $work -JobPython $testPython | Select-String -Pattern 'PASS|FAIL|tests,' | ForEach-Object { $_.Line }
  if ($LASTEXITCODE -ne 0) { throw "system test FAILED (see $work\out-fast\console.log)" }
}
Write-Host "Built $build\LTXAlphaGen.ofx.bundle ($portVersion). Next: .\install.ps1"
