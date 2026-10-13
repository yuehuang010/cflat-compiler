# C++ constructor tie with a dropped narrowing sibling: same-width distinct types still picked silently

From T148 review 1 (2026-10-12), master and T148 alike. T140 asks clang only when CFlat's pick WIDENS; T148 added "Exact by width
but the C++ names differ and one side is long/ulong" (Windows 32-bit long, Darwin long vs long long). Other same-width distinct C++
types beside a char constructor still pick the non-char overload (33) where clang++ -std=c++20 reports the call ambiguous:
short -> unsigned short, int -> unsigned int, and `enum E : long` -> long long.
Repros scratch/repro_keep/t148/ (short_unsigned.cb, int_unsigned.cb, enum_long.cb, probe.hpp).
Site: cflat/LLVMBackend_CInterop.cpp ~15469 (CxxConstructorNeedsClangResolution). Fix direction: compare canonical C++ identities
of argument and parameter for Exact-by-width scalar conversions (signedness, enum-to-integer), distinguishing same-type aliases
(typedefs, CFlat aliases - those must stay exact); keep the global width classifier unchanged; add legs to
Test/errors/err_cpp_null_tie_ctor_ambiguous.cb.
