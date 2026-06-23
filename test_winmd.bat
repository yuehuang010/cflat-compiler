@echo off
setlocal EnableDelayedExpansion

REM ===========================================================================
REM test_winmd.bat - parse every .winmd on the machine through the WinMD reader.
REM
REM Enumerates the OS / Windows SDK metadata roots and validates that the reader
REM can decode all of them (parse-only, via `cflat.exe --check <file.winmd>`).
REM This is a reader stress test: it exercises real generics, delegates,
REM parameterized interfaces, and large union metadata that the small Test/
REM fixtures do not. It does NOT compile or run any program, and test.bat does
REM not call it - run it on demand.
REM
REM Files are batched into one `--check` invocation per chunk (a single process
REM reuses one backend across many files), so hundreds of .winmd validate in a
REM handful of processes rather than one process each.
REM
REM Usage:  test_winmd.bat [Debug|Release]   (default Release; CFLAT_CONFIG honored)
REM ===========================================================================

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=%CFLAT_CONFIG%
if "%CONFIG%"=="" set CONFIG=Release

set COMPILER=x64\%CONFIG%\cflat.exe
if not exist "%COMPILER%" (
    echo ERROR: compiler not found at %COMPILER% - build %CONFIG% first.
    exit /b 1
)

set OUT=out\winmd
if not exist "%OUT%" mkdir "%OUT%" >nul 2>&1
set LOG=%OUT%\check.log

REM Max files per `--check` invocation - keeps the command line well under the
REM ~32 KB limit (each path is ~100-150 chars).
set CHUNK=80

set /a TOTAL=0
set /a FAIL=0
set /a NINBATCH=0
set "ARGS="

echo Reader: %COMPILER%
echo Scanning metadata roots for .winmd ...
echo.

REM OS per-namespace metadata (Windows.Foundation, Windows.UI.Xaml, generics, ...).
call :scan "%WINDIR%\System32\WinMetadata"
REM SDK contract-split reference metadata (one .winmd per API contract / version).
call :scan "%ProgramFiles(x86)%\Windows Kits\10\References"
REM SDK union metadata (merged Windows.winmd views), if present.
call :scan "%ProgramFiles(x86)%\Windows Kits\10\UnionMetadata"
call :flush

set /a PASS=TOTAL-FAIL
echo.
echo ============================================================
echo  winmd parse:  !PASS! passed,  !FAIL! failed,  !TOTAL! total
echo ============================================================

if !TOTAL! equ 0 (
    echo ERROR: no .winmd files found in any metadata root.
    exit /b 1
)
if !FAIL! gtr 0 exit /b 1
exit /b 0

REM ---------------------------------------------------------------------------
REM :scan <dir> - queue every .winmd under <dir>, flushing each full chunk.
REM ---------------------------------------------------------------------------
:scan
set "DIR=%~1"
if not exist "%DIR%" goto :eof
for /r "%DIR%" %%F in (*.winmd) do (
    set /a TOTAL+=1
    set "ARGS=!ARGS! "%%F""
    set /a NINBATCH+=1
    if !NINBATCH! geq %CHUNK% call :flush
)
goto :eof

REM ---------------------------------------------------------------------------
REM :flush - run the queued chunk through one `--check`, tally + echo failures.
REM ---------------------------------------------------------------------------
:flush
if !NINBATCH! equ 0 goto :eof
"%COMPILER%" --check --nologo !ARGS! > "%LOG%" 2>&1
set CHUNKERR=!ERRORLEVEL!
REM `--check` returns non-zero iff some file in the chunk failed; the exit code is
REM authoritative. Only crack open the log (--nologo leaves just "FAIL:" lines) to
REM tally and print which files failed.
if !CHUNKERR! neq 0 (
    if exist "%LOG%" (
        for /f %%c in ('find /c "FAIL:" ^< "%LOG%"') do set /a FAIL+=%%c
        findstr /b /c:"FAIL:" "%LOG%"
        findstr /c:"Failed to read" "%LOG%"
    ) else (
        set /a FAIL+=1
        echo FAILED: a chunk produced no output ^(errorlevel !CHUNKERR!^)
    )
)
set "ARGS="
set /a NINBATCH=0
goto :eof
