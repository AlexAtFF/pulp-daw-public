# Downloads portable CMake and Ninja into this folder from their official release pages.
# No administrator rights needed. Versions are pinned.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmakeVer = '4.4.4'
$cmakeSha = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'
$ninjaVer = '1.13.2'
$ninjaSha = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'

function Get-Checked($url, $out, $sha) {
  Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
  $got = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $sha) { throw "Checksum mismatch for $url (got $got)" }
}

$dl = Join-Path $here 'dl'
New-Item -ItemType Directory -Force $dl | Out-Null
if (-not (Test-Path (Join-Path $here 'cmake\bin\cmake.exe'))) {
  Write-Host "Fetching CMake $cmakeVer"
  $zip = Join-Path $dl 'cmake.zip'
  Get-Checked "https://github.com/Kitware/CMake/releases/download/v$cmakeVer/cmake-$cmakeVer-windows-x86_64.zip" $zip $cmakeSha
  Expand-Archive $zip -DestinationPath $here -Force
  if (Test-Path (Join-Path $here 'cmake')) { Remove-Item -Recurse -Force (Join-Path $here 'cmake') }
  Rename-Item (Join-Path $here "cmake-$cmakeVer-windows-x86_64") 'cmake'
}
if (-not (Test-Path (Join-Path $here 'ninja\ninja.exe'))) {
  Write-Host "Fetching Ninja $ninjaVer"
  $zip = Join-Path $dl 'ninja.zip'
  Get-Checked "https://github.com/ninja-build/ninja/releases/download/v$ninjaVer/ninja-win.zip" $zip $ninjaSha
  Expand-Archive $zip -DestinationPath (Join-Path $here 'ninja') -Force
}
Remove-Item -Recurse -Force $dl
