@echo off
setlocal

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
set _CFG_ARG=%~1
set _POOL_ARG=%~1
if /I "%_CFG_ARG%"=="Debug" (
    set CFLAT_CONFIG=Debug
    set _POOL_ARG=%~2
) else if /I "%_CFG_ARG%"=="Release" (
    set CFLAT_CONFIG=Release
    set _POOL_ARG=%~2
)
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe

rem LSP backend/worker pool size. Defaults to 4 (the diminishing-returns sweet spot
rem for this suite); override by passing a count, e.g. "test_lsp.bat Release 8".
if "%_POOL_ARG%"=="" set _POOL_ARG=4
set _POOL=--lsp-pool-size %_POOL_ARG%

echo === LSP smoke tests ===
python vscode-extension\test\lsp_test.py %COMPILER% %_POOL%
if errorlevel 1 (
    echo FAILED: LSP smoke tests
    exit /b 1
)

echo.
echo === LSP fixture tests ===
python vscode-extension\test\lsp_fixture_test.py %COMPILER% %_POOL%
if errorlevel 1 (
    echo FAILED: LSP fixture tests
    exit /b 1
)

echo.
echo === LSP bulk source sweep ===
python vscode-extension\test\lsp_bulk_test.py %COMPILER% %_POOL%
if errorlevel 1 (
    echo FAILED: LSP bulk source sweep
    exit /b 1
)

echo.
echo All LSP tests passed.
exit /b 0
