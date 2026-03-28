@echo off
setlocal
cd /d "%~dp0"

echo === MyCompiler VSCode Extension Build ===
echo.

REM Check for Node.js
where node >nul 2>&1
if errorlevel 1 (
    echo ERROR: Node.js is not installed or not on PATH.
    echo        Download from https://nodejs.org/
    exit /b 1
)

REM Install dependencies if node_modules is missing
if not exist node_modules (
    echo [1/2] Installing npm dependencies...
    call npm install
    if errorlevel 1 (
        echo ERROR: npm install failed.
        exit /b 1
    )
) else (
    echo [1/2] npm dependencies already installed. Skipping npm install.
    echo        Run "npm install" manually if you need to refresh packages.
)

REM Compile TypeScript
echo [2/2] Compiling TypeScript...
call npm run compile
if errorlevel 1 (
    echo ERROR: TypeScript compilation failed.
    exit /b 1
)

echo.
echo === Build successful! ===
echo.
echo Next steps:
echo   launch.bat          - Open VSCode with the extension loaded (development mode)
echo   install.bat         - Package and install as a .vsix
echo.
endlocal
