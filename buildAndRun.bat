@echo off
setlocal

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Debug

echo CFLAT_CONFIG=%CFLAT_CONFIG%

set TESTFILE=%1
if "%TESTFILE%"=="" set TESTFILE=test_basic.cb

msbuild cflat.slnx -p:Platform=x64 -p:Configuration=%CFLAT_CONFIG% -v:minimal -nologo && (
    C:\source\MyCompiler\x64\%CFLAT_CONFIG%\cflat.exe --nologo Test\%TESTFILE% -o out\myapp.exe --out-lli out.ll -p win64
    if not errorlevel 1 out\myapp.exe
)

endlocal
