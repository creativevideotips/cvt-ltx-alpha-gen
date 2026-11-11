# Windows port: runs one tier of the system test on this PC, as run_systest.sh does on a Mac
# (systest/README.md).
#   systest\run_systest.ps1 -Tier fast|full -Bundle <LTXAlphaGen.ofx.bundle> -TestHost <ltxag_systest.exe>
#                           -Work <dir> [-JobPython <python with numpy>] [-FakeUi <ltxag_fake_ui.exe>]
#                           [-Only a,b] [-JobTimeout s]
# <dir> gets:
#   home\       the isolated user folder (config, cache, logs): recreated on every run
#   bundle\     a copy of the bundle whose ltxag_ui.exe is the fake window (systest/fake_ui.cpp)
#   ui-<tier>\  the fake windows' logs;  out-<tier>\  junit-<tier>.xml, console.log, logs.zip
# The real user config (%USERPROFILE%\.config\ltx-alphagen\config.conf) supplies ltx_python and
# model_dir, and its cache root the prompt embedding; nothing of the real setup is written to.
param(
  [Parameter(Mandatory = $true)][ValidateSet('fast', 'full')][string]$Tier,
  [Parameter(Mandatory = $true)][string]$Bundle,
  [Parameter(Mandatory = $true)][string]$TestHost,
  [Parameter(Mandatory = $true)][string]$Work,
  [string]$JobPython = '',
  [string]$FakeUi = '',
  [string]$Only = '',
  [string]$JobTimeout = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Bundle -PathType Container)) { throw "no bundle at $Bundle" }
if (-not (Test-Path $TestHost -PathType Leaf)) { throw "no test host at $TestHost" }
if (-not $FakeUi) { $FakeUi = Join-Path (Split-Path $TestHost) 'ltxag_fake_ui.exe' }
if (-not (Test-Path $FakeUi -PathType Leaf)) { throw "no fake window program at $FakeUi" }

$realHome = $env:USERPROFILE
$realConfig = Join-Path $realHome '.config\ltx-alphagen\config.conf'
function Get-Conf([string]$key) {
  if (-not (Test-Path $realConfig)) { return '' }
  $line = Get-Content $realConfig | Where-Object { $_ -like "$key=*" } | Select-Object -Last 1
  if ($line) { return $line.Substring($key.Length + 1) } else { return '' }
}
$ltxPython = Get-Conf 'ltx_python'
$modelDir = (Get-Conf 'model_dir').TrimEnd('\', '/')
$realCache = Get-Conf 'cache_dir'
if (-not $realCache) { $realCache = Join-Path $realHome '.cache\ltx-alphagen\mattes' }
$prompt = Join-Path $realCache 'prompt-empty.safetensors'
if ($Tier -eq 'full' -and (-not $ltxPython -or -not (Test-Path $ltxPython) -or -not $modelDir -or
    -not (Test-Path $modelDir) -or -not (Test-Path $prompt))) {
  Write-Error "full tier: needs a completed Setup for this user (ltx_python, model_dir and the prompt cache in $realConfig)"
  exit 2
}
# The jobs run under the runtime's Python, as in the plugin; the fast tier takes any Python with numpy.
if (-not $JobPython) { $JobPython = $ltxPython }
if (-not $JobPython -or -not (Test-Path $JobPython)) {
  Write-Error 'give -JobPython <a python.exe with numpy> (there is no completed Setup to take one from)'
  exit 2
}
if (-not $ltxPython -or -not (Test-Path $ltxPython)) { $ltxPython = $JobPython }

New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$testHome = Join-Path $Work 'home'
$ui = Join-Path $Work "ui-$Tier"
$out = Join-Path $Work "out-$Tier"
$copy = Join-Path $Work 'bundle\LTXAlphaGen.ofx.bundle'
foreach ($d in @($testHome, $ui, $out, (Join-Path $Work 'bundle'))) {
  if (Test-Path $d) { Remove-Item -Recurse -Force $d }
}
New-Item -ItemType Directory -Force (Join-Path $testHome '.cache\ltx-alphagen\mattes'), $ui, $out | Out-Null
if (Test-Path $prompt) {
  # A copy, not a link: a link needs Developer Mode or admin rights on Windows.
  Copy-Item $prompt (Join-Path $testHome '.cache\ltx-alphagen\mattes\prompt-empty.safetensors')
}
# The fake progress and message windows: the bundle's ltxag_ui.exe, in a copy of the bundle.
Copy-Item -Recurse $Bundle $copy
Copy-Item -Force $FakeUi (Join-Path $copy 'Contents\Win64\ltxag_ui.exe')

$hostArgs = @('--bundle', $copy, '--tier', $Tier, '--out', $out, '--ltx-python', $ltxPython, '--job-python', $JobPython)
if ($modelDir) { $hostArgs += @('--model-dir', $modelDir) }
if ($Only) { $hostArgs += @('--only', $Only) }
if ($JobTimeout) { $hostArgs += @('--job-timeout', $JobTimeout) }
$console = Join-Path $out 'console.log'
"run_systest: tier $Tier, bundle $Bundle, work $Work" | Tee-Object -FilePath $console
$env:USERPROFILE = $testHome
$env:LTXAG_FAKE_UI_DIR = $ui
$ErrorActionPreference = 'Continue'
try {
  & $TestHost @hostArgs 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $console -Append
  $rc = $LASTEXITCODE
} finally {
  $env:USERPROFILE = $realHome
  Remove-Item Env:LTXAG_FAKE_UI_DIR -ErrorAction SilentlyContinue
}
# Whatever the plugin, job.py and the worker logged, for the report.
$logs = Join-Path $testHome '.cache\ltx-alphagen\logs'
if (Test-Path $logs) {
  Compress-Archive -Force -Path $logs, $ui -DestinationPath (Join-Path $out 'logs.zip') -ErrorAction SilentlyContinue
}
"run_systest: exit $rc" | Tee-Object -FilePath $console -Append
exit $rc
