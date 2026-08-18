@echo off
setlocal EnableDelayedExpansion

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
if not defined CFLAT_LOCALE_DIR set CFLAT_LOCALE_DIR=%~dp0cflat\locales
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
set SRC=Test
set LIB=Test\library
set GROUP=0
if not defined CFLAT_OUT set CFLAT_OUT=out
set OUT=%CFLAT_OUT%
if not defined CFLAT_POLICY_SUPPORTED set CFLAT_POLICY_SUPPORTED=1

if "%~1"=="--group" set GROUP=%~2

if "%~1"=="--discover" goto :Discover

set ERRORS=0
set NORMAL_ERRORS=0
set POLICY_ERRORS=0

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
call :RunPolicyModuloGroup !GRP_REM! %GROUP_COUNT%
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
    %COMPILER% --check -i %LIB% --locale-dir "%CFLAT_LOCALE_DIR%" --nologo!GROUPFILES!
    if !ERRORLEVEL! neq 0 (
        set /a ERRORS+=1
        set /a NORMAL_ERRORS+=1
    )
)
exit /b

:RunPolicyModuloGroup
REM Policy files carry sidecars, so each matching file gets its own compiler invocation.
set /a POLICY_REM=%~1
set /a POLICY_DIV=%~2
set /a POLICY_CTR=0
set POLICY_FILES_FOUND=0
for %%F in (%SRC%\errors\policy\err_*.cb) do (
    if exist "%%F" (
        set /a POLICY_MOD=POLICY_CTR %% POLICY_DIV
        if !POLICY_MOD!==!POLICY_REM! (
            set POLICY_FILES_FOUND=1
            call :RunPolicyTest "%%F"
        )
        set /a POLICY_CTR+=1
    )
)
if !POLICY_FILES_FOUND! equ 1 echo === policy error group %~1 of %~2 ===
exit /b

:LoadPolicyFlags
set "POLICY_FILE=%~1"
set "POLICY_FLAGS="
set "POLICY_EXPECT_EXIT="
set "POLICY_EXPECT_OUTPUT="
for /f "usebackq tokens=1* delims==" %%A in ("!POLICY_FILE!.flags") do (
    if /I "%%A"=="flags" (
        if defined POLICY_FLAGS (
            set "POLICY_FLAGS=!POLICY_FLAGS! %%B"
        ) else (
            set "POLICY_FLAGS=%%B"
        )
    ) else if /I "%%A"=="expect_exit" (
        set "POLICY_EXPECT_EXIT=%%B"
    ) else if /I "%%A"=="expect_output" (
        set "POLICY_EXPECT_OUTPUT=%%B"
    )
)
exit /b

:RunPolicyTest
set "POLICY_FILE=%~1"
set "POLICY_NAME=%~n1"
set "POLICY_LOG=%OUT%\results\policy_!POLICY_NAME!.log"
set "POLICY_RESULT=%OUT%\results\policy_!POLICY_NAME!.result"
if /I "!CFLAT_POLICY_SUPPORTED!"=="0" (
    echo SKIP: policy\!POLICY_NAME!.cb - --isolated is not supported on this host
    echo SKIP: --isolated is not supported on this host>"!POLICY_LOG!"
    echo SKIP>"!POLICY_RESULT!"
    exit /b
)
call :LoadPolicyFlags "!POLICY_FILE!"
set "POLICY_T0=!TIME!"
%COMPILER% "!POLICY_FILE!" -i %LIB% --locale-dir "%CFLAT_LOCALE_DIR%" --check --nologo !POLICY_FLAGS! >"!POLICY_LOG!" 2>&1
set "POLICY_RC=!ERRORLEVEL!"
set "POLICY_PASS=0"
if /I "!POLICY_EXPECT_EXIT!"=="nonzero" (
    if !POLICY_RC! neq 0 if defined POLICY_EXPECT_OUTPUT (
        findstr /c:"!POLICY_EXPECT_OUTPUT!" "!POLICY_LOG!" >nul
        if !ERRORLEVEL! equ 0 set "POLICY_PASS=1"
    )
) else if !POLICY_RC! equ 0 (
    set "POLICY_PASS=1"
)
set "POLICY_T1=!TIME!"
for /f "tokens=1-4 delims=:." %%a in ("!POLICY_T0: =0!") do set /a POLICY_CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
for /f "tokens=1-4 delims=:." %%a in ("!POLICY_T1: =0!") do set /a POLICY_CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
set /a POLICY_ECS=POLICY_CS1-POLICY_CS0
if !POLICY_ECS! lss 0 set /a POLICY_ECS+=8640000
set /a POLICY_ES=POLICY_ECS/100
set /a POLICY_EF=POLICY_ECS-POLICY_ES*100
if !POLICY_EF! lss 10 set POLICY_EF=0!POLICY_EF!
if !POLICY_PASS! equ 1 (
    echo PASS !POLICY_ES!.!POLICY_EF!s>"!POLICY_RESULT!"
    echo PASS: policy\!POLICY_NAME!.cb
) else (
    echo FAILED: policy\!POLICY_NAME!.cb>"!POLICY_RESULT!"
    echo FAILED: policy\!POLICY_NAME!.cb
    set /a ERRORS+=1
    set /a POLICY_ERRORS+=1
)
exit /b

:Done
echo.
if %ERRORS% EQU 0 (
    echo All error tests passed.
) else (
    echo %ERRORS% error tests failed.
    if !NORMAL_ERRORS! EQU 0 if !POLICY_ERRORS! GTR 0 echo POLICY_ONLY_FAILURES
    exit /b 1
)
exit /b 0

:RunCircularTest
set CIRC_FILE=%~1
set CIRC_TMP=%TEMP%\circ_%~n1_out.txt
echo === circular\%CIRC_FILE% ===
%COMPILER% %SRC%\errors\circular\%CIRC_FILE% --locale-dir "%CFLAT_LOCALE_DIR%" --nologo > %CIRC_TMP% 2>&1
findstr /i "Circular" %CIRC_TMP% > nul
if %ERRORLEVEL% equ 0 (
    echo PASS: circular\%CIRC_FILE%
) else (
    echo FAILED: circular\%CIRC_FILE% - expected "Circular import" in output
    type %CIRC_TMP%
    set /a ERRORS+=1
    set /a NORMAL_ERRORS+=1
)
del /q %CIRC_TMP% 2>nul
exit /b

:Discover
set "DISCOVERY_FILES="
for %%F in (%SRC%\errors\err_*.cb) do set "DISCOVERY_FILES=!DISCOVERY_FILES! "%SRC%\errors\%%~nxF""
%COMPILER% --locale pseudo --update-locale en-pseudo --locale-dir "%CFLAT_LOCALE_DIR%" --check -i %LIB% --nologo !DISCOVERY_FILES!
if errorlevel 1 exit /b 1
if /I "!CFLAT_POLICY_SUPPORTED!"=="0" (
    for %%F in (%SRC%\errors\policy\err_*.cb) do if exist "%%F" echo SKIP: policy\%%~nF.cb - --isolated is not supported on this host
    exit /b 0
)
for %%F in (%SRC%\errors\policy\err_*.cb) do if exist "%%F" (
    call :LoadPolicyFlags "%%F"
    %COMPILER% --locale pseudo --update-locale en-pseudo --locale-dir "%CFLAT_LOCALE_DIR%" --check -i %LIB% --nologo !POLICY_FLAGS! "%%F"
    set "DISCOVERY_RC=!ERRORLEVEL!"
)
exit /b 0
