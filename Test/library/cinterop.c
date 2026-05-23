/* Real C (clang-compilable) fixture for the C-interop test.
   Compiled by clang-cl when imported from a .cb; the object is linked by lld. */

int c_add(int a, int b)
{
    return a + b;
}

int c_square(int x)
{
    return x * x;
}
