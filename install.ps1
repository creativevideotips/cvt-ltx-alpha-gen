# ---------------------------------------------------------------------------
# cvtLTXAlphaGen install.ps1 (Windows; install.sh is the Mac one)
#
# From Creative Video Tips (CVT) - LTX Alpha Gen for DaVinci Resolve, a port
# of Flame_LTXAlphaGen_OFX (MPL 2.0, see src/LICENSE and src/CVT_PORT.md).
#
# What it does:
#   Copies build\LTXAlphaGen.ofx.bundle into
#   C:\Program Files\Common Files\OFX\Plugins, where Resolve and Fusion Studio
#   look for OFX plugins. That folder needs administrator rights, so Windows
#   asks you to allow it.
#
#   .\install.ps1              install (replaces an earlier copy)
#   .\install.ps1 -Uninstall   remove it
#
# Quit Resolve and Fusion Studio first; they find the plugin at their next
# start. Models, the LTX runtime and the matte cache are not touched (see
# README.md).
# ---------------------------------------------------------------------------
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$name = 'LTXAlphaGen.ofx.bundle'
$dest = Join-Path $env:CommonProgramFiles 'OFX\Plugins'
$src = Join-Path $here "build\$name"

if (Get-Process -Name Resolve, Fusion -ErrorAction SilentlyContinue) {
  Write-Error 'Resolve or Fusion Studio is running: quit it first, then run this again. Nothing was changed.'
  exit 1
}
if (-not $Uninstall -and -not (Test-Path (Join-Path $src 'Contents\Win64\LTXAlphaGen.ofx'))) {
  Write-Error 'No build found: run .\build.ps1 first.'
  exit 1
}
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
  # Start again as administrator (Windows asks), and wait for it.
  $psArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
  if ($Uninstall) { $psArgs += '-Uninstall' }
  $p = Start-Process powershell -ArgumentList $psArgs -Verb RunAs -Wait -PassThru
  if ($p.ExitCode -ne 0) { Write-Error 'Not installed (the administrator step did not finish).'; exit 1 }
  if ($Uninstall) { Write-Host "Removed $dest\$name." }
  else { Write-Host "Installed $dest\$name. Start Resolve: Fusion page > Effects > OpenFX > Creative Video Tips > LTX Alpha Gen." }
  exit 0
}

$target = Join-Path $dest $name
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
if ($Uninstall) { exit 0 }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Recurse $src $target
# Compiled Python caches from the build's test runs carry the build PC's paths (cvt1.44).
Get-ChildItem -Recurse -Directory -Filter '__pycache__' $target | Remove-Item -Recurse -Force
exit 0
