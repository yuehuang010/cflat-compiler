// MyCompiler.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

#include "MyCompilerLLVM.h"

int main()
{
	std::cout << "MyCompilerLLVM\n";

	MyCompilerLLVM compiler;
	compiler.Compile("testfile.cflat");
	std::cout << "Done.\n";
}
