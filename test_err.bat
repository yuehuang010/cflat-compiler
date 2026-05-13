@echo off
setlocal EnableDelayedExpansion

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Debug
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
set SRC=Test
set LIB=Test\library
set GROUP=0

if "%~1"=="--group" set GROUP=%~2

set ERRORS=0

if "%GROUP%"=="0" goto :RunAll
if "%GROUP%"=="1" goto :Group1
if "%GROUP%"=="2" goto :Group2
if "%GROUP%"=="3" goto :Group3
if "%GROUP%"=="4" goto :Group4
goto :RunAll

:RunAll
call :Group1Tests
call :Group2Tests
call :Group3Tests
call :Group4Tests
goto :Done

:Group1
call :Group1Tests
goto :Done

:Group2
call :Group2Tests
goto :Done

:Group3
call :Group3Tests
goto :Done

:Group4
call :Group4Tests
goto :Done

REM Files are distributed round-robin across 4 groups by index modulo 4.
REM Adding a new err_*.cb is self-maintaining - no list updates needed.

:Group1Tests
call :RunModuloGroup 0 4
exit /b

:Group2Tests
call :RunModuloGroup 1 4
exit /b

:Group3Tests
call :RunModuloGroup 2 4
for %%F in (%SRC%\errors\circular\entry_*.cb) do (
    call :RunCircularTest %%~nxF
)
exit /b

:Group4Tests
call :RunModuloGroup 3 4
exit /b

:RunModuloGroup
REM Runs every err_*.cb whose index mod %~2 == %~1
set /a MOD_REM=%~1
set /a MOD_DIV=%~2
set /a CTR=0
for %%F in (%SRC%\errors\err_*.cb) do (
    set /a MOD=CTR %% MOD_DIV
    if !MOD!==!MOD_REM! call :RunErrorTest %%~nxF
    set /a CTR+=1
)
exit /b

:Done
echo.
if %ERRORS% EQU 0 (
    echo All error tests passed.
) else (
    echo %ERRORS% error tests failed.
    exit /b 1
)
exit /b 0

:RunErrorTest
set ERRFILE=%~1
echo === %ERRFILE% ===
%COMPILER% %SRC%\errors\%ERRFILE% -i %LIB% --nologo
if %ERRORLEVEL% neq 0 (
    echo FAILED: %ERRFILE%
    set /a ERRORS+=1
)
exit /b

:RunCircularTest
set CIRC_FILE=%~1
set CIRC_TMP=%TEMP%\circ_%~n1_out.txt
echo === circular\%CIRC_FILE% ===
%COMPILER% %SRC%\errors\circular\%CIRC_FILE% --nologo > %CIRC_TMP% 2>&1
findstr /i "Circular" %CIRC_TMP% > nul
if %ERRORLEVEL% equ 0 (
    echo PASS: circular\%CIRC_FILE%
) else (
    echo FAILED: circular\%CIRC_FILE% - expected "Circular import" in output
    type %CIRC_TMP%
    set /a ERRORS+=1
)
del /q %CIRC_TMP% 2>nul
exit /b
