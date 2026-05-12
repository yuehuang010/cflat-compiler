@echo off
setlocal

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Release
set _CFG_ARG=%~1
set _EXTRA=%*
if /I "%_CFG_ARG%"=="Debug" (
    set CFLAT_CONFIG=Debug
    set _EXTRA=%2 %3 %4 %5 %6 %7 %8 %9
) else if /I "%_CFG_ARG%"=="Release" (
    set CFLAT_CONFIG=Release
    set _EXTRA=%2 %3 %4 %5 %6 %7 %8 %9
)
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe

echo === LSP smoke tests ===
python vscode-extension\test\lsp_test.py %COMPILER% %_EXTRA%
if errorlevel 1 (
    echo FAILED: LSP smoke tests
    exit /b 1
)

echo.
echo === LSP fixture tests ===
python vscode-extension\test\lsp_fixture_test.py %COMPILER% %_EXTRA%
if errorlevel 1 (
    echo FAILED: LSP fixture tests
    exit /b 1
)

echo.
echo === LSP bulk source sweep ===
python vscode-extension\test\lsp_bulk_test.py %COMPILER% %_EXTRA%
if errorlevel 1 (
    echo FAILED: LSP bulk source sweep
    exit /b 1
)

echo.
echo All LSP tests passed.
exit /b 0
