# P3: C++ pointer-to-bool ranking edge cases

## Repros

These unresolved calls disagree with `clang++ -std=c++20` after the N64 round-3 ranking fix.
The tests are in `scratch/rv64/rv.h` and `scratch/rv64/nv.h`.

```cpp
inline int c1(char*) { return 1; }
inline int c1(bool) { return 3; }
// c1("x"): CFlat selects char* (1); clang selects bool (3).

inline int identity(int x) { return x; }
inline int f2(const void*) { return 4; }
inline int f2(bool) { return 3; }
inline int f2_fn_probe() { return f2(identity); }
// f2(identity): CFlat rejects the call; clang selects bool (3).

inline int n3(int*) { return 1; }
inline int n3(bool) { return 3; }
// n3(0): CFlat selects int* (1); clang reports an ambiguous call.
```

The ranking-only non-viable marker also cannot fix cases where CFlat does not admit the
user-defined-conversion overload into its candidate set:

```cpp
struct Wrap { Wrap(void*) {} };
inline int w1(Wrap) { return 1; }
inline int w1(int*) { return 2; }
// w1(voidPointer): CFlat selects int* (2); clang selects Wrap (1).

inline int vw(int*) { return 1; }
inline int vw(Wrap) { return 2; }
// vw(voidPointer): CFlat selects int* (1); clang selects Wrap (2).

inline int nw(std::nullptr_t) { return 5; }
inline int nw(Wrap) { return 1; }
// nw(charPointer): CFlat rejects the call; clang selects Wrap (1).
```

One fallback case is deliberately not refused by overload finishing, as required to preserve
CFlat's existing acceptance when no C++-viable candidate wins:

```cpp
inline int nb(std::nullptr_t) { return 5; }
inline int nb(int*) { return 1; }
// nb(charPointer): CFlat selects int* (1); clang reports no matching overload.
```

## Root cause

The ranker still treats string-literal-to-mutable-`char*` as viable, does not model function-pointer
conversion ranking or integer-literal-zero ambiguity. Separately, CFlat's candidate set omits some
overloads reachable through a C++ converting constructor, so marking `void* -> int*` as non-viable
cannot make the missing `Wrap` candidate win. The required finish fallback retains some
CFlat-accepted calls when all ranked C++ choices are non-viable.

## Fix direction

Keep these cases separate from pointer-to-`void*` versus pointer-to-`bool` ranking. Add focused
candidate-set and conversion-ranking probes before changing either path.
