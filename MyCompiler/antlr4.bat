@echo off
setlocal

REM Find the location of the python executable and store it in a variable

FOR /F "delims=" %%P IN ('where python') DO (
    SET PYTHON_EXE_PATH=%%P
    GOTO FoundPython
)

:FoundPython
REM Extract the directory containing the python executable
REM The /D modifier in FOR command can be complex to use directly with the output of 'where python'
REM Instead, we will manipulate the string using variable substitution

REM Isolate the directory path by removing the executable name
SET PYTHON_DIR=%PYTHON_EXE_PATH%
FOR %%I IN ("%PYTHON_DIR%") DO SET PYTHON_DIR=%%~dI%%~pI%%~nI

REM A more direct way to get the parent directory using FOR variable modifiers
FOR %%I IN ("%PYTHON_EXE_PATH%") DO SET PYTHON_DIR=%%~dpI

REM Construct the full path to the Scripts folder
SET SCRIPTS_DIR=%PYTHON_DIR%Scripts\

ECHO Python executable found at: %PYTHON_EXE_PATH%
ECHO Scripts folder path: %SCRIPTS_DIR%

%SCRIPTS_DIR%\antlr4.exe %*