# p3: Returned const C++ pointer still permits writes to non-mutable fields

## Repro

`Test/library/cpp_t217_mutable.hpp` declares `inline const T217KM* t217_const_ptr()`. This CFlat write currently compiles and runs, but clang++ refuses it:

```cflat
import cpp "cpp_t217_mutable.hpp";
extern int main() { t217_const_ptr()->n = 6; return 0; }
```

Measured against the master Release executable: compile/run succeeds with exit code 0. `clang++ -std=c++20 -fsyntax-only` says `cannot assign to return value because function 't217_const_ptr' returns a const value`. The mutable sibling `t217_const_ptr()->m` remains writable.

## Root cause

The assignment const-object guard only recognizes a destination whose underlying object is a registered const global. A field reached through a returned pointer has a call-result base, so that guard does not see the returned pointer's const qualification. This is separate from the T335 GEP ancestry and flattened-field metadata gaps, which are fixed in this worktree.

## Fix direction

Propagate const-pointee provenance from a C++ pointer result to member accesses and refuse writes to non-mutable fields while retaining C++ `mutable` writes. Keep the const-reference-result path distinct.
