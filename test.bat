@echo off
setlocal

set PATH=%CD%\vcpkg_installed\x64-windows-static\x64-windows-static\tools\llvm;%PATH%
set COMPILER=x64\Debug\MyCompiler.exe
set SRC=MyCompiler\Test
set LIB=MyCompiler\Test\library
set OUT=out


set ERRORS=0

if not exist "%OUT%" mkdir "%OUT%"

call :RunTest testfile
call :RunTestCb testfile2
call :RunTestCb test_generics
call :RunTestCb testfile_module -i %LIB%
call :RunTestCb test_library_string -i %LIB%
call :RunTestCb test_operators
call :RunTestCb test_is_as
call :RunTestCb test_core
call :RunTestCb test_core_string
call :RunTestCb test_filesystem

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
set EXTRA=%~2 %~3
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.c %EXTRA% -o %OUT%\%NAME%.exe --out-lli %OUT%\%NAME%.ll
if %ERRORLEVEL% neq 0 (
    echo FAILED: compiler returned error %ERRORLEVEL% for %NAME%.c
    set /a ERRORS+=1
    exit /b
)

%OUT%\%NAME%.exe
if %ERRORLEVEL% neq 0 (
    echo FAILED: %NAME%.exe returned error %ERRORLEVEL%
    set /a ERRORS+=1
    exit /b
)

echo PASSED: %NAME%
exit /b

:RunTestCb
set NAME=%~1
set EXTRA=%~2 %~3
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.cb %EXTRA% -o %OUT%\%NAME%.exe --out-lli %OUT%\%NAME%.ll
if %ERRORLEVEL% neq 0 (
    echo FAILED: compiler returned error %ERRORLEVEL% for %NAME%.cb
    set /a ERRORS+=1
    exit /b
)

%OUT%\%NAME%.exe
if %ERRORLEVEL% neq 0 (
    echo FAILED: %NAME%.exe returned error %ERRORLEVEL%
    set /a ERRORS+=1
    exit /b
)

echo PASSED: %NAME%
exit /b
