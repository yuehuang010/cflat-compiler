@echo off
setlocal EnableDelayedExpansion

set COMPILER=x64\Debug\MyCompiler.exe
set PERF_SRC=performance
set OUT=out\perf
set SCRIPT=%~f0

REM ===========================================================================
REM Worker mode: compile a single benchmark, write result file
REM ===========================================================================
if "%~1"=="--worker-compile" (
    set NAME=%~2
    !COMPILER! !PERF_SRC!\!NAME!.cb -i !PERF_SRC! -o !OUT!\!NAME!.exe --nologo -O2 > "!OUT!\results\!NAME!.log" 2>&1
    if !ERRORLEVEL! neq 0 (
        echo FAILED: !NAME! - compiler error>"!OUT!\results\!NAME!.result"
    ) else (
        echo PASS>"!OUT!\results\!NAME!.result"
    )
    exit /b
)

REM ===========================================================================
REM Main
REM ===========================================================================
echo ===================================================
echo  MyCompiler Performance Suite
echo ===================================================
echo.

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\results" mkdir "%OUT%\results"
del /q "%OUT%\results\*.result" 2>nul
del /q "%OUT%\results\*.log" 2>nul

set BENCH_FILES=perf_alloc_throughput perf_peak_memory perf_container_stress perf_program_lifecycle perf_yes_compare perf_json

REM Launch all compilations in parallel
set /a LAUNCHED=0
echo Compiling benchmarks...
for %%F in (%BENCH_FILES%) do (
    set /a LAUNCHED+=1
    start "" /b cmd /c "%SCRIPT% --worker-compile %%F"
)

REM Wait for all compile workers to finish (up to 60 seconds)
set /a WAITED=0
:WaitLoop
set /a DONE=0
for %%R in (%OUT%\results\*.result) do set /a DONE+=1
if !DONE! lss !LAUNCHED! (
    if !WAITED! geq 60 (
        echo TIMEOUT: only !DONE! of !LAUNCHED! benchmarks compiled after 60s
        goto :Collect
    )
    ping -n 2 127.0.0.1 >nul 2>&1
    set /a WAITED+=1
    goto WaitLoop
)

:Collect
set /a ERRORS=0
for %%F in (%BENCH_FILES%) do (
    set /p RESULT=<"%OUT%\results\%%F.result"
    if /I "!RESULT:~0,4!" neq "PASS" (
        echo ERROR: failed to compile %%F.cb
        type "%OUT%\results\%%F.log"
        set /a ERRORS+=1
    ) else (
        echo   %%F.cb
    )
)

if %ERRORS% gtr 0 (
    echo.
    echo %ERRORS% benchmarks failed to compile.
    exit /b 1
)

echo.
echo Running benchmarks...

for %%F in (%BENCH_FILES%) do (
    %OUT%\%%F.exe
    if !ERRORLEVEL! neq 0 (
        echo WARNING: %%F.exe exited with code !ERRORLEVEL!
    )
)

REM ===========================================================================
REM C++ comparison benchmark (compile + run)
REM ===========================================================================
echo.
echo ===================================================
echo  C++ Comparison Benchmark
echo ===================================================
echo.

set CPP_SRC=performance\perf_yes_compare.cpp
set CPP_EXE=out\perf\perf_yes_compare_cpp.exe
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist %VCVARS% (
    echo SKIP: vcvarsall.bat not found at %VCVARS%
    goto :Done
)

echo Compiling %CPP_SRC%...
cmd /c "%VCVARS% x64 > nul 2>&1 && cl /O2 /std:c++17 /EHsc /nologo %CPP_SRC% /Fe%CPP_EXE% /Fo%OUT%\ > %OUT%\results\perf_yes_compare_cpp.log 2>&1"
if !ERRORLEVEL! neq 0 (
    echo ERROR: C++ compile failed:
    type %OUT%\results\perf_yes_compare_cpp.log
    goto :Done
)
echo   perf_yes_compare.cpp

echo.
%CPP_EXE%

:Done
echo.
echo ===================================================
echo  Done.
echo ===================================================
exit /b 0
