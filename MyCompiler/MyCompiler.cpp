// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

#include "MyCompilerLLVM.h"

int main()
{
	std::cout << "MyCompilerLLVM\n";

	MyCompilerLLVM compiler;
	compiler.Compile("testfile2.cflat");
	std::cout << "Done.\n";

	// Run C:\source\vcpkg\installed\x64-windows\tools\llvm\lli.exe out.ll
}
