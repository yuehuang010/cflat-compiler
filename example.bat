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
REM files (no int main()) that are only meant to be imported by sibling examples,
REM plus examples with external prerequisites (e.g. get pulls libcurl through
REM example/vcpkg/vcpkg.json - cflat auto-invokes vcpkg install, so it works
REM if vcpkg is available on the machine but is excluded from CI smoke runs).
set EXCLUDE=test_helper ui win32host http_parser http_response http_json http_server http_client router rest_server http_io

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

echo.
echo Results: !PASSED! passed, !FAILED! failed, !SKIPPED! skipped
if !FAILED! gtr 0 exit /b 1
exit /b 0

:IsExcluded
for %%E in (%EXCLUDE%) do (
    if /I "%~1"=="%%E" exit /b 1
)
exit /b 0
