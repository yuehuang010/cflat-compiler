# Bucket p3: a C++ class held as a FIELD of a CFlat struct global is never constructed

Found 2026-09-17 while fixing internal/issue/p2/cpp-global-class-object-never-constructed-or-destroyed.md
(macOS arm64, Release, branch fix/cpp-global-ctor).

## Summary

The p2 fix constructs a file-scope object whose OWN type is a C++ class from an
`llvm.global_ctors` entry. A C++ class reached through a CFlat struct is still zero-filled:

```cflat
import cpp "library/cpp_interop_arrayelem.h";
struct H { cppa.Trk t = default; };
H g = default;                       // cppa.ctor_count() == 0, g.t.v == 0
extern int main() { printf("%d %d\n", cppa.ctor_count(), g.t.v); return 0; }
```

Measured identical on the pre-fix and post-fix binaries (`ctor=0 v=0`, no diagnostic), so this is
not a regression - it is the part of the family the p2 fix deliberately left alone.

## Root cause (different site from the p2 fix)

The p2 fix queues the DECLARED type at the global declarator. Here the declared type is a CFlat
struct, and its field default comes from `MainListener::GenerateDefaultValue`, whose C++-class arm
is gated `&& !global_scope` (cflat/MainListener_Declarations.cpp, "IsForeignCxxClassWithConstructors
... && !global_scope"). At global scope it falls through to `Constant::getNullValue`.

A local `H h = default;` constructs the field correctly, so the asymmetry is global-scope only.

## Why it needs its own ruling and accept set

Fixing it means walking a CFlat struct's fields (recursively, through fixed arrays and nested
structs) inside the module initializer and constructing only the C++ sub-objects - while the
enclosing CFlat struct's OWN default construction at global scope is a constant FOLD
(`TryFoldGlobalDefaultConstruction`), a different mechanism. The two have to be sequenced (the
fold writes the whole aggregate; a field constructor writes into it afterwards) and the
zero-initialized fallback path already emits the "global is zero-initialized" note for CFlat
structs that do not fold, which is a separate, broader gap. Decide whether the field walk runs
before or after that note, and what happens when both apply, before building it.

## Fix direction

Extend `MainListener::EmitPendingGlobalCxxConstructions` to also queue a CFlat-struct global whose
layout transitively contains a C++ class with a nontrivial default constructor, and construct each
such sub-object through a GEP in the module initializer, after the aggregate's folded constant is
in place. Accept set to freeze first: a CFlat struct global with no C++ field (unchanged, still one
constant), a struct global whose fold fails today (still just the note), and a LOCAL `H h =
default;` (already correct).

## Neighbouring cell measured 2026-09-17: a `static` DATA MEMBER of a CFlat struct

Raised in review of fix/cpp-global-ctor. NOT the same family as the field cell above, and not the
global declarator the fix touches - recorded here so it is not lost.

```cflat
import cpp "cpp_interop_arrayelem.h";
struct H { static cppa.GTrk t; int z; };
extern int main() { printf("gctor=%d\n", cppa.gctor_count()); return 0; }   // prints gctor=0
```

Measured identical on master, on 1b082dc9 and on the fix: `gctor=0`, clean compile.

The reason is broader than C++ interop: a `static` data member of a CFlat struct creates **no
storage at all**, for ANY type. `struct H { static int t; int z; };` emits no global either, and
reading `H.t` is rejected with "'t' does not name a value here. If it is a method, call it:
't()'." So there is nothing to construct, no global declarator runs, and the C++ case is only the
most visible symptom. The struct body silently ACCEPTS the declaration, which is the actual
defect.

That needs its own ruling (does CFlat have static data members at all - and if not, the struct
body must reject the spelling instead of dropping it) and its own accept set, so it is not folded
into the global-construction fix. Once ruled, whichever way it goes, a global of a C++ class as a
static member has to route through the same module initializer this fix installs.
