@echo off
setlocal

rem Build curl via vcpkg manifest mode. Installs into .\vcpkg_installed\x64-windows\.
rem Uses the vcpkg shipped with Visual Studio 2022 by default; override VCPKG_EXE
rem in the environment to use a different copy.

if "%VCPKG_EXE%"=="" set "VCPKG_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\vcpkg.exe"

if not exist "%VCPKG_EXE%" (
    echo ERROR: vcpkg not found at "%VCPKG_EXE%".
    echo Set VCPKG_EXE to point at vcpkg.exe and retry.
    exit /b 1
)

pushd "%~dp0"

rem Triplet x64-windows = dynamic CRT + dynamic libraries (DLLs).
rem cflat compiles via clang-cl + lld-link against the dynamic UCRT, so this is
rem the matching triplet. Static triplets (e.g. x64-windows-static) link curl's
rem mprintf.obj into the exe, which collides with cflat core's snprintf/vsnprintf.
echo === vcpkg install (triplet: x64-windows) ===
"%VCPKG_EXE%" install --triplet x64-windows --x-manifest-root=. --x-install-root=vcpkg_installed --clean-after-build
if errorlevel 1 (
    popd
    exit /b 1
)

echo.
echo === Cleaning temp/debug artifacts ===
rem Debug-config libs and PDBs are not needed for release linking.
if exist "vcpkg_installed\x64-windows\debug" rmdir /s /q "vcpkg_installed\x64-windows\debug"
rem vcpkg's internal bookkeeping copies; safe to drop.
if exist "vcpkg_installed\vcpkg" rmdir /s /q "vcpkg_installed\vcpkg"
if exist "vcpkg_installed\.vcpkg-root" del /q "vcpkg_installed\.vcpkg-root"
rem Tools, share/doc/man trees we will not link against.
if exist "vcpkg_installed\x64-windows\tools" rmdir /s /q "vcpkg_installed\x64-windows\tools"
if exist "vcpkg_installed\x64-windows\share" rmdir /s /q "vcpkg_installed\x64-windows\share"

echo.
echo === Done ===
echo Headers:  %CD%\vcpkg_installed\x64-windows\include
echo Lib:      %CD%\vcpkg_installed\x64-windows\lib\libcurl.lib
echo DLL:      %CD%\vcpkg_installed\x64-windows\bin\libcurl.dll

popd
endlocal
