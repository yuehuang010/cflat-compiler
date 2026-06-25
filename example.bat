@echo off
REM Build all examples in example/ and subdirectories
REM Usage: example.bat [Release|Debug]

setlocal enabledelayedexpansion

set CONFIG=%1
if "!CONFIG!"=="" set CONFIG=Release

echo Building examples (!CONFIG!)...
echo.

set CFLAT=x64\!CONFIG!\cflat.exe
if not exist "!CFLAT!" (
    echo Error: !CFLAT! not found. Build the solution first.
    exit /b 1
)

set PASSED=0
set FAILED=0
set SKIPPED=0

REM Space-separated list of base filenames (without .cb) to skip. Library/helper
REM files (no int main()) that are only meant to be imported by sibling examples.
REM The example/vcpkg/ examples are run too - cflat auto-invokes vcpkg install to
REM pull their external packages (libcurl, SDL3, sqlite, zlib).
REM direct2d_demo imports the Win32 metadata winmd, which ships in the
REM Microsoft.Windows.SDK.Win32Metadata nuget package (not the system) and needs an
REM explicit -i to it; build it manually per the command in the file header.
set EXCLUDE=test_helper ui win32host http_parser http_response http_json http_server http_client router rest_server http_io direct2d_demo

REM Helper function to try compile and run
for /r example %%F in (*.cb) do (
    set FILE=%%F
    set FILENAME=%%~nxF
    set BASENAME=%%~nF

    call :IsExcluded "!BASENAME!"
    if errorlevel 1 (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else (
        set OUTDIR=out\examples
        if not exist "!OUTDIR!" mkdir "!OUTDIR!"

        set OUTFILE=!OUTDIR!\!FILENAME:.cb=!.exe
        set IMPORTED_DIRS=

        REM Determine import directories based on location
        if "!FILE:~0,22!"=="example\restAPI\" (
            set IMPORTED_DIRS=-i example/restAPI -i example/restAPI/network
        ) else if "!FILE:~0,15!"=="example\shell\" (
            set IMPORTED_DIRS=
        )

        REM Compile
        "!CFLAT!" "!FILE!" -o "!OUTFILE!" !IMPORTED_DIRS! >nul 2>&1

        if !errorlevel! equ 0 (
            set /a PASSED+=1
            echo PASS: !FILENAME!
        ) else (
            set /a FAILED+=1
            echo FAIL: !FILENAME!
            "!CFLAT!" "!FILE!" -o "!OUTFILE!" !IMPORTED_DIRS!
        )
    )
)

REM ── UI framework self-test + leak gate (S3) ──────────────────────────────────
REM The UI host examples ship deterministic headless self-tests that run under
REM redirected stdin (Terminal.init() fails -> selfTest path). Build each with the
REM HeapAudit leak oracle (--heap-audit), run it, and fail the suite on any
REM assertion failure (non-zero exit) OR any leak. This makes example.bat gate UI
REM BEHAVIOR and leak-cleanliness, not just compilation. app.cb/counter.cb are
REM not assertion suites (they always exit 0) but are included for the leak gate.
set OUTDIR=out\examples
set UI_SELFTESTS=example\ui\host.cb example\ui\boxes.cb example\ui\app.cb example\ui\counter.cb example\ui\counter_jsx.cb
for %%U in (%UI_SELFTESTS%) do (
    set STNAME=%%~nxU
    set STEXE=!OUTDIR!\%%~nU_st.exe
    set STLOG=!OUTDIR!\%%~nU_st.log
    "!CFLAT!" "%%U" --heap-audit -o "!STEXE!" >nul 2>&1
    if not exist "!STEXE!" (
        set /a FAILED+=1
        echo FAIL: !STNAME! ^(self-test build failed^)
    ) else (
        "!STEXE!" <nul >"!STLOG!" 2>&1
        set STRC=!errorlevel!
        findstr /C:"heap-audit: LEAK" "!STLOG!" >nul 2>&1
        if !errorlevel! equ 0 (
            set /a FAILED+=1
            echo FAIL: !STNAME! ^(heap-audit leak^)
        ) else if !STRC! neq 0 (
            set /a FAILED+=1
            echo FAIL: !STNAME! ^(self-test assertion, exit !STRC!^)
        ) else (
            set /a PASSED+=1
            echo PASS: !STNAME! ^(self-test + leak-clean^)
        )
    )
)

REM GUI offscreen GDI readback self-test: builds the Win32 demo with --heap-audit
REM and runs it headless (--selftest) - it paints the widget tree into a memory
REM bitmap via GdiCanvas and reads pixels back, so the GDI path is gated for
REM behavior AND leaks without ever opening a window.
set GUI_STEXE=!OUTDIR!\win32_boxes_st.exe
set GUI_STLOG=!OUTDIR!\win32_boxes_st.log
"!CFLAT!" "example\ui\win32_boxes.cb" --heap-audit -o "!GUI_STEXE!" >nul 2>&1
if not exist "!GUI_STEXE!" (
    set /a FAILED+=1
    echo FAIL: win32_boxes.cb ^(gui self-test build failed^)
) else (
    "!GUI_STEXE!" --selftest <nul >"!GUI_STLOG!" 2>&1
    set GSTRC=!errorlevel!
    findstr /C:"heap-audit: LEAK" "!GUI_STLOG!" >nul 2>&1
    if !errorlevel! equ 0 (
        set /a FAILED+=1
        echo FAIL: win32_boxes.cb ^(heap-audit leak^)
    ) else if !GSTRC! neq 0 (
        set /a FAILED+=1
        echo FAIL: win32_boxes.cb ^(gui self-test, exit !GSTRC!^)
    ) else (
        set /a PASSED+=1
        echo PASS: win32_boxes.cb ^(gui self-test + leak-clean^)
    )
)

echo.
echo Results: !PASSED! passed, !FAILED! failed, !SKIPPED! skipped
if !FAILED! gtr 0 exit /b 1
exit /b 0

:IsExcluded
for %%E in (%EXCLUDE%) do (
    if /I "%~1"=="%%E" exit /b 1
)
exit /b 0
