// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

#include "MyCompilerLLVM.h"
#include "ArgParser.h"

int main(int argc, char* argv[])
{
    ArgParser args;
    args.addPositional("filename", "Source file to compile");
    args.addOption("output", 'o', "Output IR file path", ".\\out.ll");
    args.addOption("bitcode", 'b', "Output bitcode file path (.bc)");
    args.addFlag("debug-info", 'g', "Emit DWARF debug information");

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

    MyCompilerLLVM compiler;
    compiler.Compile(args);
    std::cout << "Done.\n";

    // Run vcpkg_installed\x64-windows\x64-windows\tools\llvm\lli.exe MyCompiler\out.ll
}
