@echo off
setlocal

set COMPILER=x64\Debug\MyCompiler.exe
set LLI=vcpkg_installed\x64-windows\x64-windows\tools\llvm\lli.exe
set SRC=MyCompiler\Test
set LIB=MyCompiler\Test\library
set OUT=out

set ERRORS=0

if not exist "%OUT%" mkdir "%OUT%"

call :RunTest testfile
call :RunTest testfile2
call :RunTest test_generics
call :RunTest testfile_module -i %LIB%
call :RunTest test_library_string -i %LIB%
call :RunTestCb test_operators
call :RunTestCb test_is_as

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

%COMPILER% %SRC%\%NAME%.c %EXTRA% -o %OUT%\%NAME%.ll
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

:RunTestCb
set NAME=%~1
set EXTRA=%~2 %~3
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.cb %EXTRA% -o %OUT%\%NAME%.ll
if %ERRORLEVEL% neq 0 (
    echo FAILED: compiler returned error %ERRORLEVEL% for %NAME%.cb
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
