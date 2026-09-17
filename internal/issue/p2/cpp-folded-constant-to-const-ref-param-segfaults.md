# Passing a folded C++ constant to a `const T&` C++ parameter compiles and segfaults

Found 2026-09-16 by the round-1 review of fix/cpp-static-const-member (macOS arm64, Release).
PRE-EXISTING: reproduced on the pre-fix binary with a `static constexpr` member and with an
enum constant; the static-const fix only widens the set of names that reach it.

## Repro

Header:

```cpp
namespace cr {
    struct A { static constexpr int k = 41; };
    inline int takes_ref(const int& v) { return v + 1; }
}
```

```cflat
import cpp "cr.h";
extern int main() { if (cr.takes_ref(cr.A.k) != 42) return 1; return 0; }
```

Compiles; the program exits 139. A literal argument (`cr.takes_ref(41)`) is refused cleanly at
compile time; only NAMED storage-less constants (folded static members, enum constants) slip
through, so the reference is materialized from nothing.

## Root cause (hypothesis)

The const-ref argument path takes the address of the named variable's storage; a folded
constant has no Storage, and the missing-storage check that rejects literals does not run for
the named-constant shape. See the review notes in the static-const fix worktree report
(scratch/sconst_review1.md at the time) for the measured cells.

## Fix direction

Materialize a temporary for a storage-less constant bound to a `const T&` parameter (the same
thing C++ does), or refuse with the literal's diagnostic. Assert the value round-trips
(`takes_ref(cr.A.k) == 42`) in Test/test_cpp_interop.cb.
