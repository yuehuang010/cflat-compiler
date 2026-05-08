@echo off
setlocal EnableDelayedExpansion

set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
set SOLUTION=cflat.slnx
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
%MSBUILD% %SOLUTION% -p:Configuration=%CFG% -p:Platform=x64 -v:minimal
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
call test.bat
if errorlevel 1 (
    echo TESTS FAILED: %CFG% test.bat
    set /a OVERALL_ERRORS+=1
)

echo.
echo =========================================================================
echo TESTS [%CFG%]: test_lsp.bat
echo =========================================================================
set CFLAT_CONFIG=%CFG%
call test_lsp.bat
if errorlevel 1 (
    echo TESTS FAILED: %CFG% test_lsp.bat
    set /a OVERALL_ERRORS+=1
)

exit /b 0

:Main
call :RunConfig Debug
call :RunConfig Release

echo.
echo =========================================================================
call :ElapsedTime "%START_TIME%" "%TIME%"
if %OVERALL_ERRORS% EQU 0 (
    echo CI PASSED.
    exit /b 0
) else (
    echo CI FAILED: %OVERALL_ERRORS% stage(s) failed.
    exit /b 1
)

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
