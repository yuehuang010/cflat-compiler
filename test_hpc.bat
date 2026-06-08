@echo off
setlocal enabledelayedexpansion

REM HPC -O2 IR checks. Two related guarantees that test.bat (which compiles at -O0) cannot
REM exercise, since both only take effect once the loop vectorizer runs at -O2:
REM   1. the `vectorize` keyword - a marked loop must vectorize or the compile hard-errors;
REM   2. the span<T> noalias contract - a by-value span axpy must vectorize with NO runtime
REM      alias check (no vector.memcheck), proving the T[] field's scoped alias metadata works.

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
echo === SPAN NOALIAS: by-value span axpy must vectorize with NO runtime alias check ===
call :span_noalias

echo.
if !FAIL! NEQ 0 (
    echo FAILED: !FAIL! HPC check^(s^) failed.
    exit /b 1
)
echo All HPC checks passed.
exit /b 0

:span_noalias
REM The span<T> noalias guarantee: span_axpy passes two span<double> BY VALUE and indexes each
REM span's buffer through a local T[]. At -O2 the loop must vectorize with NO vector.memcheck -
REM the span's T[] _ptr field carries scoped alias metadata that proves distinct spans disjoint.
REM The `vectorize` keyword in the fixture also hard-errors if the loop fails to vectorize at all.
set SPAN_FIX=Test\span_noalias_fixture.cb
set SPAN_EXE=out\span_noalias_fixture.exe
set SPAN_LL=out\span_noalias_fixture.ll
del /q "%SPAN_EXE%" "%SPAN_LL%" 2>nul
"%COMPILER%" "%SPAN_FIX%" -i Test\library -o "%SPAN_EXE%" --out-lli "%SPAN_LL%" -O2 --cpu x86-64-v3
if errorlevel 1 (
    echo   FAILED: span fixture did not compile/vectorize at -O2
    set /a FAIL+=1
    exit /b 0
)
"%SPAN_EXE%"
if errorlevel 1 (
    echo   FAILED: span fixture produced a wrong result
    set /a FAIL+=1
    exit /b 0
)
REM Extract just the span_axpy function body and assert: it vectorizes, carries the array-view
REM alias.scope metadata, and contains NO vector.memcheck. (A whole-file grep would be confused by
REM memchecks in unrelated core functions, so the check is scoped to the function.)
pwsh -NoProfile -Command ^
  "$ll = Get-Content -Raw '%SPAN_LL%';" ^
  "$m = [regex]::Match($ll, '(?s)define[^\r\n]*@_span_axpy.*?\r?\n\}');" ^
  "if (-not $m.Success) { Write-Host '  FAILED: span_axpy not found in IR'; exit 1 }" ^
  "$b = $m.Value;" ^
  "if (-not ($b -match '<\d+ x double>'))  { Write-Host '  FAILED: span_axpy did not vectorize'; exit 1 }" ^
  "if (-not ($b -match 'alias\.scope'))    { Write-Host '  FAILED: span_axpy missing array-view alias.scope metadata'; exit 1 }" ^
  "if ($b -match 'memcheck')               { Write-Host '  FAILED: span_axpy emitted a runtime alias check (vector.memcheck) - noalias not effective'; exit 1 }" ^
  "Write-Host '  OK: span_axpy vectorized, alias-scoped, no memcheck'; exit 0"
if errorlevel 1 set /a FAIL+=1
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
