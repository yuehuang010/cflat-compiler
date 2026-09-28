# String literal template arguments deduce const char* (C++: const char[N]); char* expressions unspellable

Found 2026-09-27 (fix/strlit-ref). Pre-existing, same before and after that fix.

1. A literal passed to a template `U&`, `U&&` or `const U&` deduces `U = const char*` in CFlat, C++ deduces
   `const char[N]`: `nref("abc")` (returns sizeof(U)) gives 8 vs clang 4; `fwdk("abc")` gives 2 vs clang 1.
   Cause: the template wrapper sends a literal as `const char *` first and only spells it inline on a
   failed call (RequestCxxFunctionTemplate argument loop, LLVMBackend_CInterop.cpp,
   CxxStringLiteralSpelling / literalCallArguments).
2. `s + 1` and `cond ? "a" : "b"` as C++ template arguments are refused with "an argument type cannot be
   spelled in C++".

Fix direction: for reference positions spell the literal as the array lvalue (like the decayed-array path
added in e6c60220, `*reinterpret_cast<const char (*)[N]>(p)`), keeping caller-frame storage; spell a
char* rvalue expression as `const char*` / `char*`. Legs near 2761-2764 in Test/test_cpp_interop_template.cb.
