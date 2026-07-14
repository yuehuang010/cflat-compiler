@echo off
REM Run cflat\out.ll through lli from the shared dependency tree.
if not defined CFLAT_VCPKG_INSTALLED set CFLAT_VCPKG_INSTALLED=%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed
"%CFLAT_VCPKG_INSTALLED%\x64-windows-static\tools\llvm\lli.exe" cflat\out.ll
