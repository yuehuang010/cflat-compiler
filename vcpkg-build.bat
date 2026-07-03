@echo off
REM Provision (or restore) the vcpkg_installed dependency tree from vcpkg.json.
REM
REM The CMake presets set VCPKG_MANIFEST_INSTALL=OFF, so `cmake --preset` never
REM installs dependencies - it only consumes a pre-populated vcpkg_installed.
REM This script is the explicit, human-initiated step that populates it. Run it
REM once after a fresh clone or after `git clean -xdf` wipes vcpkg_installed.
REM
REM Usage:
REM   vcpkg-build.bat
REM
REM Cost: ~26 GB installed, ~50 min cold (LLVM dominates; ~1.7 h fully cold).
REM
REM Layout note: the win-x64 preset sets
REM   VCPKG_INSTALLED_DIR = <repo>\vcpkg_installed\x64-windows-static
REM and the vcpkg toolchain appends the target triplet, so it searches
REM   vcpkg_installed\x64-windows-static\x64-windows-static
REM That double-nesting is why --x-install-root points at the FIRST level below.
REM --clean-after-build drops build trees + staging so the tree stays ~26 GB
REM instead of ballooning past 200 GB.
setlocal

set SCRIPT_DIR=%~dp0
set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
if not defined VCPKG_ROOT set VCPKG_ROOT=%VS_ROOT%\VC\vcpkg

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo vcpkg.exe not found at "%VCPKG_ROOT%" - set VCPKG_ROOT and retry.
  exit /b 1
)

pushd "%SCRIPT_DIR%"

echo === Installing dependencies from vcpkg.json (this can take ~50 min) ===
"%VCPKG_ROOT%\vcpkg.exe" install ^
  --triplet x64-windows-static ^
  --host-triplet x64-windows ^
  --x-install-root=vcpkg_installed\x64-windows-static ^
  --clean-after-build
set RC=%errorlevel%

popd

if %RC% neq 0 ( echo vcpkg install failed & exit /b %RC% )
echo === Done: vcpkg_installed populated. Now run cmake_build.bat. ===
endlocal
