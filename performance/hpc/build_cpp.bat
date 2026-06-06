@echo off
REM Build the C++ B+ tree reference with MSVC -O2.
REM Usage: performance\hpc\build_cpp.bat
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "out\perf" mkdir "out\perf"
call "%VCVARS%" x64 >nul 2>&1
cl /O2 /std:c++17 /EHsc /nologo performance\hpc\bptree.cpp /Feout\perf\bptree_cpp.exe /Foout\perf\
