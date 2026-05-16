@echo off
setlocal

set SCRIPT_DIR=%~dp0
pushd "%SCRIPT_DIR%"

set TESTFILE=%1
if "%TESTFILE%"=="" set TESTFILE=test_basic.cb

echo Building Release...
msbuild cflat.slnx -p:Platform=x64 -p:Configuration=Release -v:minimal -nologo
if errorlevel 1 goto :end

echo Building Debug...
msbuild cflat.slnx -p:Platform=x64 -p:Configuration=Debug -v:minimal -nologo && (
    "%SCRIPT_DIR%x64\Debug\cflat.exe" --nologo Test\%TESTFILE% -o out\myapp.exe --out-lli out.ll -p win64
    if not errorlevel 1 out\myapp.exe
)

:end
popd
endlocal
