@echo off
setlocal
REM set PATH=%CD%\vcpkg_installed\x64-windows-static\x64-windows-static\tools\llvm;%PATH%
msbuild MyCompiler.slnx /p:Platform=x64 /p:Configuration=Debug && C:\source\MyCompiler\x64\Debug\MyCompiler.exe MyCompiler\Test\test_generics.cb -o myapp.exe --out-lli out.ll -p win64 %*

endlocal