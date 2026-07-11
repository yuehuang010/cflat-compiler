@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM new-worktree.bat - create a git worktree that shares the main checkout's
REM 26 GB vcpkg_installed via a directory junction, so vcpkg never rebuilds it.
REM
REM Usage:
REM   new-worktree.bat <path> [<branch> | -b <new-branch>]
REM
REM Examples:
REM   new-worktree.bat ..\cflat-feature -b feature/foo
REM   new-worktree.bat ..\cflat-hotfix master
REM ---------------------------------------------------------------------------

if "%~1"=="" (
    echo Usage: new-worktree.bat ^<path^> [^<branch^> ^| -b ^<new-branch^>]
    exit /b 1
)

set "WT_PATH=%~1"

REM Locate the MAIN checkout (the worktree that owns the real .git). Its
REM vcpkg_installed is the shared source. --git-common-dir points at <main>\.git
REM (absolute when run from a linked worktree, relative ".git" from main).
REM Anchor to this script's own dir so it works regardless of the caller's CWD.
pushd "%~dp0"
for /f "delims=" %%G in ('git rev-parse --git-common-dir') do set "COMMON_GIT=%%G"
if "!COMMON_GIT!"=="" (
    popd
    echo ERROR: "%~dp0" is not inside a git repository.
    exit /b 1
)
for %%I in ("!COMMON_GIT!\..") do set "MAIN_ROOT=%%~fI"
popd
set "SRC_VCPKG=!MAIN_ROOT!\vcpkg_installed"

if not exist "!SRC_VCPKG!\" (
    echo ERROR: shared vcpkg_installed not found at "!SRC_VCPKG!".
    echo Build the main checkout once before creating shared worktrees.
    exit /b 1
)

REM Create the worktree (forward all args so -b / branch / commitish all work).
echo == git worktree add %*
git worktree add %*
if errorlevel 1 (
    echo ERROR: git worktree add failed.
    exit /b 1
)

REM Resolve the created worktree to an absolute path for the junction.
for %%I in ("%WT_PATH%") do set "WT_ABS=%%~fI"
set "LINK=!WT_ABS!\vcpkg_installed"

if exist "!LINK!" (
    echo NOTE: "!LINK!" already exists - leaving it as-is.
) else (
    echo == mklink /J "!LINK!" -^> "!SRC_VCPKG!"
    mklink /J "!LINK!" "!SRC_VCPKG!"
    if errorlevel 1 (
        echo ERROR: failed to create junction. The worktree exists but will
        echo rebuild vcpkg_installed unless you create the junction manually.
        exit /b 1
    )
)

REM ---------------------------------------------------------------------------
REM CRITICAL: a fresh `git worktree add` stamps vcpkg.json with mtime=now, which
REM is newer than the shared build's stamp. That defeats vcpkg's stamp-skip and
REM forces a full `vcpkg install` re-resolve on the worktree's first build - and
REM if the shared tree is even slightly ABI-stale, vcpkg REBUILDS (llvm etc.)
REM straight into the shared dir. We age the worktree's manifest files back to
REM the main checkout's vcpkg.json mtime so the worktree skips install just like
REM main does.
echo == aligning manifest mtimes to skip redundant vcpkg install
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$src = Get-Item -LiteralPath (Join-Path '!MAIN_ROOT!' 'vcpkg.json');" ^
  "foreach ($f in @('vcpkg.json','vcpkg-configuration.json')) {" ^
  "  $p = Join-Path '!WT_ABS!' $f;" ^
  "  if (Test-Path -LiteralPath $p) { (Get-Item -LiteralPath $p).LastWriteTime = $src.LastWriteTime } }"
if errorlevel 1 echo NOTE: could not align mtimes - first build may run a (fast) vcpkg re-validate.

echo.
echo Done. Worktree "!WT_ABS!" shares vcpkg_installed with "!MAIN_ROOT!".
echo Build it with:  cmake_build.bat release
endlocal
