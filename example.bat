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

REM Helper function to try compile and run
for /r example %%F in (*.cb) do (
    set FILE=%%F
    set FILENAME=%%~nxF

    REM Skip library/helper files that aren't meant to be run directly
    if /i "!FILENAME!"=="threadpool.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="test_helper.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="http_parser.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="http_response.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="http_json.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="http_server.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="http_client.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="router.cb" (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else if /i "!FILENAME!"=="rest_server.cb" (
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
