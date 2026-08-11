@echo off
setlocal EnableDelayedExpansion

REM Launcher integration test driver.
REM Like test.bat, this script does not build. Run cmake_build.bat first so the
REM selected configuration contains cflat-launcher-test and its fake compiler.
REM Usage: test_launcher.bat [Debug|Release]

if "%CFLAT_CONFIG%"=="" set "CFLAT_CONFIG=Release"
if /I "%~1"=="Debug" set "CFLAT_CONFIG=Debug"
if /I "%~1"=="Release" set "CFLAT_CONFIG=Release"
if /I not "%CFLAT_CONFIG%"=="Debug" if /I not "%CFLAT_CONFIG%"=="Release" (
    echo Unknown config "%CFLAT_CONFIG%" - use Debug or Release
    exit /b 2
)

set "ROOT=%~dp0"
set "OUT=%ROOT%out-launcher"
set "RESULTS=%OUT%\results"
set "HARNESS=%ROOT%x64\%CFLAT_CONFIG%\cflat-launcher-test.exe"
set "LAUNCHER=%ROOT%x64\%CFLAT_CONFIG%\cflat-launcher.exe"
set "TEST_COMPILER=%ROOT%x64\%CFLAT_CONFIG%\launcher-test-compiler.exe"
set "LOG=%RESULTS%\launcher-integration.log"

if not exist "%HARNESS%" (
    echo ERROR: launcher test harness not found: "%HARNESS%"
    echo Build it with: cmake_build.bat %CFLAT_CONFIG%
    exit /b 1
)
if not exist "%LAUNCHER%" (
    echo ERROR: launcher not found: "%LAUNCHER%"
    echo Build it with: cmake_build.bat %CFLAT_CONFIG%
    exit /b 1
)
if not exist "%TEST_COMPILER%" (
    echo ERROR: launcher test compiler not found: "%TEST_COMPILER%"
    echo Build it with: cmake_build.bat %CFLAT_CONFIG%
    exit /b 1
)

if not exist "%RESULTS%" mkdir "%RESULTS%"
del /q "%LOG%" 2>nul

set "START_TIME=%TIME%"
echo Running cflat launcher integration suite [%CFLAT_CONFIG%]...
"%HARNESS%" "%LAUNCHER%" "%TEST_COMPILER%" > "%LOG%" 2>&1
set "RESULT=%ERRORLEVEL%"
type "%LOG%"

call :ElapsedTime "%START_TIME%" "%TIME%"
if "%RESULT%"=="0" (
    echo All launcher tests passed.
    exit /b 0
)
echo Launcher tests failed with exit code %RESULT%.
echo Full log: "%LOG%"
exit /b %RESULT%

:ElapsedTime
setlocal
set "T0=%~1"
set "T1=%~2"
for /f "tokens=1-4 delims=:." %%a in ("%T0: =0%") do set /a S0=1%%a*3600+1%%b*60+1%%c-366100
for /f "tokens=1-4 delims=:." %%a in ("%T1: =0%") do set /a S1=1%%a*3600+1%%b*60+1%%c-366100
set /a SECS=S1-S0
if !SECS! lss 0 set /a SECS+=86400
echo Elapsed: !SECS!s
endlocal
exit /b 0
