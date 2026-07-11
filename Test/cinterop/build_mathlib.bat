@echo off
setlocal
REM ---------------------------------------------------------------------------
REM Builds mathlib.lib (a STATIC archive) from mathlib.c, using the clang-cl /
REM lld-link that cflat deploys next to cflat.exe. A static archive needs no
REM linking here - the CRT symbols resolve later when cflat links the final exe,
REM so this works without the Windows SDK/MSVC lib paths on the lld-link line.
REM
REM Usage:  build_mathlib.bat [Release|Debug]   (default: Release)
REM ---------------------------------------------------------------------------
set HERE=%~dp0
set CFG=%~1
if "%CFG%"=="" set CFG=Release

set TOOLS=%HERE%..\..\x64\%CFG%
set CLANG=%TOOLS%\clang-cl.exe
set LLDLIB=%TOOLS%\lld-link.exe

if not exist "%CLANG%" (
    echo ERROR: clang-cl not found at "%CLANG%"
    echo Build cflat first with cmake_build.bat; that deploys clang-cl/lld-link.
    exit /b 1
)

"%CLANG%" /c /nologo "%HERE%mathlib.c" /Fo"%HERE%mathlib.obj"
if errorlevel 1 exit /b 1

"%LLDLIB%" /lib /nologo /out:"%HERE%mathlib.lib" "%HERE%mathlib.obj"
if errorlevel 1 exit /b 1

del "%HERE%mathlib.obj" 2>nul
echo Built "%HERE%mathlib.lib"
