@echo off
setlocal

set SCRIPT_DIR=%~dp0
pushd "%SCRIPT_DIR%"

set TESTFILE=%1
if "%TESTFILE%"=="" set TESTFILE=test_basic.cb

echo Building Debug...
call "%SCRIPT_DIR%cmake_build.bat" debug
if errorlevel 1 goto :end

echo Building Release...
call "%SCRIPT_DIR%cmake_build.bat" release && (
    "%SCRIPT_DIR%x64\Release\cflat.exe" --nologo Test\%TESTFILE% -o out\myapp.exe -p win64
    if not errorlevel 1 out\myapp.exe
)

:end
popd
endlocal
