@echo off
setlocal EnableDelayedExpansion

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Debug
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe
set PERF_SRC=performance
set OUT=out\perf
set SCRIPT=%~f0

REM Max seconds to wait for the parallel compile workers. Debug-compiler + -O2
REM builds of all benchmarks at once can take well over a minute on a cold run.
set TIMEOUT_SECS=180

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
echo  cflat Performance Suite
echo ===================================================
echo.

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\results" mkdir "%OUT%\results"
del /q "%OUT%\results\*.result" 2>nul
del /q "%OUT%\results\*.log" 2>nul

set BENCH_FILES=perf_alloc_throughput perf_peak_memory perf_container_stress perf_program_lifecycle perf_yes_compare perf_string_stream perf_json perf_stringbuilder perf_hashset_dict perf_threadpool

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
    if !WAITED! geq !TIMEOUT_SECS! (
        echo TIMEOUT: only !DONE! of !LAUNCHED! benchmarks compiled after !TIMEOUT_SECS!s
        goto :Collect
    )
    ping -n 2 127.0.0.1 >nul 2>&1
    set /a WAITED+=1
    goto WaitLoop
)

:Collect
set /a ERRORS=0
for %%F in (%BENCH_FILES%) do (
    REM Clear RESULT each iteration: a missing .result file leaves `set /p`
    REM untouched, which would otherwise inherit the previous loop's value
    REM and silently report an unfinished compile as PASS.
    set "RESULT="
    if exist "%OUT%\results\%%F.result" set /p RESULT=<"%OUT%\results\%%F.result"
    if /I "!RESULT:~0,4!"=="PASS" (
        echo   %%F.cb
    ) else (
        if not exist "%OUT%\results\%%F.result" (
            echo ERROR: %%F.cb did not finish compiling within !TIMEOUT_SECS!s
        ) else (
            echo ERROR: failed to compile %%F.cb
            if exist "%OUT%\results\%%F.log" type "%OUT%\results\%%F.log"
        )
        set /a ERRORS+=1
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

set CPP_HD_SRC=performance\perf_hashset_dict.cpp
set CPP_HD_EXE=out\perf\perf_hashset_dict_cpp.exe

echo Compiling %CPP_HD_SRC%...
cmd /c "%VCVARS% x64 > nul 2>&1 && cl /O2 /std:c++17 /EHsc /nologo %CPP_HD_SRC% /Fe%CPP_HD_EXE% /Fo%OUT%\ > %OUT%\results\perf_hashset_dict_cpp.log 2>&1"
if !ERRORLEVEL! neq 0 (
    echo ERROR: C++ compile failed:
    type %OUT%\results\perf_hashset_dict_cpp.log
    goto :Done
)
echo   perf_hashset_dict.cpp

echo.
%CPP_HD_EXE%

:Done
echo.
echo ===================================================
echo  Done.
echo ===================================================
exit /b 0
