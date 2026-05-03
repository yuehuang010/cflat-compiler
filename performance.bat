@echo off
setlocal EnableDelayedExpansion

set COMPILER=x64\Debug\MyCompiler.exe
set PERF_SRC=performance
set OUT=out\perf

echo ===================================================
echo  MyCompiler Performance Suite
echo ===================================================
echo.

if not exist "%OUT%" mkdir "%OUT%"

set FILES=perf_alloc_throughput perf_peak_memory perf_container_stress perf_program_lifecycle perf_yes_stdout perf_yes_channel

echo Compiling benchmarks...
for %%F in (%FILES%) do (
    echo   %%F.cb
    %COMPILER% %PERF_SRC%\%%F.cb -i %PERF_SRC% -o %OUT%\%%F.exe --nologo -O2
    if !ERRORLEVEL! neq 0 (
        echo.
        echo ERROR: failed to compile %%F.cb
        exit /b 1
    )
)

echo.
echo Running benchmarks...

for %%F in (%FILES%) do (
    %OUT%\%%F.exe
    if !ERRORLEVEL! neq 0 (
        echo WARNING: %%F.exe exited with code !ERRORLEVEL!
    )
)

echo.
echo ===================================================
echo  Done.
echo ===================================================
exit /b 0
