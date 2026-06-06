@echo off
setlocal enabledelayedexpansion

REM Enforced-vectorization tests for the `vectorize` keyword. These must run at
REM -O2 (the only level where the loop vectorizer runs, so the only level where
REM `vectorize` is enforced). test.bat compiles at -O0 and cannot exercise the
REM contract, hence this dedicated harness (mirrors test_debug_info.bat needing -g).

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
set FAIL=0

echo === POSITIVE: test_vectorize.cb must compile at -O2 ^(all loops vectorize^) ===
set POS_EXE=out\test_vectorize_o2.exe
del /q "%POS_EXE%" 2>nul
"%COMPILER%" Test\test_vectorize.cb -i Test\library -o "%POS_EXE%" -O2
if errorlevel 1 (
    echo   FAILED: a `vectorize` loop in test_vectorize.cb did not vectorize at -O2
    set /a FAIL+=1
) else (
    "%POS_EXE%"
    if errorlevel 1 (
        echo   FAILED: test_vectorize.cb ran with a wrong result
        set /a FAIL+=1
    ) else (
        echo   OK: positive fixture compiled, vectorized, and ran correctly
    )
)

echo.
echo === NEGATIVES: each must FAIL at -O2 with a specific, located reason ===
call :expect_fail "Test\vectorize\neg_call.cb"             "calls 'printf'"
call :expect_fail "Test\vectorize\neg_while_noncountable.cb" "no countable trip count"
call :expect_fail "Test\vectorize\neg_carried_dep.cb"      "loop-carried dependence"

echo.
if !FAIL! NEQ 0 (
    echo FAILED: !FAIL! vectorize check^(s^) failed.
    exit /b 1
)
echo All vectorize checks passed.
exit /b 0

:expect_fail
set SRC=%~1
set WANT=%~2
set LOG=out\vec_neg.log
"%COMPILER%" "%SRC%" -i Test\library -o out\vec_neg.exe -O2 > "%LOG%" 2>&1
if not errorlevel 1 (
    echo   FAILED: %SRC% compiled at -O2 but should have been rejected
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"could not be vectorized" "%LOG%" >nul
if errorlevel 1 (
    echo   FAILED: %SRC% was rejected but without the expected vectorize diagnostic
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"%WANT%" "%LOG%" >nul
if errorlevel 1 (
    echo   FAILED: %SRC% rejected but reason did not mention "%WANT%"
    set /a FAIL+=1
    exit /b 0
)
echo   OK: %SRC% rejected with the expected reason ^("%WANT%"^)
exit /b 0
