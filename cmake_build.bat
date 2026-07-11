@echo off
REM Cross-platform CMake build helper for Windows (Ninja + MSVC).
REM Sets up the VS dev environment + VCPKG_ROOT, then configures and builds.
REM
REM Usage:
REM   cmake_build.bat              builds win-x64-release
REM   cmake_build.bat debug        builds win-x64-debug
REM   cmake_build.bat release      builds win-x64-release
REM
REM Output: x64\<Config>\cflat.exe (+ core\, lld-link, clang-cl) - the layout
REM test.bat expects.
setlocal

set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VCPKG_ROOT set VCPKG_ROOT=%VS_ROOT%\VC\vcpkg

set CFG=%~1
if /I "%CFG%"=="debug"   ( set PRESET=win-x64-debug )   else (
if /I "%CFG%"=="release" ( set PRESET=win-x64-release ) else (
if /I "%CFG%"==""        ( set PRESET=win-x64-release ) else (
  echo Unknown config "%CFG%" - use debug or release
  exit /b 2
)))

call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo Failed to initialize VS dev environment & exit /b 1 )

echo === Configuring %PRESET% ===
cmake --preset %PRESET%
if errorlevel 1 ( echo Configure failed & exit /b 1 )

echo === Building %PRESET% ===
cmake --build --preset %PRESET%
if errorlevel 1 ( echo Build failed & exit /b 1 )

echo === Done: x64\%PRESET:win-x64-=% layout ready ===
endlocal
