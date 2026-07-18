@echo off
setlocal EnableDelayedExpansion

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
set SRC=Test
set LIB=Test\library
set GROUP=0

if "%~1"=="--group" set GROUP=%~2

set ERRORS=0

REM Files are distributed round-robin across GROUP_COUNT groups by index modulo
REM GROUP_COUNT. Adding a new err_*.cb is self-maintaining - no list updates needed.
REM test.bat exports CFLAT_ERR_GROUPS and launches exactly that many --worker-err workers.
if not defined CFLAT_ERR_GROUPS set CFLAT_ERR_GROUPS=4
set GROUP_COUNT=%CFLAT_ERR_GROUPS%

if "%GROUP%"=="0" goto :RunAll
if %GROUP% geq 1 if %GROUP% leq %GROUP_COUNT% goto :RunOneGroup
goto :RunAll

:RunAll
set /a G=1
:RunAllLoop
call :GroupTests !G!
set /a G+=1
if !G! leq %GROUP_COUNT% goto :RunAllLoop
goto :Done

:RunOneGroup
call :GroupTests %GROUP%
goto :Done

REM Group N checks the files whose index mod GROUP_COUNT == N-1. Group 1 also runs the
REM circular-import tests (separate single-file compiler invocations); it carries them
REM because group 1 is the only group guaranteed to exist for any CFLAT_ERR_GROUPS.
:GroupTests
set /a GRP_REM=%~1-1
call :RunModuloGroup !GRP_REM! %GROUP_COUNT%
if "%~1"=="1" (
    for %%F in (%SRC%\errors\circular\entry_*.cb) do (
        call :RunCircularTest %%~nxF
    )
)
exit /b

:RunModuloGroup
REM Checks every err_*.cb whose index mod %~2 == %~1 in a SINGLE compiler
REM invocation (--check) to amortize the per-process spawn cost. Each file is
REM compiled in its own fresh backend and emits no output; the batch exit code
REM is non-zero if any file failed its expect_error contract.
set /a MOD_REM=%~1
set /a MOD_DIV=%~2
set /a CTR=0
set "GROUPFILES="
for %%F in (%SRC%\errors\err_*.cb) do (
    set /a MOD=CTR %% MOD_DIV
    if !MOD!==!MOD_REM! set "GROUPFILES=!GROUPFILES! %SRC%\errors\%%~nxF"
    set /a CTR+=1
)
if defined GROUPFILES (
    echo === error group %~1 of %~2 ===
    %COMPILER% --check -i %LIB% --nologo!GROUPFILES!
    if !ERRORLEVEL! neq 0 set /a ERRORS+=1
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
