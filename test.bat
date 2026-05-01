@echo off
setlocal EnableDelayedExpansion

REM set PATH=%CD%\vcpkg_installed\x64-windows-static\x64-windows-static\tools\llvm;%PATH%

REM ===========================================================================
REM Worker mode: compile and run a single .c test, write result file
REM All compiler/exe output goes to a log file; nothing printed to console.
REM ===========================================================================
if "%~1"=="--worker-c" (
    set NAME=%~2
    set COMPILER=x64\Debug\MyCompiler.exe
    set SRC=Test
    set OUT=out
    set T0=!TIME!
    !COMPILER! !SRC!\!NAME!.c -o !OUT!\!NAME!.exe --nologo --out-lli !OUT!\!NAME!.ll !MYCOMPILER_EXTRA! > "!OUT!\results\!NAME!.log" 2>&1
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
    set COMPILER=x64\Debug\MyCompiler.exe
    set SRC=Test
    set LIB=Test\library
    set OUT=out
    set T0=!TIME!
    !COMPILER! !SRC!\!NAME!.cb -i !LIB! -o !OUT!\!NAME!.exe --nologo --out-lli !OUT!\!NAME!.ll !MYCOMPILER_EXTRA! > "!OUT!\results\!NAME!.log" 2>&1
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
REM Worker mode: run test_err.bat, capture output to log file
REM ===========================================================================
if "%~1"=="--worker-err" (
    set OUT=out
    set T0=!TIME!
    call "%~dp0test_err.bat" > "!OUT!\results\test_err.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: test_err>"!OUT!\results\test_err.result"
    ) else (
        set T1=!TIME!
        for /f "tokens=1-4 delims=:." %%a in ("!T0: =0!") do set /a CS0=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
        for /f "tokens=1-4 delims=:." %%a in ("!T1: =0!") do set /a CS1=1%%a*360000+1%%b*6000+1%%c*100+1%%d-36610100
        set /a ECS=CS1-CS0
        if !ECS! lss 0 set /a ECS+=8640000
        set /a ES=ECS/100
        set /a EF=ECS-ES*100
        if !EF! lss 10 set EF=0!EF!
        echo PASS !ES!.!EF!s>"!OUT!\results\test_err.result"
    )
    exit /b
)

REM ===========================================================================
REM Main: launch all tests in parallel, wait, collect results
REM ===========================================================================
set COMPILER=x64\Debug\MyCompiler.exe
set SRC=Test
set LIB=Test\library
set OUT=out
set MYCOMPILER_EXTRA=%*
set EXCLUDE=test_helper
set SCRIPT=%~f0
set START_TIME=%TIME%

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\results" mkdir "%OUT%\results"
del /q "%OUT%\results\*.result" 2>nul
del /q "%OUT%\results\*.log" 2>nul

set /a LAUNCHED=0

REM Launch error tests first — they're slower and benefit most from early start
set /a LAUNCHED+=1
start "" /b cmd /c "%SCRIPT% --worker-err"

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
    if !WAITED! geq 120 (
        echo TIMEOUT: only !DONE! of !LAUNCHED! tests completed after 120s
        goto :Collect
    )
    ping -n 2 127.0.0.1 >nul 2>&1
    set /a WAITED+=1
    goto WaitLoop
)

:Collect
set /a ERRORS=0
for %%R in (%OUT%\results\*.result) do (
    set /p RESULT=<"%%R"
    if /I "!RESULT:~0,4!" neq "PASS" (
        echo.
        echo === %%~nR ===
        type "%%~dpnR.log"
        echo !RESULT!
        set /a ERRORS+=1
    ) else (
        set ELAPSED=!RESULT:~5!
        echo PASSED: %%~nR  [!ELAPSED!]
    )
)

echo.
call :ElapsedTime "%START_TIME%" "%TIME%"
if %ERRORS% EQU 0 (
    echo All tests passed.
    exit /b 0
) else (
    echo %ERRORS% tests failed.
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
