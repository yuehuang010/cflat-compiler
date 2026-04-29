@echo off
setlocal

set COMPILER=x64\Debug\MyCompiler.exe
set SRC=Test
set LIB=Test\library

set ERRORS=0

for %%F in (%SRC%\errors\err_*.cb) do (
    call :RunErrorTest %%~nxF
)

for %%F in (%SRC%\errors\circular\entry_*.cb) do (
    call :RunCircularTest %%~nxF
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

:RunCircularTest
set CIRC_FILE=%~1
echo === circular\%CIRC_FILE% ===
%COMPILER% %SRC%\errors\circular\%CIRC_FILE% --nologo > %TEMP%\circ_test_out.txt 2>&1
findstr /i "Circular" %TEMP%\circ_test_out.txt > nul
if %ERRORLEVEL% equ 0 (
    echo PASS: circular\%CIRC_FILE%
) else (
    echo FAILED: circular\%CIRC_FILE% - expected "Circular import" in output
    type %TEMP%\circ_test_out.txt
    set /a ERRORS+=1
)
exit /b
