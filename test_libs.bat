@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "REPO=%~dp0"
set "REPO_NS=%REPO:~0,-1%"
pushd "%REPO%"

set "CFG=%CFLAT_CONFIG%"
if not defined CFG set "CFG=Release"
set "MAX_TIER=1"
set "VCPKG_TRIPLET=x64-windows"
set "JOBS=4"
set "STRICT=0"
set "INCLUDE_DISABLED=0"
set "WARM=0"
set "LIST=0"
set "FILTER=;"

:args
if "%~1"=="" goto args_done
if /I "%~1"=="Release" (set "CFG=Release"& shift& goto args)
if /I "%~1"=="Debug" (set "CFG=Debug"& shift& goto args)
if /I "%~1"=="-t" (set "MAX_TIER=%~2"& shift& shift& goto args)
if /I "%~1"=="-j" (set "JOBS=%~2"& shift& shift& goto args)
if /I "%~1"=="--strict" (set "STRICT=1"& shift& goto args)
if /I "%~1"=="--include-disabled" (set "INCLUDE_DISABLED=1"& shift& goto args)
if /I "%~1"=="--warm" (set "WARM=1"& shift& goto args)
if /I "%~1"=="--list" (set "LIST=1"& shift& goto args)
set "FILTER=!FILTER!%~1;"
shift
goto args

:args_done
set "COMPILER=%REPO%x64\%CFG%\cflat.exe"
set "OUT=%REPO%out\libs"
set "CACHE=%REPO%out\libs-cache"
set "CFLAT_CACHE_DIR=%CACHE%"
if not "%LIST%"=="1" if not exist "%COMPILER%" (
    echo FAIL: compiler not found: "%COMPILER%"
    exit /b 1
)
if not "%LIST%"=="1" (
    if not exist "%OUT%" mkdir "%OUT%"
    if not exist "%CACHE%" mkdir "%CACHE%"
    "%COMPILER%" --init-local >"%OUT%\init.log" 2>&1
    if errorlevel 1 (
        echo FAIL: compiler --init-local
        type "%OUT%\init.log"
        exit /b 1
    )
)

set /a PASS=0,FAIL=0,SKIP=0,DISABLED=0,XPASS=0,XFAIL=0
for /r "%REPO%test_libs" %%C in (*.cb) do call :check_stale "%%~fC"
for /d %%L in ("%REPO%test_libs\*") do if /I not "%%~nxL"=="vcpkg_installed" call :library "%%~fL"
echo.
echo Summary: !PASS! PASS, !FAIL! FAIL, !SKIP! SKIP, !DISABLED! DISABLED, !XPASS! XPASS, !XFAIL! XFAIL
if not "!FAIL!"=="0" exit /b 1
exit /b 0

:library
set "LIBDIR=%~1"
for %%N in ("%LIBDIR%") do set "LIBNAME=%%~nxN"
if not "!FILTER!"==";" (
    echo(!FILTER!| findstr /i /c:";!LIBNAME!;" >nul
    if errorlevel 1 exit /b 0
)
if not exist "%LIBDIR%\lib.cfg" (
    echo FAIL !LIBNAME!: missing lib.cfg
    set /a FAIL+=1
    exit /b 0
)
REM Keys get a CFG_ prefix: bare names (include, lib, ...) would clobber MSVC's INCLUDE / LIB.
for %%K in (tier root_win env_win probe include lib_win runpath_win version_win hint_win args timeout) do set "CFG_%%K="
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%LIBDIR%\lib.cfg") do set "CFG_%%A=%%B"
if not defined CFG_tier set "CFG_tier=1"
if !CFG_tier! gtr !MAX_TIER! exit /b 0
if "%LIST%"=="1" (
    for %%C in ("%LIBDIR%\*.cb") do if exist "%%~fC" call :list_case "%%~fC"
    exit /b 0
)
set "WARM_CASES=;"
set "ROOT="
if /I "!CFG_root_win!"=="testlibs" (
    call :vcpkg_preflight
    set "ENV_NAME=!CFG_env_win!"
    if not defined ENV_NAME set "ENV_NAME=CFLAT_TESTLIB_!LIBNAME!"
    call set "ROOT=%%!ENV_NAME!%%"
    if not defined ROOT set "ROOT=%REPO%test_libs\vcpkg_installed\%VCPKG_TRIPLET%"
) else if /I "!CFG_root_win!"=="deps" (
    if defined CFLAT_VCPKG_INSTALLED (set "ROOT=%CFLAT_VCPKG_INSTALLED%") else set "ROOT=%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed"
    set "ROOT=!ROOT!\x64-windows-static"
) else if /I "!CFG_root_win:~0,4!"=="env:" (
    set "ENV_NAME=!CFG_root_win:~4!"
    call set "ROOT=%%!ENV_NAME!%%"
)
if not defined ROOT (
    call :missing_lib "root !CFG_root_win! is not configured"
    exit /b 0
)
if not exist "!ROOT!\!CFG_probe!" (
    call :missing_lib "probe missing: !ROOT!\!CFG_probe!"
    exit /b 0
)
if defined CFG_version_win (
    echo !LIBNAME! version:
    call !CFG_version_win!
)
for %%C in ("%LIBDIR%\*.cb") do if exist "%%~fC" call :case "%%~fC" 0
if "%WARM%"=="1" (
    for %%C in ("%LIBDIR%\*.cb") do if exist "%%~fC" (
        echo(!WARM_CASES!| findstr /i /c:";%%~nC;" >nul
        if not errorlevel 1 call :case "%%~fC" 1
    )
)
exit /b 0

:vcpkg_preflight
if defined VCPKG_PREFLIGHT_DONE exit /b 0
set "VCPKG_PREFLIGHT_DONE=1"
REM Install test_libs\vcpkg.json once, before any case runs. --check of one package-vcpkg case makes cflat
REM run its own vcpkg install (one tree, same triplet the cases use); a no-op when already installed.
REM Racing per-case compiles would otherwise fight over vcpkg-running.lock.
echo Installing test_libs vcpkg ports ^(first run builds them; later runs are a no-op^)...
"%COMPILER%" "%REPO%test_libs\zlib\zlib_01_demo.cb" --check --nologo >"%OUT%\vcpkg-install.log" 2>&1
if errorlevel 1 (
    echo FAIL vcpkg install of test_libs\vcpkg.json
    call :tail5 "%OUT%\vcpkg-install.log"
    set /a FAIL+=1
)
exit /b 0

:missing_lib
if "%STRICT%"=="1" (
    echo FAIL !LIBNAME!: %~1 !CFG_hint_win!
    set /a FAIL+=1
) else (
    echo SKIP !LIBNAME!: %~1 !CFG_hint_win!
    set /a SKIP+=1
)
exit /b 0

:list_case
set "CASE_FILE=%~1"
for %%N in ("%CASE_FILE%") do set "CASE=%%~nN"
set "MODE=run"
set "DISABLED_PATHS="
findstr /b /c:"// MODE: check" "%CASE_FILE%" >nul && set "MODE=check"
for /f "tokens=1,* delims=:" %%A in ('findstr /b /c:"// DISABLED:" "%CASE_FILE%"') do for /f "tokens=*" %%D in ("%%B") do set "DISABLED_PATHS=%%D"
echo !LIBNAME! tier !CFG_tier! !CASE! mode=!MODE! disabled=!DISABLED_PATHS!
exit /b 0

:case
set "CASE_FILE=%~1"
set "WARM_PASS=%~2"
for %%N in ("%CASE_FILE%") do set "CASE=%%~nN"
set "MODE=run"
set "DISABLED_PATHS="
findstr /b /c:"// MODE: check" "%CASE_FILE%" >nul && set "MODE=check"
for /f "tokens=1,* delims=:" %%A in ('findstr /b /c:"// DISABLED:" "%CASE_FILE%"') do for /f "tokens=*" %%D in ("%%B") do set "DISABLED_PATHS=%%D"
if defined DISABLED_PATHS (
    if not "%INCLUDE_DISABLED%"=="1" (
        if not "%WARM_PASS%"=="1" (echo DISABLED !CASE! & set /a DISABLED+=1)
        exit /b 0
    )
)
if "%WARM_PASS%"=="1" if defined DISABLED_PATHS exit /b 0
for %%N in ("%LIBDIR%") do set "LIBNAME=%%~nxN"
set "CASE_OUT=%OUT%\!LIBNAME!\!CASE!"
if not exist "!CASE_OUT!" mkdir "!CASE_OUT!"
set "FLAGS="
set "CASE_ARGS=!CFG_args:@REPO@=%REPO_NS%!"
set "CASE_PATH=%PATH%"
for %%I in (!CFG_include!) do set "FLAGS=!FLAGS! --c-include "!ROOT!\%%I""
for %%L in (!CFG_lib_win!) do set "FLAGS=!FLAGS! --c-lib "!ROOT!\%%L""
pushd "!CASE_OUT!"
if defined CFG_runpath_win set "PATH=!ROOT!\!CFG_runpath_win!;%PATH%"
if "!MODE!"=="check" (
    "%COMPILER%" "%CASE_FILE%" !FLAGS! --check >"compile.log" 2>&1
) else (
    "%COMPILER%" "%CASE_FILE%" !FLAGS! -o "!CASE!.exe" >"compile.log" 2>&1
)
if errorlevel 1 (
    if defined DISABLED_PATHS (
        echo XFAIL !CASE!
        set /a XFAIL+=1
    ) else (
        echo FAIL !CASE! compile
        set /a FAIL+=1
    )
    call :tail5 "compile.log"
    set "PATH=!CASE_PATH!"
    popd
    exit /b 0
)
if "!MODE!"=="run" (
    ".\!CASE!.exe" !CASE_ARGS! >"run.log" 2>&1
    if errorlevel 1 (
        if defined DISABLED_PATHS (echo XFAIL !CASE!& set /a XFAIL+=1) else (echo FAIL !CASE! run& set /a FAIL+=1)
        call :tail5 "run.log"
        set "PATH=!CASE_PATH!"
        popd
        exit /b 0
    )
)
set "PATH=!CASE_PATH!"
popd
if defined DISABLED_PATHS (
    echo XPASS !CASE! ^(enable it^)
    set /a XPASS+=1
) else if "%WARM_PASS%"=="1" (
    echo PASS !CASE! warm
) else (
    echo PASS !CASE!
    set /a PASS+=1
    set "WARM_CASES=!WARM_CASES!!CASE!;"
)
exit /b 0

:check_stale
set "CASE_FILE=%~1"
for %%N in ("%CASE_FILE%") do set "CASE=%%~nN"
set "DISABLED_PATHS="
for /f "tokens=1,* delims=:" %%A in ('findstr /b /c:"// DISABLED:" "%CASE_FILE%"') do for /f "tokens=*" %%D in ("%%B") do set "DISABLED_PATHS=%%D"
for %%P in (!DISABLED_PATHS!) do call :stale_one "%%P"
exit /b 0

:stale_one
set "SP=%~1"
set "SP=!SP:/=\!"
if not exist "%REPO%!SP!" (
    echo FAIL !CASE!: stale DISABLED marker %~1
    set /a FAIL+=1
)
exit /b 0

:tail5
echo --- last log lines ---
powershell -NoProfile -Command "Get-Content -LiteralPath '%~1' -Tail 5"
exit /b 0
