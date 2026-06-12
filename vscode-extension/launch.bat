@echo off
setlocal
cd /d "%~dp0"

echo === cflat VSCode Extension - Development Launch ===
echo.

REM Check for VSCode
where code >nul 2>&1
if errorlevel 1 (
    echo ERROR: 'code' not found on PATH.
    echo        In VSCode: Ctrl+Shift+P -^> "Shell Command: Install 'code' command in PATH"
    exit /b 1
)

REM Check for Node.js
where node >nul 2>&1
if errorlevel 1 (
    echo ERROR: Node.js not found. Download from https://nodejs.org/
    exit /b 1
)

REM Build first if out/ doesn't exist or is older than src/
set NEEDS_BUILD=0
if not exist out\extension.js set NEEDS_BUILD=1
if not exist out\server.js set NEEDS_BUILD=1

if "%NEEDS_BUILD%"=="1" (
    echo Extension not built yet. Running build...
    call build.bat
    if errorlevel 1 ( echo Build failed. & exit /b 1 )
    echo.
)

REM Strip trailing backslash from extension path (VSCode rejects it)
set EXT_PATH=%~dp0
if "%EXT_PATH:~-1%"=="\" set EXT_PATH=%EXT_PATH:~0,-1%

REM Determine workspace folder to open (default to cflat source root)
set WORKSPACE=%~dp0..
if "%~1" neq "" set WORKSPACE=%~1

echo Launching VSCode with extension in development (experimental) mode...
echo   Extension path : %EXT_PATH%
echo   Workspace      : %WORKSPACE%
echo.

REM --extensionDevelopmentPath loads the extension without installing it.
REM VSCode opens a NEW "[Extension Development Host]" window with the extension active.
code --extensionDevelopmentPath="%EXT_PATH%" --new-window "%WORKSPACE%"

echo VSCode launched. A new window should appear momentarily.
echo.
echo Tips:
echo   - Open a .cb file to activate the language server.
echo   - View -^> Output -^> "cflat Language Server" to see server logs.
echo   - Press F5 inside the extension source to attach the debugger (port 6009).
echo   - Run build.bat and restart VSCode to pick up code changes.
echo.
endlocal
