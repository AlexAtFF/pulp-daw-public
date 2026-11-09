# Removes what install.ps1 put in place: the shortcuts, the .pulp file type for this user, and the
# program folder this script sits in. Projects, settings and backups (in %APPDATA%\PULP and wherever
# projects were saved) are left alone.
param([switch]$KeepFolder)
$ErrorActionPreference = 'Stop'
$target = $PSScriptRoot
if (-not (Test-Path (Join-Path $target 'PULP.exe'))) { throw "This does not look like a PULP folder: $target" }
if (Get-Process -Name PULP -ErrorAction SilentlyContinue) { throw 'PULP is running. Close it first.' }

foreach ($place in @([Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('Desktop'))) {
    $link = Join-Path $place 'PULP.lnk'
    if (Test-Path $link) { Remove-Item $link -Force }
}
$classes = 'HKCU:\Software\Classes'
$command = (Get-ItemProperty -Path "$classes\PULP.Project\shell\open\command" -ErrorAction SilentlyContinue).'(default)'
# the file type is only removed if it points at this copy of the program
if ($command -and $command.ToLower().Contains((Join-Path $target 'PULP.exe').ToLower())) {
    Remove-Item -Path "$classes\PULP.Project" -Recurse -Force -ErrorAction SilentlyContinue
    if ((Get-ItemProperty -Path "$classes\.pulp" -ErrorAction SilentlyContinue).'(default)' -eq 'PULP.Project') { Remove-Item -Path "$classes\.pulp" -Recurse -Force }
}
if (-not $KeepFolder) {
    # a script cannot delete the folder it is running from while it runs: hand the last step to cmd
    Start-Process -FilePath cmd.exe -ArgumentList "/c timeout /t 2 /nobreak >nul & rmdir /s /q `"$target`"" -WindowStyle Hidden
}
Write-Host 'PULP has been removed. Your projects and settings were left where they are.'
