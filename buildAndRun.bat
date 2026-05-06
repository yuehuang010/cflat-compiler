@echo off
setlocal

if "%MYCOMPILER_CONFIG%"=="" set MYCOMPILER_CONFIG=Debug

set TESTFILE=%1
if "%TESTFILE%"=="" set TESTFILE=test_basic.cb

msbuild MyCompiler.slnx -p:Platform=x64 -p:Configuration=%MYCOMPILER_CONFIG% -v:minimal && (
    C:\source\MyCompiler\x64\%MYCOMPILER_CONFIG%\MyCompiler.exe --nologo Test\%TESTFILE% -o out\myapp.exe --out-lli out.ll -p win64
    if not errorlevel 1 out\myapp.exe
)

endlocal
