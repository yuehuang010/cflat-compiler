@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM rm-worktree.bat - safely remove a worktree created by new-worktree.bat.
REM Deletes the vcpkg_installed JUNCTION first (rmdir removes the link only,
REM never the shared target), THEN removes the git worktree. Do NOT use
REM `rm -rf` or Explorer on these worktrees: those can follow the junction and
REM delete the shared 26 GB vcpkg_installed.
REM
REM Usage:  rm-worktree.bat <worktree-path>
REM ---------------------------------------------------------------------------

if "%~1"=="" (
    echo Usage: rm-worktree.bat ^<worktree-path^>
    exit /b 1
)
for %%I in ("%~1") do set "WT_ABS=%%~fI"
set "LINK=!WT_ABS!\vcpkg_installed"

REM Remove the junction if present (rmdir on a junction unlinks; target is safe).
if exist "!LINK!" (
    echo == rmdir junction "!LINK!"
    rmdir "!LINK!"
    if errorlevel 1 (
        echo ERROR: could not remove junction. Aborting to protect shared tree.
        exit /b 1
    )
)

echo == git worktree remove "!WT_ABS!"
git worktree remove "!WT_ABS!" --force
if errorlevel 1 (
    echo ERROR: git worktree remove failed. The junction is already gone; you
    echo can finish manually with: git worktree remove "!WT_ABS!" --force
    exit /b 1
)
echo Done. Removed worktree "!WT_ABS!".
endlocal
