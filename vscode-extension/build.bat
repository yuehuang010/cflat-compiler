@echo off
setlocal
cd /d "%~dp0"

echo === cflat VSCode Extension Build ===
echo.

REM Check for Node.js
where node >nul 2>&1
if errorlevel 1 (
    echo ERROR: Node.js is not installed or not on PATH.
    echo        Download from https://nodejs.org/
    exit /b 1
)

REM Sync deps unconditionally - npm decides whether anything needs doing. A
REM hand-rolled "is node_modules current" probe only catches deps someone
REM remembered to add a line for, and silently skips newly added ones.
REM "build.bat ci" installs from the lockfile only: reproducible, fails on
REM lockfile drift, never rewrites package-lock.json. Costs a full reinstall,
REM so the dev loop gets the incremental npm install instead.
if /i "%~1"=="ci" (
    echo [1/2] Installing npm dependencies from lockfile ^(npm ci^)...
    call npm ci --no-audit --no-fund
) else (
    echo [1/2] Syncing npm dependencies...
    call npm install --no-audit --no-fund
)
if errorlevel 1 (
    echo ERROR: npm dependency install failed.
    exit /b 1
)

REM Compile TypeScript
echo [2/3] Regenerating l10n bundles and compiling TypeScript...
call npm run compile
if errorlevel 1 (
    echo ERROR: TypeScript compilation failed.
    exit /b 1
)

REM Stamp the extension version from the compiler's Version.h (single source of truth)
call node sync-version.js
if errorlevel 1 (
    echo ERROR: Version sync failed.
    exit /b 1
)

REM Install vsce if not present
echo [3/3] Packaging extension as .vsix...
call npx vsce --version >nul 2>&1
if errorlevel 1 (
    call npm install --save-dev @vscode/vsce
    if errorlevel 1 ( echo ERROR: Failed to install vsce. & exit /b 1 )
)
call npx vsce package --allow-missing-repository --no-git-tag-version 2>&1
if errorlevel 1 (
    echo ERROR: Packaging failed.
    exit /b 1
)

echo.
echo === Build successful! ===
echo.
echo Next steps:
echo   launch.bat          - Open VSCode with the extension loaded (development mode)
echo   install.bat         - Install the .vsix into VSCode
echo.
endlocal
