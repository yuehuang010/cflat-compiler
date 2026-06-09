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

echo === POSITIVE: test_vectorize.cb must compile at -O2 ^(all loops vectorize^) and run correctly ===
REM --run JIT-compiles at -O2 (enforcing the `vectorize` keyword) and executes in-process; its
REM exit code is the program's. A nonzero code means either the vectorize gate hard-errored or
REM the program returned a wrong result - the captured log disambiguates the two.
set POS_LOG=out\vec_pos.log
"%COMPILER%" Test\test_vectorize.cb -i Test\library --run -O2 --nologo > "%POS_LOG%" 2>&1
if not errorlevel 1 (
    echo   OK: positive fixture vectorized and ran correctly via --run
) else (
    findstr /C:"could not be vectorized" "%POS_LOG%" >nul && (
        echo   FAILED: a `vectorize` loop in test_vectorize.cb did not vectorize at -O2
    ) || (
        echo   FAILED: test_vectorize.cb ran with a wrong result under --run
    )
    set /a FAIL+=1
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
REM The fixture is compiled to IR (--out-lli) so the validator below can inspect it for
REM vector.memcheck / alias.scope; no exe is needed. This compile is also the -O2 vectorize
REM gate. Runtime correctness is then verified separately with --run (read-only JIT; cannot be
REM combined with --out-lli, so it is a distinct invocation).
set SPAN_FIX=Test\span_noalias_fixture.cb
set SPAN_LL=out\span_noalias_fixture.ll
del /q "%SPAN_LL%" 2>nul
"%COMPILER%" "%SPAN_FIX%" -i Test\library --out-lli "%SPAN_LL%" -O2 --cpu x86-64-v3
if errorlevel 1 (
    echo   FAILED: span fixture did not compile/vectorize at -O2
    set /a FAIL+=1
    exit /b 0
)
"%COMPILER%" "%SPAN_FIX%" -i Test\library --run -O2 --cpu x86-64-v3 --nologo
if errorlevel 1 (
    echo   FAILED: span fixture produced a wrong result under --run
    set /a FAIL+=1
    exit /b 0
)
REM Assert per function that span_axpy/chunk_axpy each vectorized, carry the array-view
REM alias.scope metadata, and contain NO vector.memcheck. The checks are scoped to each
REM function body (a whole-file grep would be confused by memchecks in unrelated core
REM functions). Validation is done by a small CFlat program rather than a shell script.
REM The validator is 'int main(int argc, char** argv)'; --run forwards the .ll path after
REM '--' as argv[1]. A nonzero exit means it failed to compile or an assertion failed.
set CHECK_SRC=Test\verify_span_noalias.cb
"%COMPILER%" "%CHECK_SRC%" -i Test\library --run --nologo -- "%SPAN_LL%"
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

REM Like :expect_fail but for diagnostics with a different prefix: checks the
REM compile failed and that BOTH a marker phrase (%2) and a reason phrase (%3)
REM appear. Used by the Detection-B memcheck case ("did not vectorize cleanly").
:expect_fail_msg
set SRC=%~1
set MARK=%~2
set WANT=%~3
set LOG=out\vec_neg.log
"%COMPILER%" "%SRC%" -i Test\library -o out\vec_neg.exe -O2 > "%LOG%" 2>&1
if not errorlevel 1 (
    echo   FAILED: %SRC% compiled at -O2 but should have been rejected
    set /a FAIL+=1
    exit /b 0
)
findstr /C:"%MARK%" "%LOG%" >nul
if errorlevel 1 (
    echo   FAILED: %SRC% rejected but without the expected marker "%MARK%"
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
