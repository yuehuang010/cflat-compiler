@echo off
setlocal

REM set PATH=%CD%\vcpkg_installed\x64-windows-static\x64-windows-static\tools\llvm;%PATH%
set COMPILER=x64\Debug\MyCompiler.exe
set SRC=Test
set LIB=Test\library
set OUT=out

set ERRORS=0

REM Tests excluded from the run (space-separated, no extensions)
set EXCLUDE=test_helper

if not exist "%OUT%" mkdir "%OUT%"

for %%F in (%SRC%\test_*.c) do (
    call :IsExcluded %%~nF
    if not errorlevel 1 call :RunTest %%~nF
)

for %%F in (%SRC%\test_*.cb) do (
    call :IsExcluded %%~nF
    if not errorlevel 1 call :RunTestCb %%~nF
)

for %%F in (%SRC%\errors\err_*.cb) do (
    call :RunErrorTest %%~nxF
)

echo.
if %ERRORS% EQU 0 (
    echo All tests passed.
) else (
    echo %ERRORS% tests failed.
    exit /b 1
)
exit /b 0

:IsExcluded
for %%E in (%EXCLUDE%) do (
    if /I "%~1"=="%%E" exit /b 1
)
exit /b 0

:RunTest
set NAME=%~1
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.c -o %OUT%\%NAME%.exe --nologo --out-lli %OUT%\%NAME%.ll
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

:RunErrorTest
set ERRFILE=%~1
echo === %ERRFILE% ===
%COMPILER% %SRC%\errors\%ERRFILE% -i %LIB% --nologo
if %ERRORLEVEL% neq 0 (
    echo FAILED: %ERRFILE%
    set /a ERRORS+=1
)
exit /b

:RunTestCb
set NAME=%~1
echo === %NAME% ===

%COMPILER% %SRC%\%NAME%.cb -i %LIB% -o %OUT%\%NAME%.exe --nologo --out-lli %OUT%\%NAME%.ll
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
