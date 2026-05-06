@echo off
setlocal

if "%MYCOMPILER_CONFIG%"=="" set MYCOMPILER_CONFIG=Debug
set COMPILER=x64\%MYCOMPILER_CONFIG%\MyCompiler.exe
set SRC=Test
set LIB=Test\library
set GROUP=0

if "%~1"=="--group" set GROUP=%~2

set ERRORS=0

REM Group 1: statements, constraints, types, typeswitch, vararg, annotations
if "%GROUP%"=="0" goto :RunAll
if "%GROUP%"=="1" goto :Group1
if "%GROUP%"=="2" goto :Group2
if "%GROUP%"=="3" goto :Group3
goto :RunAll

:RunAll
call :Group1Tests
call :Group2Tests
call :Group3Tests
goto :Done

:Group1
call :Group1Tests
goto :Done

:Group2
call :Group2Tests
goto :Done

:Group3
call :Group3Tests
goto :Done

:Group1Tests
call :RunErrorTest err_statements.cb
call :RunErrorTest err_constraints.cb
call :RunErrorTest err_types.cb
call :RunErrorTest err_typeswitch_mixed.cb
call :RunErrorTest err_vararg_forward_not_variadic.cb
call :RunErrorTest err_annotations.cb
exit /b

:Group2Tests
call :RunErrorTest err_reflect.cb
call :RunErrorTest err_move.cb
call :RunErrorTest err_interface_pointer.cb
call :RunErrorTest err_declarations.cb
call :RunErrorTest err_variadic_not_last.cb
call :RunErrorTest err_move_return.cb
exit /b

:Group3Tests
call :RunErrorTest err_field_init_unknown.cb
call :RunErrorTest err_field_init_duplicate.cb
call :RunErrorTest err_field_init_arg_no_struct.cb
call :RunErrorTest err_field_init_arg_ambiguous.cb
call :RunErrorTest err_bond_lambda_escape.cb
for %%F in (%SRC%\errors\circular\entry_*.cb) do (
    call :RunCircularTest %%~nxF
)
exit /b

:Done
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
set CIRC_TMP=%TEMP%\circ_%~n1_out.txt
echo === circular\%CIRC_FILE% ===
%COMPILER% %SRC%\errors\circular\%CIRC_FILE% --nologo > %CIRC_TMP% 2>&1
findstr /i "Circular" %CIRC_TMP% > nul
if %ERRORLEVEL% equ 0 (
    echo PASS: circular\%CIRC_FILE%
) else (
    echo FAILED: circular\%CIRC_FILE% - expected "Circular import" in output
    type %CIRC_TMP%
    set /a ERRORS+=1
)
del /q %CIRC_TMP% 2>nul
exit /b
