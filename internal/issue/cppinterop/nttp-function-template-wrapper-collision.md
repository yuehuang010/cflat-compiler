# `std.get<0>` and `std.get<1>` on one tuple type share a wrapper: both read element 0

Found 2026-09-22 while measuring the constructor-template matrix. SILENT WRONG VALUE.

## Repro

```cflat
import cpp "tuple";
extern int printf(char* f, ...);
extern int main()
{
    std.tuple<int, int> t = std.tuple<int, int>(1, 2);
    int b = std.get<1>(t);
    int a = std.get<0>(t);
    printf("b %d a %d\n", b, a);   // prints "b 2 a 2"; expected "b 2 a 1"
    return 0;
}
```

The IR calls ONE `__cflat_free_<hash>` for both `std.get` calls - whichever is requested first
wins. The existing coverage (`std.get<0>/<1>` on `std.tuple<cppi.Tracked, int>`, 1216-1217 in
Test/test_cpp_interop_template.cb) passes only because the two element types differ, so the
wrapper signatures differ.

## Root cause (suspected, not verified)

The free-function-template wrapper key (RequestCxxFreeFunction / RequestCxxFunctionTemplate in
cflat/LLVMBackend_CInterop.cpp) is built from the argument and return types but not from the
explicit NON-TYPE template arguments. Check the hashKey composition and the functionTable
registration name.

## Fix direction

Fold explicit template arguments into the wrapper key and registration name. Acceptance: the repro
prints `b 2 a 1`; add `std.get<1>` to the 2556 leg in Test/test_cpp_interop_template.cb.
