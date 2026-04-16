// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <filesystem>
#include <stdlib.h>

#include "MyCompilerLLVM.h"
#include "ArgParser.h"

int main(int argc, char* argv[])
{
_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);                                                                    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);                                                                    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);

    // Disable stdout buffering so verbose/diagnostic output survives a crash.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    ArgParser args;
    args.addPositional("filename", "Source file to compile");
    args.addOption("output", 'o', "Output native executable path (.exe)");
    args.addOption("out-lli", 'l', "Output LLVM IR file path (.ll)");
    args.addOption("bitcode", 'b', "Output bitcode file path (.bc)");
    args.addFlag("debug-info", 'g', "Emit DWARF debug information");
    args.addOption("import-dir", 'i', "Directory to search for imported modules");
    args.addOption("platform", 'p', "Target platform: x64 (default) or x86", "x64");
    args.addFlag("verbose", 'v', "Print detailed diagnostic messages during compilation");

    if (!args.parse(argc, argv))
        return 1;

    auto filename = args.getPositional(0);
    if (!filename)
    {
        std::cerr << "Error: no input file specified.\n\n";
        args.printUsage();
        return 1;
    }

    std::cout << "MyCompilerLLVM\n";

    // Locate runtime.cb next to this executable.
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    std::string runtimeDir = std::filesystem::path(pgmptr ? pgmptr : "").parent_path().string();

    MyCompilerLLVM compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    if (!compiler.Compile(args))
    {
        std::cerr << "Compilation failed.\n";
        return 1;
    }
    std::cout << "Done.\n";

    // Run vcpkg_installed\x64-windows\x64-windows\tools\llvm\lli.exe MyCompiler\out.ll
}
