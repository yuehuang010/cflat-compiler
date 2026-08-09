# A global's seeded value depends on whether its type was already emitted

Filed 2026-08-09 by review of the global-default-seeding fix (commit
"Give a global the constant value of its own default construction").

Severity: silent wrong value with a located note. Two textually identical
declarations of the same global fold differently depending on code ELSEWHERE in
the file, which is the part that makes this worth a file of its own rather than
a line in the fix's out-of-scope list.

## Repro

Measured on the fix binary. The two programs differ only by an unrelated
function above the global:

`scratch/rgs_order_a.cb` - the global comes first:

```cflat
struct Box2<T> { T item = default; int tag = 55; };
Box2<int> gb;
extern int main(){ Box2<int> lb; printf("G tag=%d L tag=%d\n", gb.tag, lb.tag); return 0; }
```

-> `G tag=0 L tag=55`, plus
`rgs_order_a.cb(2,10): (Box2__i32) global is zero-initialized: its default construction could not be reduced to a compile-time constant here.`

`scratch/rgs_order_b.cb` - one unrelated use of `Box2<int>` first:

```cflat
struct Box2<T> { T item = default; int tag = 55; };
int warm(){ Box2<int> w; return w.tag; }
Box2<int> gb;
extern int main(){ Box2<int> lb; printf("G tag=%d L tag=%d\n", gb.tag, lb.tag); return 0; }
```

-> `G tag=55 L tag=55`, no note.

The non-generic twin is the same defect spelled with declaration order
(`scratch/gs_17_fwd.cb`): a global whose struct type is defined BELOW it reads
`G a=0` while the local twin reads 17.

## Root cause

The fold runs at the DECLARATION site and needs the type's constructor to
already be a defined `llvm::Function` - `FoldConstructedValueToConstant` bails
on `callee->isDeclaration()`. A generic instantiation's constructor body is
emitted on first use, and a forward-referenced struct's is emitted when the walk
reaches the definition, so in both orderings the constructor is not there yet
and the global keeps the zero default.

## Fix direction

Defer instead of giving up: record the pending global
(`llvm::GlobalVariable*` + its `DeclTypeAndValue` + the declaration context) and
flush at the end of the file walk, calling `setInitializer` on the ones whose
constructor exists by then, and emitting the note only for the ones still
unfoldable at that point. `setInitializer` on an already-created global is
supported, so no new emission mechanism is needed.

Two things to settle before implementing: how the deferred flush interacts with
imported modules (an import's globals are walked in the importing compile), and
whether the flush point survives the `--init` warm-cache path, where core
constructors arrive as loaded bitcode rather than freshly emitted IR.

## Related, same file by decision

**A folded constructor's side effects are dropped, and the fold accepts calls
the direct global spelling rejects.** `FoldConstructedValueToConstant` reads a
callee's `ret` operand and ignores the rest of its body, so a field default that
CALLS something folds to the callee's constant return while the call itself
never happens. Measured (`scratch/rgs_sidefx.cb`):

```cflat
int counter = 0;
int bump(){ counter = counter + 1; return 3; }
struct SFX { int a = bump(); };
SFX gs;
```

-> `G a=3 counter=1` on the fix binary. `counter` is 1, not 2: only the LOCAL
construction ran `bump()`; the global took the value 3 and dropped the call.
Pre-fix the global read `G a=0 counter=1` - the effect was already dropped, so
this is not a new lost side effect, but the plausible-looking value now hides it.

The asymmetry: the direct global spelling of the same call is a hard error -
`int g = bump();` gives `global variable initializer must be a compile-time
constant`, exit 1 (`scratch/rgs_sidefx_direct.cb`). So one spelling rejects the
call and the neighbouring one silently constant-folds it.

Filed here rather than separately because both faces are about the fold running
at the wrong TIME relative to the rest of the walk. A conservative tightening
worth measuring alongside the deferred flush: refuse to fold a callee whose body
contains anything but the instructions the walk understands, so a constructor
that does real work reaches the note instead of a fabricated constant.
