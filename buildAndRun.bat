@echo off
setlocal

set TESTFILE=%1
if "%TESTFILE%"=="" set TESTFILE=test_basic.cb

msbuild MyCompiler.slnx -p:Platform=x64 -p:Configuration=Debug -v:minimal && (
    C:\source\MyCompiler\x64\Debug\MyCompiler.exe --nologo Test\%TESTFILE% -o out\myapp.exe --out-lli out.ll -p win64
    if not errorlevel 1 out\myapp.exe
)

endlocal
