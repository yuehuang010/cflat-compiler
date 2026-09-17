# `std::atomic<T>`'s load / store / fetch_add are never bound, and the error lists CFlat CORE's atomic overloads instead

Found 2026-09-17 by the C++-interop bug bash round 6 (macOS arm64, Release, worktree at master
e409a26d). Suggested bucket: **p2** (a headline std type is unusable; the diagnostic actively
misleads).

## Summary

`std.atomic<int>` binds as a TYPE: it can be declared, `= default` works, its size is right, and a
C++ function taking `std::atomic<int>&` can be called with it. `a.is_lock_free()` binds and runs.
But the three members that make the type useful - `load`, `store`, `fetch_add` (also `exchange`) -
are not in the C++ member set at all. Every call fails, and the candidate list printed is CFlat
CORE's `atomic<T>` monomorphizations (`atomic__i32`, `atomic__i64`, `atomic_counter`), which the
program never imported and which the user cannot reach. So the message points at the wrong
library entirely.

There is also no way to spell a memory order (`std.memory_order.seq_cst` -> "'seq_cst' is not a
member of namespace 'std.memory_order'"), so the explicit-order overloads are unreachable too -
the type is effectively write-only through hand-written C++ helpers.

## Repro (compile 1)

`scratch/bb6_atomic2.cb`
```cflat
import cpp "atomic" cache;

extern int main()
{
    std.atomic<int> a = default;
    printf("load=%d expect=0\n", a.load());
    return 0;
}
```

Measured:
```
bb6_atomic2.cb(6,33): no overload of 'load' matches the given arguments.
  Call arguments (1):
    [0] std.atomic<int> <this>
  Candidates (2):
    load(atomic__i32*)
    load(atomic__i64*)
```

`a.store(v)` with an `int` local gives the same shape, listing
`store(atomic_counter*, i64)`, `store(atomic__i32*, int)`, `store(atomic__i64*, i64)`.
`a.load(bb6mo.get())` (an explicit `std::memory_order` value obtained from a C++ helper) fails
identically, so it is not an arity problem.

What DOES work, in the same build:

* `scratch/bb6_at4.cb`: `a.is_lock_free()` -> compile 0, prints 1.
* `scratch/bb6_at3.cb`: `bb6a.bump(a)` / `bb6a.peek(a)` where the header declares
  `int bump(std::atomic<int>& a) { return a.fetch_add(2); }` -> compile 0, prints
  `size=4 bump=0 peek=2`. So the object, its layout and its reference passing are all fine.

## Root cause - narrowed, not found

Each of the obvious suspects was probed and EXCLUDED (all compile 0 and print the expected value):

| Hypothesis | Probe | Result |
|---|---|---|
| default argument of scoped-enum type | `bb6_defarg` (`Mode m = Mode::A`) | binds |
| default argument that is a `constexpr` VARIABLE | `bb6_defarg2` (`Mode m = kDefault`) | binds |
| `const` / `const volatile` overload pair | `bb6_vol`, `bb6_vol2` (also with default args) | binds |
| `__attribute__((diagnose_if(...)))` on the member (libc++'s `_LIBCPP_CHECK_LOAD_MEMORY_ORDER`) | `bb6_dif` | binds |
| members inherited from a DEPENDENT class-template base (`Der<T> : BaseT<T>`, signature uses `T`) | `bb6_depbase` | binds |
| a C++ class of its own with members named `load` / `store` / `fetch_add` (core shadowing) | `bb6_shadow` | binds and runs |
| `std::memory_order` as a parameter / return type | `bb6_mo` | binds, round-trips as 5 |

So the drop is specific to the shape libc++ uses in `std::__1::__atomic_base`. The next step is to
dump the bound member list for `std.atomic<int>` during the clang AST walk and see at which filter
`load` is discarded.

Independently of the cause, the FALLBACK is a defect on its own: when a C++ member name does not
resolve, the error falls through to CFlat's global overload table and prints core-library
candidates. It should say "class 'std.atomic<int>' has no member 'load'" and list the members that
WERE bound.

## Fix direction

1. Instrument the member-binding filter for `std::__1::__atomic_base<int, false>` and find the
   rejection point; the six probes above rule out the generic explanations.
2. Make an unresolved C++ member call report the C++ class's own member set, never CFlat core's
   same-named free/overload set.
3. Make scoped-enum ENUMERATORS in `std` spellable (`std.memory_order.seq_cst`); related to
   `internal/issue/p3/cpp-scoped-enum-non-type-template-argument-unspellable.md`.
4. Coverage belongs in `Test/test_cpp_interop.cb`: `std.atomic<int>` store/load/fetch_add with an
   asserted sequence, plus an IR check that the emitted call really is an atomic one.
