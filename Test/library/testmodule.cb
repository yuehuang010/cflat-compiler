// Module for import testing.
// Defines functions, a struct, and a namespace that testfile3.c imports.

int moduleAdd(int a, int b) { return a + b; }
int moduleMultiply(int a, int b) { return a * b; }

struct ModulePoint
{
    int x = 0;
    int y = 0;

    int Sum() { return x + y; }
    int Scale(int factor) { return (x + y) * factor; }
};

namespace ModuleMath
{
    int square(int x) { return x * x; }
    int cube(int x)   { return x * x * x; }
}
