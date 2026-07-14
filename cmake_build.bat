@echo off
setlocal EnableDelayedExpansion
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
REM
REM Worktree-friendly (same scheme as cmake_build.sh). The vcpkg dependency tree
REM lives OUTSIDE the source tree at %CFLAT_VCPKG_INSTALLED% (default
REM %USERPROFILE%\.cflat-compiler-deps\vcpkg_installed), so every worktree shares
REM one 26 GB copy and `git worktree add` needs no junction or post-processing.

set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VCPKG_ROOT set VCPKG_ROOT=%VS_ROOT%\VC\vcpkg
if not defined CFLAT_VCPKG_INSTALLED set CFLAT_VCPKG_INSTALLED=%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed

REM Quote the assignments: `set PRESET=win-x64-release )` would capture the
REM trailing space into the value and corrupt every path built from it.
set CFG=%~1
if /I "%CFG%"=="debug"   ( set "PRESET=win-x64-debug" )   else (
if /I "%CFG%"=="release" ( set "PRESET=win-x64-release" ) else (
if /I "%CFG%"==""        ( set "PRESET=win-x64-release" ) else (
  echo Unknown config "%CFG%" - use debug or release
  exit /b 2
)))

if not exist "%CFLAT_VCPKG_INSTALLED%\x64-windows-static\" (
  echo ERROR: shared dependency tree not found at "%CFLAT_VCPKG_INSTALLED%".
  echo Populate it once ^(restores LLVM from the binary cache, no source build^):
  echo   vcpkg-build.bat
  exit /b 1
)

REM CMake stores cache paths with forward slashes. Normalize so the comparison
REM below and the -D we pass both speak CMake's dialect.
set "WANT=%CFLAT_VCPKG_INSTALLED:\=/%"

REM A build dir configured against a different dependency tree has stale absolute
REM paths cached (LLVM_DIR etc). Reconfiguring in place would silently keep them.
set "CACHE=%~dp0build\%PRESET%\CMakeCache.txt"
if exist "%CACHE%" (
  set "CACHED="
  for /f "tokens=2 delims==" %%V in ('findstr /b "VCPKG_INSTALLED_DIR:" "%CACHE%"') do set "CACHED=%%V"
  if defined CACHED if /I not "!CACHED!"=="!WANT!" (
    echo == dependency tree moved ^(!CACHED! -^> !WANT!^); wiping stale build\%PRESET%
    rmdir /s /q "%~dp0build\%PRESET%"
  )
)

call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo Failed to initialize VS dev environment & exit /b 1 )

REM `cmake --preset` reads CMakePresets.json from the CWD, so anchor to this
REM script's own checkout. Without this, running <worktree>\cmake_build.bat from
REM another directory silently configures THAT directory's tree instead.
pushd "%~dp0"

REM -D makes CFLAT_VCPKG_INSTALLED actually override the preset's default path.
echo === Configuring %PRESET% ===
cmake --preset %PRESET% -D VCPKG_INSTALLED_DIR="!WANT!"
if errorlevel 1 ( popd & echo Configure failed & exit /b 1 )

echo === Building %PRESET% ===
cmake --build --preset %PRESET%
if errorlevel 1 ( popd & echo Build failed & exit /b 1 )

popd
echo === Done: x64\%PRESET:win-x64-=% layout ready ===
endlocal
