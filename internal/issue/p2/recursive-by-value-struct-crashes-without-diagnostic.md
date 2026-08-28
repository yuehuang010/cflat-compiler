# A struct containing itself BY VALUE crashes the compiler with no diagnostic

Filed 2026-08-27, found while verifying the opaque-by-value FunctionType fix under an
assertions-enabled LLVM. Pre-existing and unrelated to that fix.

## Repro

Direct self-containment:

```cflat
struct A
{
    A inner;
    int x;
};

extern int main()
{
    A a;
    a.x = 1;
    printf("x=%d\n", a.x);
    return 0;
}
```

Mutual containment (`B` holds a `C`, `C` holds a `B`) reproduces identically:

```cflat
struct B { C c; int x; };
struct C { B b; int y; };
```

Three spellings crash identically (verified); the pointer form is the working control:

| Field | Result |
|-------|--------|
| `A inner;` in a `struct` | crash |
| `A inner;` in a `class` | crash |
| `A[4] arr;` (array of self) | crash |
| `A* next;` | compiles and runs correctly |

The array case matters for the fix: a check that walks only directly struct-typed fields
misses `A[4]`, which is equally infinite.

## Observed

| Config | Exit | Output |
|--------|------|--------|
| Release | 139 (SIGSEGV) | nothing at all |
| Debug (LLVM assertions ON) | 134 (SIGABRT) | see below |

```
Failure value returned from cantFail wrapped call
identified structure type 'A' is recursive
UNREACHABLE executed at .../llvm-22.1.8-assert/include/llvm/Support/Error.h:779!
```

The Release behaviour is the bad one: a silent segfault with no `file(line,col):` diagnostic
and no indication of which type is at fault.

## Root cause

A by-value containment cycle has no finite layout, so `StructType::setBody` produces a
recursive identified type and LLVM rejects it downstream (the data-layout query is wrapped
in `cantFail`, hence `UNREACHABLE` rather than a returned error).

The compiler has no by-value containment cycle check. The one cycle check that exists,
`RejectUniqueDestructionCycles` ([LLVMBackend_VariablesAndIR.cpp:1218](../../../cflat/LLVMBackend_VariablesAndIR.cpp:1218),
called from `CreateStructType` at :1278 and :1293), is a different analysis: it walks only
`unique` POINTER fields (`f.IsUnique && f.Pointer && !f.ElemPointer`) looking for unbounded
destructor recursion. Pointer fields are exactly the case that is legal here, and by-value
fields are exactly the case it skips.

## Fix direction

Add a by-value containment cycle check alongside `RejectUniqueDestructionCycles` in
`CreateStructType`, walking non-pointer struct-typed fields (`!f.Pointer`) transitively and
reporting with `LogError` when the walk reaches the type being defined. This satisfies the
CLAUDE.md rule that a diagnosed LLVM assert gets a proper compiler error message.

Two wrinkles the implementation has to handle:

- **Mutual recursion.** `B -> C -> B` must be caught when defining whichever of the two
  closes the cycle; the walk needs a `seen` set like `UniqueChainReaches` has.
- **Definition order.** At `CreateStructType` time the referenced struct may not be in
  `dataStructures` yet, or may still be an opaque shell from `ForwardRefScanner`. A walk that
  silently `continue`s on a missing entry (as `UniqueChainReaches` does) will miss cycles
  that close through a not-yet-defined type. This interacts with the provisional-declaration
  machinery added for the opaque-by-value fix; the check may need to run at the point the
  body is actually set rather than only at first registration.

Error message should name the field and the cycle, e.g. `'A.inner' contains 'A' by value,
which has no finite size. Make it a pointer ('A*') or break the cycle.`

## Prior art

Every comparable language rejects this at compile time with a named diagnostic; none
support it, and in all three the escape hatch is the same indirection cflat already allows.

- **C++** - ill-formed. A class is incomplete inside its own definition, and a non-static
  data member cannot have incomplete type. Clang: `field has incomplete type 'A'`. Arrays of
  self are rejected for the same reason; `static A a;` is fine (not part of layout), as are
  pointers and references.
- **Rust** - `error[E0072]: recursive type 'A' has infinite size`, with
  `help: insert some indirection (e.g., a Box, Rc, or &) to break the cycle`. Mutual
  recursion reports the same code. This is the best model for the message: it names the
  type, states WHY (infinite size), and prescribes the fix.
- **C#** - `error CS0523: Struct member 'A.inner' of type 'A' causes a cycle in the struct
  layout`. Note the value/reference split: this is an error for `struct` only. For a `class`,
  the field is a reference, so `class A { A inner; }` is legal and universally used for
  linked structures. cflat crashes on BOTH its `struct` and its `class`, so if `class` is
  meant to carry reference semantics anywhere, the check has to respect that distinction
  rather than blanket-rejecting.

## Impact

Invalid user code, so no correct program is affected - but the failure mode is a bare
segfault in Release, which is indistinguishable from a compiler bug and gives the user
nothing to act on. Low frequency, poor diagnosability.
