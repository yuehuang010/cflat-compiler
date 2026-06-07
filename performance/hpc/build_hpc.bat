@echo off
REM Build and run the single-threaded HPC library suite (correctness + benchmark).
REM Usage: performance\hpc\build_hpc.bat [Debug|Release] [cpu]
REM   default config = Release, default cpu = x86-64-v3 (AVX2)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set CPU=%2
if "%CPU%"=="" set CPU=x86-64-v3

set CFLAT=x64\%CONFIG%\cflat.exe
if not exist "out\hpc" mkdir "out\hpc"

echo === Correctness (hpc_demo) ===
"%CFLAT%" performance\hpc\hpc_demo.cb -i performance\hpc -i cflat\core -o out\hpc\hpc_demo.exe -O2 --cpu %CPU%
if errorlevel 1 exit /b 1
out\hpc\hpc_demo.exe
if errorlevel 1 exit /b 1

echo.
echo === Benchmark (hpc_bench, --cpu %CPU%) ===
"%CFLAT%" performance\hpc\hpc_bench.cb -i performance\hpc -i cflat\core -o out\hpc\hpc_bench.exe -O2 --cpu %CPU%
if errorlevel 1 exit /b 1
out\hpc\hpc_bench.exe
if errorlevel 1 exit /b 1

echo.
echo === Solvers (CG + Jacobi over CSR) ===
"%CFLAT%" performance\hpc\solvers.cb -i performance\hpc -i cflat\core -o out\hpc\solvers.exe -O2 --cpu %CPU%
if errorlevel 1 exit /b 1
out\hpc\solvers.exe
if errorlevel 1 exit /b 1

echo.
echo === Dense factorization (Cholesky + LU) ===
"%CFLAT%" performance\hpc\factor.cb -i performance\hpc -i cflat\core -o out\hpc\factor.exe -O2 --cpu %CPU%
if errorlevel 1 exit /b 1
out\hpc\factor.exe
if errorlevel 1 exit /b 1

echo.
echo === Black-Scholes (closed-form + Monte-Carlo) ===
"%CFLAT%" performance\hpc\montecarlo.cb -i performance\hpc -i cflat\core -o out\hpc\montecarlo.exe -O2 --cpu %CPU%
if errorlevel 1 exit /b 1
out\hpc\montecarlo.exe
