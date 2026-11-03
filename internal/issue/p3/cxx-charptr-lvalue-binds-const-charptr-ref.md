# C++ `const char*&` binds a char* lvalue (clang refuses)

Found by MIX21 (2026-10-31) while probing `cbump(t ? p : q)` (Test/library/cpp_t67_postinc.hpp). PRE-EXISTING on master.

Repro (cflat accepts, clang refuses):

    import cpp "cpp_t67_postinc.hpp";
    extern int main() { char* p = nullptr; cpp_t67.cbump(p); return 0; }

clang++ -std=c++20: "non-const lvalue reference to type 'const char *' cannot bind to a temporary of type 'char *'"
(a `char*` lvalue needs a temporary for `const char*&`). cflat compiles it with rc 0.

Note: `char* p; char* q; cbump(t ? p : q)` is refused by clang too; the MIX21 brief assumed it must bind - wrong premise.

Root cause: not diagnosed. The overload path accepts a non-const `T*&` whose pointee needs an added const when the
argument is a char* lvalue (the const-added pointee arm of the C++ ref-to-pointer viability).

Fix direction: in the C++ viability for IsCxxRefToPointer params (LLVMBackend_Overloads.cpp, refToPointerBind arm),
refuse an lvalue whose pointee const must be ADDED for a non-const reference; keep `const char* const&` and
`const char*` by-value binding. Probe the by-value and const-reference neighbours before changing it.
