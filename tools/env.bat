@echo off
rem Sets up the compiler, CMake and Ninja for this shell. Called by build.bat.
if not exist "%~dp0cmake\bin\cmake.exe" goto bootstrap
if not exist "%~dp0ninja\ninja.exe" goto bootstrap
goto tools_ok
:bootstrap
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bootstrap.ps1" || exit /b 1
:tools_ok
set "PATH=%~dp0cmake\bin;%~dp0ninja;%PATH%"
if defined VSCMD_VER goto :eof
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto have_vc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto have_vc
:no_vc
echo Could not find the MSVC C++ build tools.
exit /b 1
:have_vc
call "%VCVARS%" >nul 2>&1
