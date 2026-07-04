@echo off
REM Build all examples in example/ and subdirectories - in parallel.
REM Usage: example.bat [Release|Debug]
REM
REM Mirrors test.bat: worker-mode blocks (below) each build/gate one item and
REM write a .result file; the main body fans them all out with `start /b`, then
REM waits and collects. CONFIG / WIN32MD are passed to workers via the inherited
REM environment (child cmd processes inherit the parent's setlocal variables).

setlocal enabledelayedexpansion

REM ===========================================================================
REM Worker mode: compile a single example .cb, write its result file
REM ===========================================================================
if "%~1"=="--worker-example" (
    set "FILE=%~2"
    set "RESID=%~3"
    set "CFLAT=x64\%CONFIG%\cflat.exe"
    set "OUTDIR=out\examples"
    set "RESDIR=out\examples\results"
    for %%A in ("!FILE!") do (set "FILENAME=%%~nxA" & set "BASENAME=%%~nA")
    set "OUTFILE=!OUTDIR!\!FILENAME:.cb=!.exe"

    REM Determine import directories based on location (matches the legacy loop).
    set IMPORTED_DIRS=
    if "!FILE:~0,22!"=="example\restAPI\" (
        set IMPORTED_DIRS=-i example/restAPI -i example/restAPI/network
    ) else if "!FILE:~0,15!"=="example\shell\" (
        set IMPORTED_DIRS=
    ) else if /I "!BASENAME!"=="direct2d_demo" (
        set IMPORTED_DIRS=-i "!WIN32MD!"
    )

    "!CFLAT!" "!FILE!" -o "!OUTFILE!" !IMPORTED_DIRS! > "!RESDIR!\!RESID!.log" 2>&1
    if !errorlevel! equ 0 (
        echo PASS !FILENAME!>"!RESDIR!\!RESID!.result"
    ) else (
        echo FAIL !FILENAME!>"!RESDIR!\!RESID!.result"
    )
    exit /b
)

REM ===========================================================================
REM Worker mode: UI self-test - build with --heap-audit, run headless under
REM redirected stdin, gate on assertion exit code AND heap-audit leaks.
REM ===========================================================================
if "%~1"=="--worker-ui" (
    set "FILE=%~2"
    set "RESID=%~3"
    set "CFLAT=x64\%CONFIG%\cflat.exe"
    set "OUTDIR=out\examples"
    set "RESDIR=out\examples\results"
    for %%A in ("!FILE!") do (set "STNAME=%%~nxA" & set "STBASE=%%~nA")
    set "STEXE=!OUTDIR!\!STBASE!_st.exe"
    set "STLOG=!RESDIR!\!RESID!.log"

    "!CFLAT!" "!FILE!" --heap-audit -o "!STEXE!" > "!STLOG!" 2>&1
    if not exist "!STEXE!" (
        echo FAIL !STNAME! ^(self-test build failed^)>"!RESDIR!\!RESID!.result"
        exit /b
    )
    "!STEXE!" <nul >> "!STLOG!" 2>&1
    set STRC=!errorlevel!
    findstr /C:"heap-audit: LEAK" "!STLOG!" >nul 2>&1
    if !errorlevel! equ 0 (
        echo FAIL !STNAME! ^(heap-audit leak^)>"!RESDIR!\!RESID!.result"
    ) else if !STRC! neq 0 (
        echo FAIL !STNAME! ^(self-test assertion, exit !STRC!^)>"!RESDIR!\!RESID!.result"
    ) else (
        echo PASS !STNAME! ^(self-test + leak-clean^)>"!RESDIR!\!RESID!.result"
    )
    exit /b
)

REM ===========================================================================
REM Worker mode: GUI offscreen GDI readback self-test (win32_boxes --selftest).
REM ===========================================================================
if "%~1"=="--worker-gui" (
    set "RESID=%~2"
    set "CFLAT=x64\%CONFIG%\cflat.exe"
    set "OUTDIR=out\examples"
    set "RESDIR=out\examples\results"
    set "GUI_STEXE=!OUTDIR!\win32_boxes_st.exe"
    set "GUI_STLOG=!RESDIR!\!RESID!.log"

    "!CFLAT!" "example\ui\win32_boxes.cb" --heap-audit -o "!GUI_STEXE!" > "!GUI_STLOG!" 2>&1
    if not exist "!GUI_STEXE!" (
        echo FAIL win32_boxes.cb ^(gui self-test build failed^)>"!RESDIR!\!RESID!.result"
        exit /b
    )
    "!GUI_STEXE!" --selftest <nul >> "!GUI_STLOG!" 2>&1
    set GSTRC=!errorlevel!
    findstr /C:"heap-audit: LEAK" "!GUI_STLOG!" >nul 2>&1
    if !errorlevel! equ 0 (
        echo FAIL win32_boxes.cb ^(heap-audit leak^)>"!RESDIR!\!RESID!.result"
    ) else if !GSTRC! neq 0 (
        echo FAIL win32_boxes.cb ^(gui self-test, exit !GSTRC!^)>"!RESDIR!\!RESID!.result"
    ) else (
        echo PASS win32_boxes.cb ^(gui self-test + leak-clean^)>"!RESDIR!\!RESID!.result"
    )
    exit /b
)

REM ===========================================================================
REM Worker mode: native-controls host state-assert self-test
REM (win32_native_settings --selftest: invisible window, driven via SendMessage).
REM ===========================================================================
if "%~1"=="--worker-native" (
    set "RESID=%~2"
    set "CFLAT=x64\%CONFIG%\cflat.exe"
    set "OUTDIR=out\examples"
    set "RESDIR=out\examples\results"
    set "NAT_STEXE=!OUTDIR!\win32_native_settings_st.exe"
    set "NAT_STLOG=!RESDIR!\!RESID!.log"

    "!CFLAT!" "example\ui\win32_native_settings.cb" --heap-audit -o "!NAT_STEXE!" > "!NAT_STLOG!" 2>&1
    if not exist "!NAT_STEXE!" (
        echo FAIL win32_native_settings.cb ^(native self-test build failed^)>"!RESDIR!\!RESID!.result"
        exit /b
    )
    "!NAT_STEXE!" --selftest <nul >> "!NAT_STLOG!" 2>&1
    set NSTRC=!errorlevel!
    findstr /C:"heap-audit: LEAK" "!NAT_STLOG!" >nul 2>&1
    if !errorlevel! equ 0 (
        echo FAIL win32_native_settings.cb ^(heap-audit leak^)>"!RESDIR!\!RESID!.result"
    ) else if !NSTRC! neq 0 (
        echo FAIL win32_native_settings.cb ^(native self-test, exit !NSTRC!^)>"!RESDIR!\!RESID!.result"
    ) else (
        echo PASS win32_native_settings.cb ^(native self-test + leak-clean^)>"!RESDIR!\!RESID!.result"
    )
    exit /b
)

REM ===========================================================================
REM Worker mode: fedit editor state-assert self-test (the P4 flagship).
REM ===========================================================================
if "%~1"=="--worker-fedit" (
    set "RESID=%~2"
    set "CFLAT=x64\%CONFIG%\cflat.exe"
    set "OUTDIR=out\examples"
    set "RESDIR=out\examples\results"
    set "FED_STEXE=!OUTDIR!\fedit_st.exe"
    set "FED_STLOG=!RESDIR!\!RESID!.log"

    "!CFLAT!" "example\ui\fedit\fedit.cb" -i example/ui --heap-audit -o "!FED_STEXE!" > "!FED_STLOG!" 2>&1
    if not exist "!FED_STEXE!" (
        echo FAIL fedit.cb ^(fedit self-test build failed^)>"!RESDIR!\!RESID!.result"
        exit /b
    )
    "!FED_STEXE!" --selftest <nul >> "!FED_STLOG!" 2>&1
    set FEDRC=!errorlevel!
    findstr /C:"heap-audit: LEAK" "!FED_STLOG!" >nul 2>&1
    if !errorlevel! equ 0 (
        echo FAIL fedit.cb ^(heap-audit leak^)>"!RESDIR!\!RESID!.result"
    ) else if !FEDRC! neq 0 (
        echo FAIL fedit.cb ^(fedit self-test, exit !FEDRC!^)>"!RESDIR!\!RESID!.result"
    ) else (
        echo PASS fedit.cb ^(editor self-test + leak-clean^)>"!RESDIR!\!RESID!.result"
    )
    exit /b
)

REM ===========================================================================
REM Main: launch all example builds + self-tests in parallel, wait, collect
REM ===========================================================================
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

echo Building examples (%CONFIG%) in parallel...
echo.

set CFLAT=x64\%CONFIG%\cflat.exe
if not exist "%CFLAT%" (
    echo Error: %CFLAT% not found. Build the solution first.
    exit /b 1
)

set SCRIPT=%~f0
set OUTDIR=out\examples
set RESDIR=out\examples\results
set TIMEOUT_SECS=900
set PASSED=0
set FAILED=0
set SKIPPED=0
set /a LAUNCHED=0
set /a RESID=0
set START_TIME=%TIME%

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%RESDIR%" mkdir "%RESDIR%"
del /q "%RESDIR%\*.result" 2>nul
del /q "%RESDIR%\*.log" 2>nul

REM Space-separated list of base filenames (without .cb) to skip. Library/helper
REM files (no int main()) that are only meant to be imported by sibling examples.
REM The example/vcpkg/ examples are run too - cflat auto-invokes vcpkg install to
REM pull their external packages (libcurl, SDL3, sqlite, zlib).
REM direct2d_demo imports the Win32 metadata winmd, which ships in the
REM Microsoft.Windows.SDK.Win32Metadata nuget package (not the system), so it needs an
REM explicit -i to that package. We discover it below and compile the demo when present;
REM on a host without the package it is skipped.
REM example/macos/* target Darwin (dlopen of AppKit, sysctl, libproc) and are
REM excluded from this Windows run; compile them on a Mac instead.
set EXCLUDE=test_helper ui ui_native win32host win32_native_host fedit http_parser http_response http_json http_server http_client router rest_server http_io cocoa hello_objc cocoa_window sysinfo_mac

REM Discover the newest cached Win32-metadata package dir (the one holding Windows.Win32.winmd).
REM dir /o-n lists newest-version-first by name. Empty if the nuget package is not installed
REM for this user, in which case direct2d_demo is skipped rather than failing on the import.
set "WIN32MD="
set "WIN32MD_ROOT=%USERPROFILE%\.nuget\packages\microsoft.windows.sdk.win32metadata"
for /f "delims=" %%D in ('dir /b /ad /o-n "!WIN32MD_ROOT!" 2^>nul') do (
    if not defined WIN32MD if exist "!WIN32MD_ROOT!\%%D\Windows.Win32.winmd" set "WIN32MD=!WIN32MD_ROOT!\%%D"
)
if not defined WIN32MD set EXCLUDE=%EXCLUDE% direct2d_demo

REM Warm the compiler cache (linker paths + core-library bitcode) once up front so the
REM parallel worker compiles load bitcode instead of re-parsing the stdlib closure each time.
"%CFLAT%" --init --nologo >"%RESDIR%\init.log" 2>&1
if errorlevel 1 (
    echo FAILED: cflat.exe --init
    type "%RESDIR%\init.log"
    exit /b 1
)

REM Launch one worker per example. Excluded files are skipped up front (counted
REM here, not in a worker) so the SKIP lines print in launch order.
for /r example %%F in (*.cb) do (
    set "FILE=%%F"
    set "BASENAME=%%~nF"
    call :IsExcluded "!BASENAME!"
    if errorlevel 1 (
        set /a SKIPPED+=1
        echo SKIP: !FILE!
    ) else (
        set /a RESID+=1
        set /a LAUNCHED+=1
        start "" /b cmd /c "%SCRIPT% --worker-example %%F ex_!RESID!"
    )
)

REM Launch the UI framework self-tests (behavior + leak gate). app.cb/counter.cb
REM are not assertion suites (they always exit 0) but are included for the leak gate.
set UI_SELFTESTS=example\ui\host.cb example\ui\boxes.cb example\ui\app.cb example\ui\counter.cb example\ui\counter_jsx.cb
for %%U in (%UI_SELFTESTS%) do (
    set /a RESID+=1
    set /a LAUNCHED+=1
    start "" /b cmd /c "%SCRIPT% --worker-ui %%U ex_!RESID!"
)

REM Launch the GUI offscreen GDI readback self-test.
set /a RESID+=1
set /a LAUNCHED+=1
start "" /b cmd /c "%SCRIPT% --worker-gui ex_!RESID!"

REM Launch the native-controls host state-assert self-test.
set /a RESID+=1
set /a LAUNCHED+=1
start "" /b cmd /c "%SCRIPT% --worker-native ex_!RESID!"

REM Launch the fedit editor state-assert self-test (P4 flagship).
set /a RESID+=1
set /a LAUNCHED+=1
start "" /b cmd /c "%SCRIPT% --worker-fedit ex_!RESID!"

REM Wait for all workers to finish (up to TIMEOUT_SECS).
set /a WAITED=0
:WaitLoop
set /a DONE=0
for %%R in (%RESDIR%\*.result) do set /a DONE+=1
if !DONE! lss !LAUNCHED! (
    if !WAITED! geq !TIMEOUT_SECS! (
        echo TIMEOUT: only !DONE! of !LAUNCHED! examples completed after !TIMEOUT_SECS!s
        taskkill /f /im cflat.exe >nul 2>&1
        for %%F in (%OUTDIR%\*_st.exe) do taskkill /f /im "%%~nxF" >nul 2>&1
        goto :Collect
    )
    ping -n 2 127.0.0.1 >nul 2>&1
    set /a WAITED+=1
    goto WaitLoop
)

:Collect
for %%R in (%RESDIR%\*.result) do (
    set "LINE="
    set /p LINE=<"%%R"
    if /I "!LINE:~0,4!"=="PASS" (
        set /a PASSED+=1
        echo PASS: !LINE:~5!
    ) else (
        set /a FAILED+=1
        echo.
        echo === FAIL: !LINE:~5! ===
        type "%%~dpnR.log"
    )
)

REM Any launched worker with no .result did not complete (timeout) - count as failed.
set /a ACCOUNTED=PASSED+FAILED
if !ACCOUNTED! lss !LAUNCHED! (
    set /a MISSING=LAUNCHED-ACCOUNTED
    set /a FAILED+=MISSING
    echo FAIL: !MISSING! example^(s^) did not complete ^(timeout^)
)

echo.
call :ElapsedTime "%START_TIME%" "%TIME%"
echo Results: !PASSED! passed, !FAILED! failed, !SKIPPED! skipped
if !FAILED! gtr 0 exit /b 1
exit /b 0

:ElapsedTime
setlocal
set T0=%~1
set T1=%~2
for /f "tokens=1-4 delims=:." %%a in ("%T0: =0%") do set /a S0=1%%a*3600+1%%b*60+1%%c-366100
for /f "tokens=1-4 delims=:." %%a in ("%T1: =0%") do set /a S1=1%%a*3600+1%%b*60+1%%c-366100
set /a SECS=S1-S0
if !SECS! lss 0 set /a SECS+=86400
echo Elapsed: !SECS!s
endlocal
exit /b 0

:IsExcluded
for %%E in (%EXCLUDE%) do (
    if /I "%~1"=="%%E" exit /b 1
)
exit /b 0
