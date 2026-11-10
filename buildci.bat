@echo off
setlocal EnableDelayedExpansion

set START_TIME=%TIME%
set /a OVERALL_ERRORS=0

REM Per-stage wall time, like buildci.sh: each stage start closes the previous one. The table prints
REM at the end and one CSV line per run goes to scratch\buildci_timings.csv (Windows rows say win).
set "STAGE_NAME="
set "STAGE_TABLE="
set "STAGE_CSV="
set /a STAGE_T0=0

REM --nightly: LIBS runs tiers 1-3 (adds libtorch) instead of tier 1 only.
set LIBS_TIER=1
if /I "%~1"=="--nightly" set LIBS_TIER=3

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
call :StageClose BUILD
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
call :StageClose TESTS
echo =========================================================================
set CFLAT_CONFIG=%CFG%
call "%~dp0test.bat"
if errorlevel 1 (
    echo TESTS FAILED: %CFG% test.bat
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo LIBS [%CFG%]: test_libs.bat (tiers 1-%LIBS_TIER%)
call :StageClose LIBS
echo =========================================================================
set CFLAT_CONFIG=%CFG%
call "%~dp0test_libs.bat" %CFG% -t %LIBS_TIER%
if errorlevel 1 (
    echo LIBS FAILED: %CFG% test_libs.bat
    set /a OVERALL_ERRORS+=1
)

REM The LIBS stage above installs the test_libs vcpkg ports (test_libs/vcpkg.json).
echo.
echo =========================================================================
echo EXAMPLES [%CFG%]: test_example.bat
call :StageClose EXAMPLES
echo =========================================================================
call "%~dp0test_example.bat" %CFG%
if errorlevel 1 (
    echo EXAMPLES FAILED: %CFG% test_example.bat
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo TESTS [%CFG%]: test_lsp.bat
call :StageClose LSP
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
call :StageClose VSCODE
echo =========================================================================
call "%~dp0vscode-extension\build.bat" ci
if errorlevel 1 (
    echo BUILD FAILED: vscode-extension
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo PUBLISH: copying artifacts to drop\
call :StageClose PUBLISH
echo =========================================================================
call :Publish
if errorlevel 1 (
    echo PUBLISH FAILED
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
call :TimingReport
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

:NowSecs
for /f "tokens=1-3 delims=:." %%a in ("%TIME: =0%") do set /a NOWS=1%%a*3600+1%%b*60+1%%c-366100
exit /b 0

:StageClose
call :NowSecs
if defined STAGE_NAME (
    set /a STAGE_D=NOWS-STAGE_T0
    if !STAGE_D! lss 0 set /a STAGE_D+=86400
    set "STAGE_TABLE=!STAGE_TABLE! !STAGE_NAME!:!STAGE_D!s"
    set "STAGE_CSV=!STAGE_CSV!,!STAGE_NAME!=!STAGE_D!"
)
set "STAGE_NAME=%~1"
set /a STAGE_T0=NOWS
exit /b 0

:TimingReport
call :StageClose ""
echo Stage timings:
for %%s in (!STAGE_TABLE!) do echo   %%s
set "TR_HEAD="
for /f %%h in ('git -C "%~dp0." rev-parse --short HEAD 2^>nul') do set "TR_HEAD=%%h"
if not exist "%~dp0scratch" mkdir "%~dp0scratch"
>>"%~dp0scratch\buildci_timings.csv" echo %DATE% %TIME:~0,5%,!TR_HEAD!,win-tier%LIBS_TIER%!STAGE_CSV!
exit /b 0

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
