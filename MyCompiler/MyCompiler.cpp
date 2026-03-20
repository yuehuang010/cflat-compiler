// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

#include "MyCompilerLLVM.h"

int main()
{
	std::cout << "MyCompilerLLVM\n";

	MyCompilerLLVM compiler;
	// compiler.Compile("testfile.c");  // Baseline C standard
	// compiler.Compile("testfile2.c");  // Modified C like syntax
	compiler.Compile("testfile3.c");  // Test file
	std::cout << "Done.\n";

	// Run vcpkg_installed\x64-windows\x64-windows\tools\llvm\lli.exe MyCompiler\out.ll
}
