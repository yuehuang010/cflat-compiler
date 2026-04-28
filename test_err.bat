@echo off
setlocal

set COMPILER=x64\Debug\MyCompiler.exe
set SRC=Test
set LIB=Test\library

set ERRORS=0

for %%F in (%SRC%\errors\err_*.cb) do (
    call :RunErrorTest %%~nxF
)

echo.
if %ERRORS% EQU 0 (
    echo All error tests passed.
) else (
    echo %ERRORS% error tests failed.
    exit /b 1
)
exit /b 0

:RunErrorTest
set ERRFILE=%~1
echo === %ERRFILE% ===
%COMPILER% %SRC%\errors\%ERRFILE% -i %LIB% --nologo
if %ERRORLEVEL% neq 0 (
    echo FAILED: %ERRFILE%
    set /a ERRORS+=1
)
exit /b
