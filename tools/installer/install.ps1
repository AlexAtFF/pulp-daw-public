# Installs PULP for the current Windows user. No administrator rights are needed and nothing outside
# the user's own profile is touched:
#   - copies the PULP folder to %LOCALAPPDATA%\Programs\PULP (or -Target)
#   - adds a Start Menu shortcut (and a desktop one with -Desktop)
#   - makes .pulp projects open in PULP for this user (skip with -NoAssociation)
# It changes no sound, microphone or privacy setting. Run "Uninstall PULP.cmd" in the installed folder to remove it.
param(
    [string]$Target = (Join-Path $env:LOCALAPPDATA 'Programs\PULP'),
    [switch]$Desktop,
    [switch]$NoShortcuts,
    [switch]$NoAssociation
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'PULP'
if (-not (Test-Path (Join-Path $source 'PULP.exe'))) { throw "PULP.exe was not found beside this script (looked in $source)." }

Write-Host "Installing PULP to $Target"
if (Get-Process -Name PULP -ErrorAction SilentlyContinue) { throw 'PULP is running. Close it and run the installer again.' }
New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item -Path (Join-Path $source '*') -Destination $Target -Recurse -Force
Copy-Item -Path (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $Target -Force
Set-Content -Path (Join-Path $Target 'Uninstall PULP.cmd') -Encoding ascii -Value "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0uninstall.ps1`"`r`npause"

$exe = Join-Path $Target 'PULP.exe'
if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $places = @([Environment]::GetFolderPath('Programs'))
    if ($Desktop) { $places += [Environment]::GetFolderPath('Desktop') }
    foreach ($place in $places) {
        $link = $shell.CreateShortcut((Join-Path $place 'PULP.lnk'))
        $link.TargetPath = $exe
        $link.WorkingDirectory = $Target
        $link.Description = 'PULP: make music from patterns'
        $link.Save()
    }
}
if (-not $NoAssociation) {
    # per-user file type: HKCU\Software\Classes, the same keys the program's own File menu item writes
    $classes = 'HKCU:\Software\Classes'
    New-Item -Path "$classes\.pulp" -Force | Out-Null
    Set-ItemProperty -Path "$classes\.pulp" -Name '(default)' -Value 'PULP.Project'
    New-Item -Path "$classes\PULP.Project\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "$classes\PULP.Project" -Name '(default)' -Value 'PULP project'
    Set-ItemProperty -Path "$classes\PULP.Project\shell\open\command" -Name '(default)' -Value "`"$exe`" `"%1`""
}
Write-Host 'Done. PULP is in the Start Menu.'
