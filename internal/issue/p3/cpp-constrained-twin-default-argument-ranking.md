# C++ constrained twins with default arguments rank wrong (pre-existing)

## Summary
Two same-signature C++ members that differ only by a trailing requires-clause, where EITHER
has a default argument, are bound as one CFlat overload: RegisterCxxClassMembers
(LLVMBackend_CInterop.cpp, `instanceBySig`) dedupes by CFlat signature and keeps the first
declared. clang picks the more constrained twin whenever both are viable; CFlat calls the
first. Same on master 33866560 and after T22 (T22 prunes the less constrained twin ONLY when
neither twin has a default argument and both agree on the ellipsis).

## Repro (Test/library/cpp_t22_constrained_members.hpp shapes; clang++ -std=c++20 oracle)
```cpp
template<class T> struct P {
    int pick(int x = 1) { return x + 10; }
    int pick(int x) requires std::integral<T> { return x + 20; }   // P<long>.pick(5): clang 25, CFlat 15
};
// int pick(int x = 1) / int pick(int x = 2) requires ...  -> pick(): clang 22, CFlat 11
// int pick(int x) / int pick(int x = 3) requires ...      -> pick(): clang 23, CFlat "no overload"; pick(4): 24 vs 14
// static twins as the first shape                         -> S<long>.pick(5): clang 25, CFlat "cannot choose";
// MSVC ABI only: P<int>.pick() returns 21 (clang-cl 11) and the two-parameter pick() 23 (13):
//   the shared symbol gets the constrained twin's body.
//   MSVC ABI only: S<int>.pick() refused ("no overload"): the twins share one linkage name, so
//   the second registration merges into the first FunctionSymbol and its (empty) defaults win
// pick(int a = 1, int b = 2) / pick(int a, int b = 3) requires -> pick(4): 27 vs 16, pick(4, 4): 28 vs 18
```
Legs that already match clang on Itanium live in Test/test_cpp_interop.cb 9527/9530, guarded
off Windows (default
omitted, only the unconstrained twin viable). Also scratch repro (T22 worktree):
scratch/rev_t22_round3_msvc_winner.{hpp,cb,sh} (`f(int a=1,int b=2)` vs constrained
`f(int a,int b=next())`: p.f(4) 27, p.f(4,4) 28 in clang; CFlat returns 1).

## Root cause
The CFlat overload set has no notion of the constraint ordering, and the signature dedupe
leaves only one of the twins. Itanium on LLVM 23 mangles the constraint (distinct symbols);
MSVC does not (both twins share one symbol name).

## Fix direction
Each twin needs its own viable arity domain: the less constrained one is callable only for
argument counts the more constrained one cannot accept. A T22 attempt (reverted, round 3/3b)
bound the loser through default-argument wrappers only and filtered the demand pass on MSVC.
Pitfalls found by review of that attempt:
- CxxDefaultWrapperCallAmbiguous counts a retained loser whose trailing defaults cover the
  wrapper's arity: the WINNER's nonconstant-default wrapper (`f(int a, int b = next())`) was
  falsely "ambiguous ... without its default arguments". The arity domain must apply to the
  wrapper ambiguity check too (receiver slot included).
- Under MSVC both twins share a linkage name and so a wrapper-name prefix: classifying a
  demanded wrapper as the loser's by prefix alone mistook the winner's own wrapper. Decode the
  wrapper arity and compare it with the loser's domain.
- Under MSVC a program using both twins of ONE instantiation cannot compile in clang either
  ("definition with same mangled name"); the demand pass must pick the body by which arities
  the program uses, and refuse the combination.
