@echo off
setlocal EnableDelayedExpansion

set START_TIME=%TIME%
set /a OVERALL_ERRORS=0

REM ===========================================================================
REM Build and test a single configuration
REM Usage: call :RunConfig Debug|Release
REM ===========================================================================
goto :Main

:RunConfig
set CFG=%~1
echo.
echo =========================================================================
echo BUILD: %CFG%
echo =========================================================================
call "%~dp0cmake_build.bat" %CFG%
if errorlevel 1 (
    echo BUILD FAILED: %CFG%
    set /a OVERALL_ERRORS+=1
    exit /b 1
)

echo.
echo =========================================================================
echo TESTS [%CFG%]: test.bat
echo =========================================================================
set CFLAT_CONFIG=%CFG%
call "%~dp0test.bat"
if errorlevel 1 (
    echo TESTS FAILED: %CFG% test.bat
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo TESTS [%CFG%]: test_lsp.bat
echo =========================================================================
set CFLAT_CONFIG=%CFG%
call "%~dp0test_lsp.bat"
if errorlevel 1 (
    echo TESTS FAILED: %CFG% test_lsp.bat
    set /a OVERALL_ERRORS+=1
)

exit /b 0

:Main
call :RunConfig Release

echo.
echo =========================================================================
echo BUILD: vscode-extension
echo =========================================================================
call "%~dp0vscode-extension\build.bat"
if errorlevel 1 (
    echo BUILD FAILED: vscode-extension
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo PUBLISH: copying artifacts to drop\
echo =========================================================================
call :Publish
if errorlevel 1 (
    echo PUBLISH FAILED
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
call :ElapsedTime "%START_TIME%" "%TIME%"
if !OVERALL_ERRORS! EQU 0 (
    echo CI PASSED.
    exit /b 0
) else (
    echo CI FAILED: !OVERALL_ERRORS! stages failed.
    exit /b 1
)

:Publish
pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1"
exit /b %ERRORLEVEL%

:ElapsedTime
setlocal
set T0=%~1
set T1=%~2
for /f "tokens=1-4 delims=:." %%a in ("%T0: =0%") do set /a S0=1%%a*3600+1%%b*60+1%%c-366100
for /f "tokens=1-4 delims=:." %%a in ("%T1: =0%") do set /a S1=1%%a*3600+1%%b*60+1%%c-366100
set /a SECS=S1-S0
if !SECS! lss 0 set /a SECS+=86400
echo Elapsed: !SECS!s
endlocal
exit /b 0
