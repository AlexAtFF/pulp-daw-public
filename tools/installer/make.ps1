# Packs dist\PULP and the install scripts into dist\PULP-setup.zip. Run by "build.bat installer".
# Whoever unpacks the zip double-clicks "Install PULP.cmd".
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dist = Join-Path $root 'dist'
$program = Join-Path $dist 'PULP'
if (-not (Test-Path (Join-Path $program 'PULP.exe'))) { throw 'dist\PULP\PULP.exe is missing: run build.bat first.' }
$stage = Join-Path $dist 'PULP-setup'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -Path $program -Destination (Join-Path $stage 'PULP') -Recurse
Copy-Item -Path (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $stage
Copy-Item -Path (Join-Path $root 'README.md'), (Join-Path $root 'CREDITS.md') -Destination (Join-Path $stage 'PULP')
Set-Content -Path (Join-Path $stage 'Install PULP.cmd') -Encoding ascii -Value "@echo off`r`nrem Installs PULP for this Windows user (no administrator rights needed).`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0install.ps1`" %*`r`npause"
$zip = Join-Path $dist 'PULP-setup.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
# tar writes zip files quickly; the library is already compressed, so nothing is gained by trying harder
Push-Location $dist
try { & tar.exe -a -c -f 'PULP-setup.zip' 'PULP-setup' } finally { Pop-Location }
if (-not (Test-Path $zip)) { throw 'the zip was not written' }
Remove-Item $stage -Recurse -Force
Write-Host ("Wrote {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
