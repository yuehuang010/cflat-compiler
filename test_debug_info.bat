@echo off
setlocal enabledelayedexpansion

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
set _CFG_ARG=%~1
if /I "%_CFG_ARG%"=="Debug" set CFLAT_CONFIG=Debug
if /I "%_CFG_ARG%"=="Release" set CFLAT_CONFIG=Release
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe

if not exist "%COMPILER%" (
    echo FAILED: compiler not found at %COMPILER%
    exit /b 1
)

if not exist out mkdir out
set FIXTURE=Test\debug_info_fixture.cb
set EXE=out\debug_info_fixture.exe
set PDB=out\debug_info_fixture.pdb
set LL=out\debug_info_fixture.ll

del /q "%EXE%" "%PDB%" "%LL%" 2>nul

echo === compile with -g ===
"%COMPILER%" "%FIXTURE%" -i Test\library -o "%EXE%" --out-lli "%LL%" -g
if errorlevel 1 (
    echo FAILED: compiler returned non-zero with -g
    exit /b 1
)

echo === run executable ===
"%EXE%"
if errorlevel 1 (
    echo FAILED: fixture exited non-zero, runtime behaviour changed under -g
    exit /b 1
)

echo === artifact checks ===
if not exist "%PDB%" (
    echo FAILED: PDB not produced - lld-link /DEBUG missing?
    exit /b 1
)
for %%S in ("%PDB%") do if %%~zS LSS 1024 (
    echo FAILED: PDB is suspiciously small ^(%%~zS bytes^)
    exit /b 1
)

echo === IR metadata checks ===
set FAIL=0
call :assert "CodeView module flag"            /C:"\"CodeView\", i32 1"
call :assert "DISubprogram for add"            /C:"linkageName: \"_add_int_intint_\""
call :assert "DISubprogram for sumPoint"       /C:"linkageName: \"_sumPoint_int_PointPtr_\""
call :assert "parameter DI (arg: 1)"           /R /C:"DILocalVariable.*arg: 1"
call :assert "parameter DI (arg: 2)"           /R /C:"DILocalVariable.*arg: 2"
call :assert "struct DI for Point"             /C:"tag: DW_TAG_structure_type, name: \"Point\""
call :assert "generic struct DI for Box__int"  /C:"tag: DW_TAG_structure_type, name: \"Box__int\""
call :assert "global DI"                       /C:"DIGlobalVariableExpression"
call :assert "real subroutine type"            /C:"DISubroutineType"

if !FAIL! NEQ 0 (
    echo.
    echo FAILED: !FAIL! debug-info assertion^(s^) missed.
    exit /b 1
)

echo.
echo All debug-info checks passed.
exit /b 0

:assert
set LABEL=%~1
shift
findstr %1 %2 %3 "%LL%" >nul
if errorlevel 1 goto :assert_fail
echo   OK: !LABEL!
exit /b 0
:assert_fail
echo   MISSING: !LABEL!
set /a FAIL+=1
exit /b 0
