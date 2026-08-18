@echo off
setlocal EnableDelayedExpansion

REM set PATH=%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed\x64-windows-static\tools\llvm;%PATH%

REM ===========================================================================
REM Worker mode: compile and run a single .c test, write result file
REM All compiler/exe output goes to a log file; nothing printed to console.
REM ===========================================================================
if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
if not defined CFLAT_LOCALE_DIR set CFLAT_LOCALE_DIR=%~dp0cflat\locales

if "%~1"=="--worker-c" (
    set NAME=%~2
    set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
    set SRC=Test
    if not defined CFLAT_OUT set CFLAT_OUT=out
    set OUT=%CFLAT_OUT%
    if not defined CFLAT_PLATFORM_FLAG set CFLAT_PLATFORM_FLAG=
    set T0=!TIME!
    !COMPILER! !SRC!\!NAME!.c -o !OUT!\!NAME!.exe --nologo --out-lli !OUT!\!NAME!.ll --locale-dir "!CFLAT_LOCALE_DIR!" !CFLAT_PLATFORM_FLAG! !CFLAT_EXTRA! > "!OUT!\results\!NAME!.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: !NAME! - compiler error>"!OUT!\results\!NAME!.result"
        exit /b
    )
    !OUT!\!NAME!.exe >> "!OUT!\results\!NAME!.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: !NAME! - run error>"!OUT!\results\!NAME!.result"
        exit /b
    )
    set T1=!TIME!
    for /f "tokens=1-4 delims=:." %%a in ("!T0: =0!") do set /a CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    for /f "tokens=1-4 delims=:." %%a in ("!T1: =0!") do set /a CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    set /a ECS=CS1-CS0
    if !ECS! lss 0 set /a ECS+=8640000
    set /a ES=ECS/100
    set /a EF=ECS-ES*100
    if !EF! lss 10 set EF=0!EF!
    echo PASS !ES!.!EF!s>"!OUT!\results\!NAME!.result"
    exit /b
)

REM ===========================================================================
REM Worker mode: compile and run a single .cb test, write result file
REM ===========================================================================
if "%~1"=="--worker-cb" (
    set NAME=%~2
    set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
    set SRC=Test
    set LIB=Test\library
    if not defined CFLAT_OUT set CFLAT_OUT=out
    set OUT=%CFLAT_OUT%
    if not defined CFLAT_PLATFORM_FLAG set CFLAT_PLATFORM_FLAG=
    set T0=!TIME!
    !COMPILER! !SRC!\!NAME!.cb -i !LIB! --locale-dir "!CFLAT_LOCALE_DIR!" -o !OUT!\!NAME!.exe --nologo --out-lli !OUT!\!NAME!.ll !CFLAT_PLATFORM_FLAG! !CFLAT_EXTRA! > "!OUT!\results\!NAME!.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: !NAME! - compiler error>"!OUT!\results\!NAME!.result"
        exit /b
    )
    !OUT!\!NAME!.exe >> "!OUT!\results\!NAME!.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: !NAME! - run error>"!OUT!\results\!NAME!.result"
        exit /b
    )
    set T1=!TIME!
    for /f "tokens=1-4 delims=:." %%a in ("!T0: =0!") do set /a CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    for /f "tokens=1-4 delims=:." %%a in ("!T1: =0!") do set /a CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    set /a ECS=CS1-CS0
    if !ECS! lss 0 set /a ECS+=8640000
    set /a ES=ECS/100
    set /a EF=ECS-ES*100
    if !EF! lss 10 set EF=0!EF!
    echo PASS !ES!.!EF!s>"!OUT!\results\!NAME!.result"
    exit /b
)

REM ===========================================================================
REM Worker mode: run one group of error tests via test_err.bat --group N
REM ===========================================================================
if "%~1"=="--worker-err" (
    set GRP=%~2
    if not defined CFLAT_OUT set CFLAT_OUT=out
    set OUT=%CFLAT_OUT%
    set T0=!TIME!
    call "%~dp0test_err.bat" --group !GRP! > "!OUT!\results\test_err_!GRP!.log" 2>&1
    set ERR_GROUP_RC=!ERRORLEVEL!
    set ERR_POLICY_ONLY=0
    if !ERR_GROUP_RC! neq 0 (
        findstr /c:"POLICY_ONLY_FAILURES" "!OUT!\results\test_err_!GRP!.log" >nul
        if !ERRORLEVEL! equ 0 set ERR_POLICY_ONLY=1
    )
    if !ERR_GROUP_RC! neq 0 if !ERR_POLICY_ONLY! equ 0 (
        echo FAILED: test_err_!GRP!>"!OUT!\results\test_err_!GRP!.result"
        exit /b
    )
    set T1=!TIME!
    for /f "tokens=1-4 delims=:." %%a in ("!T0: =0!") do set /a CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    for /f "tokens=1-4 delims=:." %%a in ("!T1: =0!") do set /a CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    set /a ECS=CS1-CS0
    if !ECS! lss 0 set /a ECS+=8640000
    set /a ES=ECS/100
    set /a EF=ECS-ES*100
    if !EF! lss 10 set EF=0!EF!
    echo PASS !ES!.!EF!s>"!OUT!\results\test_err_!GRP!.result"
    exit /b
)

REM ===========================================================================
REM Worker mode: run the HPC -O2 IR checks via test_hpc.bat
REM ===========================================================================
if "%~1"=="--worker-hpc" (
    if not defined CFLAT_OUT set CFLAT_OUT=out
    set OUT=%CFLAT_OUT%
    set T0=!TIME!
    call "%~dp0test_hpc.bat" %CFLAT_CONFIG% > "!OUT!\results\hpc_o2_checks.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: hpc_o2_checks>"!OUT!\results\hpc_o2_checks.result"
        exit /b
    )
    set T1=!TIME!
    for /f "tokens=1-4 delims=:." %%a in ("!T0: =0!") do set /a CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    for /f "tokens=1-4 delims=:." %%a in ("!T1: =0!") do set /a CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
    set /a ECS=CS1-CS0
    if !ECS! lss 0 set /a ECS+=8640000
    set /a ES=ECS/100
    set /a EF=ECS-ES*100
    if !EF! lss 10 set EF=0!EF!
    echo PASS !ES!.!EF!s>"!OUT!\results\hpc_o2_checks.result"
    exit /b
)

REM ===========================================================================
REM Main: launch all tests in parallel, wait, collect results
REM ===========================================================================
set SRC=Test
set LIB=Test\library
if not defined CFLAT_PLATFORM set CFLAT_PLATFORM=win64
if "%CFLAT_PLATFORM%"=="win32" (set CFLAT_OUT=out32) else (set CFLAT_OUT=out)
set CFLAT_PLATFORM_FLAG=-p %CFLAT_PLATFORM%
set OUT=%CFLAT_OUT%
set _CFG_ARG=%~1
set CFLAT_EXTRA=%*
if /I "%_CFG_ARG%"=="Debug" (
    set CFLAT_CONFIG=Debug
    set CFLAT_EXTRA=%2 %3 %4 %5 %6 %7 %8 %9
) else if /I "%_CFG_ARG%"=="Release" (
    set CFLAT_CONFIG=Release
    set CFLAT_EXTRA=%2 %3 %4 %5 %6 %7 %8 %9
)
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
set EXCLUDE=test_helper
set TIMEOUT_SECS=600
set SCRIPT=%~f0
set START_TIME=%TIME%

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\results" mkdir "%OUT%\results"
del /q "%OUT%\results\*.result" 2>nul
del /q "%OUT%\results\*.log" 2>nul

REM Warm the compiler cache (linker paths + core-library bitcode) once up front so the
REM parallel worker compiles load bitcode instead of re-parsing the stdlib closure each time.
REM Use the compiler under test; --init-local must succeed - a failure is a real regression.
REM --init-local (not --init) so the cache lands in <exe dir>\.cflat: two worktrees or a
REM Debug/Release pair testing concurrently then cannot collide on one %USERPROFILE%\.cflat,
REM and the suite never overwrites the developer's own per-user cache.
"%COMPILER%" --init-local --nologo >"%OUT%\results\init.log" 2>&1
if errorlevel 1 (
    echo FAILED: cflat.exe --init-local
    type "%OUT%\results\init.log"
    exit /b 1
)

REM Probe --isolated once. The allow_all_ok sidecar supplies these exact flags.
REM A non-macOS host currently rejects --isolated before policy enforcement exists.
set CFLAT_POLICY_SUPPORTED=1
"%COMPILER%" "Test\errors\policy\err_policy_allow_all_ok.cb" -i "%LIB%" --locale-dir "%CFLAT_LOCALE_DIR%" --check --nologo --isolated Test/errors/policy/policies/deny_fsnet.json >"%OUT%\results\policy_probe.log" 2>&1
findstr /c:"policy-output-unsupported: --isolated is not supported on this host platform" "%OUT%\results\policy_probe.log" >nul
if not errorlevel 1 (
    set CFLAT_POLICY_SUPPORTED=0
    echo Policy tests skipped - --isolated is not supported on this host platform
)

REM Discover diagnostic templates from the complete error-test suite before launching
REM parallel workers. The discovery pass uses pseudo output so expect_error matches source text.
call "%~dp0test_err.bat" --discover --locale-dir "%CFLAT_LOCALE_DIR%" >"%OUT%\results\locale.log" 2>&1
if errorlevel 1 (
    echo FAILED: pseudo-locale diagnostic discovery
    type "%OUT%\results\locale.log"
    exit /b 1
)

REM Pre-build the C-interop fixture library so test_c_package.cb can bind it.
REM (Uses the clang-cl/lld-link deployed next to cflat.exe for this config.)
if exist "%SRC%\cinterop\build_mathlib.bat" (
    call "%SRC%\cinterop\build_mathlib.bat" %CFLAT_CONFIG% >nul 2>&1
    if errorlevel 1 echo WARNING: failed to build cinterop fixture lib - test_c_package may fail
)

set /a LAUNCHED=0

REM Launch the error tests as CFLAT_ERR_GROUPS parallel groups - files are distributed
REM round-robin across the groups by test_err.bat, which reads the same variable.
if not defined CFLAT_ERR_GROUPS set CFLAT_ERR_GROUPS=4
for /l %%G in (1,1,%CFLAT_ERR_GROUPS%) do (
    set /a LAUNCHED+=1
    start "" /b cmd /c "%SCRIPT% --worker-err %%G"
)

REM Launch the HPC -O2 IR checks (vectorize keyword + span noalias) as one worker.
set /a LAUNCHED+=1
start "" /b cmd /c "%SCRIPT% --worker-hpc"

for %%F in (%SRC%\test_*.c) do (
    call :IsExcluded %%~nF
    if not errorlevel 1 (
        set /a LAUNCHED+=1
        start "" /b cmd /c "%SCRIPT% --worker-c %%~nF"
    )
)

for %%F in (%SRC%\test_*.cb) do (
    call :IsExcluded %%~nF
    if not errorlevel 1 (
        set /a LAUNCHED+=1
        start "" /b cmd /c "%SCRIPT% --worker-cb %%~nF"
    )
)

REM Wait for all workers to finish (up to 120 seconds)
set /a WAITED=0
:WaitLoop
set /a DONE=0
for %%R in (%OUT%\results\*.result) do set /a DONE+=1
if !DONE! lss !LAUNCHED! (
    if !WAITED! geq !TIMEOUT_SECS! (
        echo TIMEOUT: only !DONE! of !LAUNCHED! tests completed after !TIMEOUT_SECS!s
        taskkill /f /im cflat.exe >nul 2>&1
        for %%F in (%OUT%\test_*.exe) do taskkill /f /im "%%~nxF" >nul 2>&1
        goto :Collect
    )
    ping -n 2 127.0.0.1 >nul 2>&1
    set /a WAITED+=1
    goto WaitLoop
)

REM Tooling regression: a static-local move must retain its sanitizer origin and DI record.
set TOOLING_NAME=static_local_tooling
set TOOLING_LOG=%OUT%\results\%TOOLING_NAME%.log
set TOOLING_EXE=%OUT%\%TOOLING_NAME%.exe
set TOOLING_LL=%OUT%\%TOOLING_NAME%.ll
"%COMPILER%" "%SRC%\test_function_ptr.cb" -i "%LIB%" -o "%TOOLING_EXE%" --out-lli "%TOOLING_LL%" --sanitize=ownership %CFLAT_PLATFORM_FLAG% >"%TOOLING_LOG%" 2>&1
if errorlevel 1 (
    echo FAILED: sanitizer probe did not compile >"%OUT%\results\%TOOLING_NAME%.result"
) else (
    "%TOOLING_EXE%" static-origin-check >>"%TOOLING_LOG%" 2>&1
    REM abort() traps via __fastfail, so the exit code is 0xC0000409 - negative to cmd's
    REM signed `if errorlevel`. Compare the value itself: any non-zero exit is a trap.
    set TOOLING_RC=!ERRORLEVEL!
    if !TOOLING_RC! equ 0 (
        echo FAILED: sanitizer probe did not trap >"%OUT%\results\%TOOLING_NAME%.result"
    ) else (
        findstr /c:"ownership violation: value moved at" "%TOOLING_LOG%" >nul
        if errorlevel 1 (
            echo FAILED: sanitizer probe lost the move origin >"%OUT%\results\%TOOLING_NAME%.result"
        ) else (
            findstr /c:".static.node.own_origin" "%TOOLING_LL%" >nul
            if errorlevel 1 (
                echo FAILED: static origin metadata is missing >"%OUT%\results\%TOOLING_NAME%.result"
            ) else (
                findstr /r /c:"name: .*node.*isLocal: true" "%TOOLING_LL%" >nul
                if errorlevel 1 (
                    echo FAILED: static-local debug metadata is missing >"%OUT%\results\%TOOLING_NAME%.result"
                ) else (
                    echo PASS 0.00s >"%OUT%\results\%TOOLING_NAME%.result"
                )
            )
        )
    )
)

:Collect
set /a ERRORS=0
set /a SKIPS=0
set FAILED_NAMES=
for %%R in (%OUT%\results\*.result) do (
    set /p RESULT=<"%%R"
    if /I "!RESULT:~0,4!"=="SKIP" (
        echo SKIPPED: %%~nR
        set /a SKIPS+=1
    ) else if /I "!RESULT:~0,4!" neq "PASS" (
        echo.
        echo === %%~nR ===
        type "%%~dpnR.log"
        echo !RESULT!
        set /a ERRORS+=1
        if !ERRORS! leq 3 (
            if defined FAILED_NAMES (
                set FAILED_NAMES=!FAILED_NAMES!, %%~nR
            ) else (
                set FAILED_NAMES=%%~nR
            )
        )
        if !ERRORS! equ 4 set FAILED_NAMES=!FAILED_NAMES!, ...
    ) else (
        set ELAPSED=!RESULT:~5!
        echo PASSED: %%~nR  [!ELAPSED!]
    )
)

echo.
call :ElapsedTime "%START_TIME%" "%TIME%"
if %ERRORS% EQU 0 (
    echo All tests passed. !SKIPS! tests skipped.
    exit /b 0
) else (
    echo %ERRORS% tests failed, !SKIPS! skipped. [!FAILED_NAMES!]
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

:IsExcluded
for %%E in (%EXCLUDE%) do (
    if /I "%~1"=="%%E" exit /b 1
)
exit /b 0
