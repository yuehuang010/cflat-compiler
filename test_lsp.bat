@echo off
setlocal

if "%CFLAT_CONFIG%"=="" set CFLAT_CONFIG=Debug
set COMPILER=x64\%CFLAT_CONFIG%\cflat.exe

echo === LSP smoke tests ===
python vscode-extension\test\lsp_test.py %COMPILER% %*
if errorlevel 1 (
    echo FAILED: LSP smoke tests
    exit /b 1
)

echo.
echo === LSP fixture tests ===
python vscode-extension\test\lsp_fixture_test.py %COMPILER% %*
if errorlevel 1 (
    echo FAILED: LSP fixture tests
    exit /b 1
)

echo.
echo All LSP tests passed.
exit /b 0
