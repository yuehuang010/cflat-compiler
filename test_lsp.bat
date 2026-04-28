@echo off
setlocal

echo === LSP smoke tests ===
python vscode-extension\test\lsp_test.py %*
if errorlevel 1 (
    echo FAILED: LSP smoke tests
    exit /b 1
)

echo.
echo === LSP fixture tests ===
python vscode-extension\test\lsp_fixture_test.py %*
if errorlevel 1 (
    echo FAILED: LSP fixture tests
    exit /b 1
)

echo.
echo All LSP tests passed.
exit /b 0
