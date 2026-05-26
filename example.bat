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
REM files that aren't meant to be run standalone, plus examples that need
REM external prerequisites (e.g. get needs vcpkg-installed libcurl - run
REM example/curl/build.bat first).
set EXCLUDE=threadpool test_helper http_parser http_response http_json http_server http_client router rest_server get

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
        if "!FILE!"=="example\bitmap.cb" (
            set IMPORTED_DIRS=
        ) else if "!FILE!"=="example\stars.cb" (
            set IMPORTED_DIRS=
        ) else if "!FILE!"=="example\tetris.cb" (
            set IMPORTED_DIRS=
        ) else if "!FILE!"=="example\minesweeper.cb" (
            set IMPORTED_DIRS=
        ) else if "!FILE!"=="example\missile_defender.cb" (
            set IMPORTED_DIRS=
        ) else if "!FILE:~0,25!"=="example\threadpool\" (
            set IMPORTED_DIRS=-i example/threadpool
        ) else if "!FILE:~0,22!"=="example\restAPI\" (
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
