@echo off
rem Rebuilds PULP from a clean checkout.
rem   build.bat            release build of everything, runs the tests, fills dist\PULP
rem   build.bat test       engine + tests only, in Debug with the checked debug heap, and runs them
rem   build.bat asan       the same with AddressSanitizer (needs that Visual Studio component installed)
rem   build.bat quick      release build of engine, render and tests only (no JUCE), runs the tests
rem   build.bat app        release build of the program only, fills dist\PULP, skips the tests
rem   build.bat testplugins   the two VST3 plugins that scripts\m10_vst3.txt hosts (into build\release\testplugins)
rem   build.bat installer     packs dist\PULP and an install script into dist\PULP-setup.zip (run build.bat first)
rem   build.bat library    fetches the recordings (tools\fetch-samples.py) and rebuilds the factory instruments
setlocal
cd /d "%~dp0"
call tools\env.bat || exit /b 1

if /i "%1"=="test" goto debug
if /i "%1"=="asan" goto asan
if /i "%1"=="quick" goto quick
if /i "%1"=="library" goto library

cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_APP=ON || exit /b 1
if /i "%1"=="installer" (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\installer\make.ps1 || exit /b 1
  exit /b 0
)
if /i "%1"=="testplugins" (
  cmake --build build\release --target testplugins || exit /b 1
  exit /b 0
)
if /i "%1"=="app" (
  cmake --build build\release --target dist || exit /b 1
  exit /b 0
)
cmake --build build\release || exit /b 1
build\release\pulp_tests.exe || exit /b 1
rem the factory instruments are built once; "build.bat library" rebuilds them
if not exist factory\Instruments call :makelibrary build\release
cmake --build build\release --target dist || exit /b 1
echo.
echo Built dist\PULP\PULP.exe
exit /b 0

:library
cmake -S . -B build\quick -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_APP=OFF || exit /b 1
cmake --build build\quick --target pulp_render || exit /b 1
call :makelibrary build\quick
exit /b %errorlevel%

rem Fetches the recordings listed in factory\sources.json (checksums verified) and builds the factory
rem instruments: from those recordings where there are any, by synthesis for the rest. Without Python
rem or a network the synthesized instruments are still built.
:makelibrary
%1\pulp_render.exe --make-factory factory || exit /b 1
where python >nul 2>&1 && python -I tools\fetch-samples.py
%1\pulp_render.exe --build-library factory\sources.json factory-cache factory || exit /b 1
where python >nul 2>&1 && python -I tools\make_credits.py
exit /b 0

:quick
cmake -S . -B build\quick -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_APP=OFF || exit /b 1
cmake --build build\quick || exit /b 1
build\quick\pulp_tests.exe %2 %3 %4 || exit /b 1
exit /b 0

:debug
cmake -S . -B build\debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_APP=OFF -DPULP_ASAN=OFF || exit /b 1
cmake --build build\debug || exit /b 1
build\debug\pulp_tests.exe %2 %3 %4 || exit /b 1
exit /b 0

:asan
cmake -S . -B build\asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_APP=OFF -DPULP_ASAN=ON || exit /b 1
cmake --build build\asan || exit /b 1
build\asan\pulp_tests.exe %2 %3 %4 || exit /b 1
exit /b 0
