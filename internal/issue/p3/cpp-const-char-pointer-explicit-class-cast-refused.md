# Explicit cast of an imported C++ const char* to a class is refused

Acceptance gap (refused on master too; T75 review 3). Header: `inline const char* constSource();`
plus a class with `RankExplicit(const char*)` and `explicit RankExplicit(char*)`. CFlat:
`(ns.RankExplicit)ns.constSource()` or a local initialized from it. CFlat refuses with
"cannot pass C++ const pointer argument ... to mutable C++ pointer parameter 'p1'";
clang++ -std=c++20 picks the const char* ctor (tag 2).

Cause: the direct-init cast path (ConvertThroughCxxConvertingCtor directInit branch,
cflat/LLVMBackend_CInterop.cpp) builds the wrapper through RequestCxxVariadicConstructor, whose
argument spelling (~15052) does not reapply IsCxxPointeeConst, so an imported const char* is spelled
char*. Fix direction: spell a pointee-const C++ value as `const char *` in the wrapper; add local and
call-result value legs beside test_cpp_interop legs 2291-2294.
