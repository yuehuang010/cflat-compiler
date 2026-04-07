@echo off
setlocal
cd /d "%~dp0"

echo === MyCompiler VSCode Extension - Package and Install ===
echo.

REM Check prerequisites
where node >nul 2>&1
if errorlevel 1 (
    echo ERROR: Node.js not found. Download from https://nodejs.org/
    exit /b 1
)
where code >nul 2>&1
if errorlevel 1 (
    echo ERROR: 'code' (VSCode CLI) not found on PATH.
    echo        Ensure VSCode is installed and "code" is added to PATH.
    echo        In VSCode: Ctrl+Shift+P -> "Shell Command: Install 'code' command in PATH"
    exit /b 1
)

REM Install dependencies
if not exist node_modules (
    echo [1/4] Installing npm dependencies...
    call npm install
    if errorlevel 1 ( echo ERROR: npm install failed. & exit /b 1 )
) else (
    echo [1/4] npm dependencies already installed.
)

REM Compile
echo [2/4] Compiling TypeScript...
call npm run compile
if errorlevel 1 ( echo ERROR: TypeScript compilation failed. & exit /b 1 )

REM Install vsce if not present
echo [3/4] Checking for vsce (VSCode Extension CLI)...
call npx vsce --version >nul 2>&1
if errorlevel 1 (
    echo        Installing @vscode/vsce...
    call npm install --save-dev @vscode/vsce
    if errorlevel 1 ( echo ERROR: Failed to install vsce. & exit /b 1 )
)

REM Package
echo [4/4] Packaging extension...
call npx vsce package --allow-missing-repository --no-git-tag-version 2>&1
if errorlevel 1 (
    echo ERROR: Packaging failed.
    exit /b 1
)

REM Find the generated .vsix file
for /f "delims=" %%f in ('dir /b /o-d *.vsix 2^>nul') do (
    set VSIX_FILE=%%f
    goto :found_vsix
)
echo ERROR: No .vsix file found after packaging.
exit /b 1

:found_vsix
echo.
echo Installing %VSIX_FILE% into VSCode...
call code --install-extension "%VSIX_FILE%"
if errorlevel 1 (
    echo ERROR: Extension install failed.
    exit /b 1
)

echo.
echo === Extension installed successfully! ===
echo   Name   : %VSIX_FILE%
echo   Reload VSCode (Ctrl+Shift+P -> "Developer: Reload Window") to activate.
echo.
echo TIP: After reloading, open a .cb file to activate the language server.
echo      Set mycompiler.executablePath in Settings if auto-detect doesn't find your build.
echo.
endlocal
