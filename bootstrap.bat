@echo off
setlocal EnableDelayedExpansion
REM One-command Windows setup: clean clone -> verified Release build.
REM
REM Steps (each is skipped if its output already exists):
REM   1. Toolchain check   - VS + vcvars64, cmake, ninja, git, antlr4
REM   2. vcpkg deps        - antlr4 / nlohmann-json / simdjson into the shared tree
REM   3. LLVM 23.1.0       - clone + source build + install (RTTI ON, /MT CRT)
REM                          with /debug, a second Release+Asserts /MTd install
REM   4. cflat             - cmake_build.bat release
REM   5. Compiler cache    - cflat --init-local
REM   6. Test suite        - test.bat Release
REM
REM Usage:
REM   bootstrap.bat                 full run
REM   bootstrap.bat /debug          also provision the assertions LLVM and build Debug
REM   bootstrap.bat /skip-llvm      reuse the existing LLVM install tree
REM   bootstrap.bat /skip-tests     stop after the build + cache step
REM   bootstrap.bat /fresh          wipe the vcpkg tree and LLVM install first
REM   bootstrap.bat /llvm-only      provision LLVM and stop
REM   bootstrap.bat /keep-build     keep the ~5 GB LLVM ninja tree (deleted by default
REM                                 once the install succeeds)
REM
REM Disk left behind by step 3: ~3.3 GB install tree + ~1.9 GB source clone, both under
REM %USERPROFILE%\.cflat-compiler-deps and shared by every worktree.
REM
REM Windows has no RTTI-enabled LLVM prebuilt (llvm.org ships RTTI OFF, and vcpkg
REM is stuck at 18), so the source build is mandatory - see
REM internal/plan/llvm-version-migration.md. Budget ~1-1.5 h for step 3; the rest
REM is minutes. By default only the /MT (Release) LLVM is built; /debug adds the
REM second install cflat Debug links against - same Release optimization level with
REM LLVM_ENABLE_ASSERTIONS=ON, built against the /MTd CRT because MSVC's debug and
REM release runtimes are ABI-incompatible (_ITERATOR_DEBUG_LEVEL changes std:: layouts).

set "SCRIPT_DIR=%~dp0"
set "LLVM_VER=23.1.0"
set "LLVM_TAG=llvmorg-%LLVM_VER%"

if not defined CFLAT_DEPS set "CFLAT_DEPS=%USERPROFILE%\.cflat-compiler-deps"
if not defined CFLAT_VCPKG_INSTALLED set "CFLAT_VCPKG_INSTALLED=%CFLAT_DEPS%\vcpkg_installed"
set "LLVM_INSTALL=%CFLAT_DEPS%\llvm-%LLVM_VER%"
set "LLVM_SRC=%CFLAT_DEPS%\src\llvm-project"
set "LLVM_BUILD=%CFLAT_DEPS%\build\llvm-%LLVM_VER%-mt"
set "LLVM_INSTALL_ASSERT=%CFLAT_DEPS%\llvm-%LLVM_VER%-assert"
set "LLVM_BUILD_ASSERT=%CFLAT_DEPS%\build\llvm-%LLVM_VER%-mtd"

set "SKIP_LLVM="
set "SKIP_TESTS="
set "FRESH="
set "LLVM_ONLY="
set "KEEP_BUILD="
set "WITH_DEBUG="
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="/skip-llvm"  ( set "SKIP_LLVM=1" ) else (
if /I "%~1"=="/skip-tests" ( set "SKIP_TESTS=1" ) else (
if /I "%~1"=="/fresh"      ( set "FRESH=1" ) else (
if /I "%~1"=="/llvm-only"  ( set "LLVM_ONLY=1" ) else (
if /I "%~1"=="/keep-build" ( set "KEEP_BUILD=1" ) else (
if /I "%~1"=="/debug"      ( set "WITH_DEBUG=1" ) else (
  echo Unknown option "%~1"
  echo Usage: bootstrap.bat [/debug] [/skip-llvm] [/skip-tests] [/fresh] [/llvm-only] [/keep-build]
  exit /b 2
))))))
shift
goto parse
:parsed

REM ---------------------------------------------------------------------------
echo === [1/6] Toolchain ===
REM ---------------------------------------------------------------------------
if not defined VS_ROOT (
  for %%E in (18 2022) do for %%D in (Enterprise Professional Community BuildTools) do (
    if not defined VS_ROOT if exist "C:\Program Files\Microsoft Visual Studio\%%E\%%D\VC\Auxiliary\Build\vcvars64.bat" (
      set "VS_ROOT=C:\Program Files\Microsoft Visual Studio\%%E\%%D"
    )
  )
)
if not defined VS_ROOT (
  echo ERROR: no Visual Studio with vcvars64.bat found.
  echo Install VS 2022 or newer with the "Desktop development with C++" workload,
  echo or set VS_ROOT to its install directory.
  exit /b 1
)
echo Visual Studio: %VS_ROOT%
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 1 )

REM VS ships cmake + ninja but vcvars64 does not put them on PATH. Only prepend
REM them when the host has no usable ninja/cmake of its own.
set "VS_CMAKE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
where ninja >nul 2>&1 || set "PATH=%VS_CMAKE%\Ninja;%PATH%"
where ninja >nul 2>&1 || ( echo ERROR: ninja not found ^(expected "%VS_CMAKE%\Ninja"^) & exit /b 1 )
REM Prefer the VS CMake over an unrelated one earlier on PATH (Strawberry Perl
REM ships a 3.29 cmake that predates some presets features).
if exist "%VS_CMAKE%\CMake\bin\cmake.exe" set "PATH=%VS_CMAKE%\CMake\bin;%PATH%"
where cmake >nul 2>&1 || ( echo ERROR: cmake not found & exit /b 1 )
where git   >nul 2>&1 || ( echo ERROR: git not found on PATH & exit /b 1 )
where antlr4 >nul 2>&1 || (
  echo ERROR: the antlr4 wrapper is not on PATH. Install it with:
  echo   pip install antlr4-tools
  echo ^(it downloads its own JRE; alternatively install a JRE and pass -DANTLR4_JAR^)
  exit /b 1
)
for /f "tokens=3" %%V in ('cmake --version ^| findstr /r "^cmake"') do echo cmake %%V
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VS_ROOT%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo ERROR: vcpkg.exe not found at "%VCPKG_ROOT%".
  echo Set VCPKG_ROOT to a bootstrapped vcpkg clone and retry.
  exit /b 1
)
echo vcpkg: %VCPKG_ROOT%

if defined FRESH (
  echo == /fresh: removing "%CFLAT_VCPKG_INSTALLED%" and "%LLVM_INSTALL%"
  if exist "%CFLAT_VCPKG_INSTALLED%" rmdir /s /q "%CFLAT_VCPKG_INSTALLED%"
  if exist "%LLVM_INSTALL%" rmdir /s /q "%LLVM_INSTALL%"
  if exist "%LLVM_INSTALL_ASSERT%" rmdir /s /q "%LLVM_INSTALL_ASSERT%"
  if exist "%SCRIPT_DIR%build" rmdir /s /q "%SCRIPT_DIR%build"
)

REM ---------------------------------------------------------------------------
echo === [2/6] vcpkg dependencies ===
REM ---------------------------------------------------------------------------
REM Always run it: vcpkg reconciles the tree against vcpkg.json, so this both
REM installs what is missing and REMOVES what the manifest dropped (the old LLVM
REM 18 port). It is a fast no-op once the tree already matches.
echo Reconciling "%CFLAT_VCPKG_INSTALLED%" against vcpkg.json
pushd "%SCRIPT_DIR%"
"%VCPKG_ROOT%\vcpkg.exe" install --triplet x64-windows-static --host-triplet x64-windows ^
  --x-install-root="%CFLAT_VCPKG_INSTALLED%" --clean-after-build
set "STEP_RC=!errorlevel!"
popd
if not "!STEP_RC!"=="0" ( echo ERROR: vcpkg install failed & exit /b !STEP_RC! )

REM ---------------------------------------------------------------------------
echo === [3/6] LLVM %LLVM_VER% ^(source build, RTTI ON^) ===
REM ---------------------------------------------------------------------------
call :ProvisionLLVM "%LLVM_INSTALL%" "%LLVM_BUILD%" OFF MultiThreaded "/MT, asserts OFF"
if errorlevel 1 exit /b 1

REM The assert tree is the SAME Release optimization level - only assertions and the
REM CRT differ. /MTd is mandatory: cflat Debug links the debug CRT and MSVC gives the
REM two runtimes incompatible std:: layouts. It is opt-in because it doubles bootstrap.
if defined WITH_DEBUG (
  call :ProvisionLLVM "%LLVM_INSTALL_ASSERT%" "%LLVM_BUILD_ASSERT%" ON MultiThreadedDebug "/MTd, asserts ON"
  if errorlevel 1 exit /b 1
)

if defined LLVM_ONLY ( echo === /llvm-only: stopping after provisioning === & exit /b 0 )

REM ---------------------------------------------------------------------------
echo === [4/6] Building cflat ^(Release^) ===
REM ---------------------------------------------------------------------------
call "%SCRIPT_DIR%cmake_build.bat" release
if errorlevel 1 ( echo ERROR: cflat build failed & exit /b 1 )
if defined WITH_DEBUG (
  echo === [4/6] Building cflat ^(Debug^) ===
  call "%SCRIPT_DIR%cmake_build.bat" debug
  if errorlevel 1 ( echo ERROR: cflat Debug build failed & exit /b 1 )
)

REM ---------------------------------------------------------------------------
echo === [5/6] Compiler cache ===
REM ---------------------------------------------------------------------------
"%SCRIPT_DIR%x64\Release\cflat.exe" --init-local
if errorlevel 1 ( echo ERROR: --init-local failed & exit /b 1 )
if defined WITH_DEBUG (
  "%SCRIPT_DIR%x64\Debug\cflat.exe" --init-local
  if errorlevel 1 ( echo ERROR: Debug --init-local failed & exit /b 1 )
)

if defined SKIP_TESTS ( echo === /skip-tests: done === & exit /b 0 )

REM ---------------------------------------------------------------------------
echo === [6/6] Test suite ===
REM ---------------------------------------------------------------------------
pushd "%SCRIPT_DIR%"
call "%SCRIPT_DIR%test.bat" Release
set "STEP_RC=%errorlevel%"
popd
if not "%STEP_RC%"=="0" ( echo === Tests reported failures ^(exit %STEP_RC%^) === & exit /b %STEP_RC% )
if defined WITH_DEBUG (
  echo === [6/6] Test suite ^(Debug^) ===
  pushd "%SCRIPT_DIR%"
  call "%SCRIPT_DIR%test.bat" Debug
  set "STEP_RC=!errorlevel!"
  popd
  if not "!STEP_RC!"=="0" ( echo === Debug tests reported failures ^(exit !STEP_RC!^) === & exit /b !STEP_RC! )
)
echo === Bootstrap complete: x64\Release\cflat.exe ===
exit /b 0

REM ===========================================================================
REM Subroutines
REM ===========================================================================

REM Bring the shared shallow clone to %LLVM_TAG%. Idempotent, so every
REM ProvisionLLVM call can invoke it; the second one is a no-op echo.
:SyncSource
if not exist "%LLVM_SRC%\llvm\CMakeLists.txt" (
  echo Cloning %LLVM_TAG% into "%LLVM_SRC%" ^(~2.6 GB^)
  if exist "%LLVM_SRC%" rmdir /s /q "%LLVM_SRC%"
  if not exist "%CFLAT_DEPS%\src" mkdir "%CFLAT_DEPS%\src"
  git clone --depth 1 --branch %LLVM_TAG% https://github.com/llvm/llvm-project.git "%LLVM_SRC%"
  if errorlevel 1 ( echo ERROR: clone failed & exit /b 1 )
  exit /b 0
)
REM The clone is shallow and pinned to whichever tag a previous run built, so a
REM version bump must fetch the new tag before reusing the tree.
pushd "%LLVM_SRC%"
set "LLVM_SRC_TAG=unknown"
for /f "delims=" %%t in ('git describe --tags 2^>nul') do set "LLVM_SRC_TAG=%%t"
if /I "!LLVM_SRC_TAG!"=="%LLVM_TAG%" (
  echo Reusing source at "%LLVM_SRC%" ^(%LLVM_TAG%^)
) else (
  echo Source at "%LLVM_SRC%" is !LLVM_SRC_TAG!; fetching %LLVM_TAG%
  git fetch --depth 1 origin tag %LLVM_TAG%
  if errorlevel 1 ( popd & echo ERROR: fetch of %LLVM_TAG% failed & exit /b 1 )
  git checkout --force %LLVM_TAG%
  if errorlevel 1 ( popd & echo ERROR: checkout of %LLVM_TAG% failed & exit /b 1 )
)
popd
exit /b 0

REM Configure + build + install one LLVM tree.
REM   %1 install dir  %2 build dir  %3 LLVM_ENABLE_ASSERTIONS  %4 CRT  %5 label
:ProvisionLLVM
setlocal EnableDelayedExpansion
set "INST=%~1"
set "BLD=%~2"
set "ASSERTS=%~3"
set "RUNTIME=%~4"
echo -- LLVM %LLVM_VER% ^(%~5^) into "!INST!"
if defined SKIP_LLVM ( echo /skip-llvm: using "!INST!" as-is & exit /b 0 )
if exist "!INST!\lib\cmake\llvm\LLVMConfig.cmake" ( echo Already installed: !INST! & exit /b 0 )
call :SyncSource
if errorlevel 1 ( exit /b 1 )

REM CMAKE_MSVC_RUNTIME_LIBRARY is what selects the CRT (/MT for the Release consumer,
REM /MTd for the assert tree cflat Debug links). LLVM_USE_CRT_RELEASE was removed in
REM LLVM 23 and is no longer passed. RTTI is a hard requirement because cflat derives
REM from polymorphic LLVM/Clang bases.
echo Configuring LLVM ^(this build takes ~1-1.5 h^)
cmake -G Ninja -S "%LLVM_SRC%\llvm" -B "!BLD!" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX="!INST!" ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=!RUNTIME! ^
  -DLLVM_ENABLE_PROJECTS="clang;lld" ^
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" ^
  -DLLVM_ENABLE_RTTI=ON ^
  -DLLVM_ENABLE_EH=ON ^
  -DLLVM_ENABLE_ASSERTIONS=!ASSERTS! ^
  -DLLVM_BUILD_LLVM_DYLIB=OFF ^
  -DLLVM_LINK_LLVM_DYLIB=OFF ^
  -DLLVM_INCLUDE_TESTS=OFF ^
  -DLLVM_INCLUDE_BENCHMARKS=OFF ^
  -DLLVM_INCLUDE_EXAMPLES=OFF ^
  -DLLVM_INCLUDE_DOCS=OFF ^
  -DLLVM_ENABLE_ZLIB=OFF ^
  -DLLVM_ENABLE_ZSTD=OFF ^
  -DLLVM_ENABLE_LIBXML2=OFF ^
  -DLLVM_PARALLEL_LINK_JOBS=4
if errorlevel 1 ( echo ERROR: LLVM configure failed & exit /b 1 )

cmake --build "!BLD!"
if errorlevel 1 ( echo ERROR: LLVM build failed & exit /b 1 )
cmake --install "!BLD!"
if errorlevel 1 ( echo ERROR: LLVM install failed & exit /b 1 )
echo LLVM installed to "!INST!"

REM The ~5 GB ninja tree is pure intermediate output: everything cflat consumes now
REM lives in the install tree. Drop it once the install succeeded. /keep-build retains
REM it for an incremental re-bump (the source clone is kept either way, so a rebuild
REM re-configures rather than re-downloading).
if defined KEEP_BUILD (
  echo /keep-build: retaining the build tree at "!BLD!"
) else (
  echo Removing intermediate build tree "!BLD!"
  rmdir /s /q "!BLD!"
)
exit /b 0
