@echo off
setlocal

set COMPILER=x64\Debug\MyCompiler.exe
set LLI=vcpkg_installed\x64-windows\x64-windows\tools\llvm\lli.exe
set SRC=MyCompiler
set OUT=out

set ERRORS=0

if not exist "%OUT%" mkdir "%OUT%"

call :RunTest testfile
call :RunTest testfile2

echo.
if %ERRORS% EQU 0 (
    echo All tests passed.
) else (
    echo %ERRORS% tests failed.
    exit /b 1
)
exit /b 0

:RunTest
set NAME=%~1
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.c -o %OUT%\%NAME%.ll
if %ERRORLEVEL% neq 0 (
    echo FAILED: compiler returned error %ERRORLEVEL% for %NAME%.c
    set /a ERRORS+=1
    exit /b
)

%LLI% %OUT%\%NAME%.ll
if %ERRORLEVEL% neq 0 (
    echo FAILED: lli returned error %ERRORLEVEL% for %NAME%.ll
    set /a ERRORS+=1
    exit /b
)

echo PASSED: %NAME%
exit /b
