# A name mangler for `function<>` / `Lambda<>` types

Status: **Stage -1, Stage 0 layer 0, layer 1 and Stage 2's pointer-depth half are LANDED.** Stage B
(qualified struct keys) is the open remainder; the layer-2 rename is deliberately NOT being done -
see "Layer 2 has no defect behind it any more". Written 2026-08-02 to unblock the parked branch
`fix/funcptr-sig`
(`b2f564b`, worktree `/Users/felixhuang/source/cflat-fix-funcptr-sig`) recorded in
`internal/issue/p1/funcptr-overload-binding-ignores-signature.md`.

The branch proved a function-pointer signature by comparing per-component DESCRIPTORS. It
failed review four times, always the same way: a false rejection. This design replaces the
descriptor comparison with a **canonical name**: mangle both sides, compare the names. The
comparison stops being a bespoke predicate and becomes string equality, which is the only form
that is auditable by reading one function.

## What exists today - four namers, none canonical

| Producer | Shape | Used for | Canonical? |
|---|---|---|---|
| `TypeAndValue::ToUniqueString` (LLVMBackend.h:729) | `cfuncptr_int_int` / `funcptr_...` | overload symbol names via `ComputeMangledName` | no - raw spellings |
| `BuildEncodedClosureName` (MainListener.h:403) | `__thinfn_1_3_int_3_int` / `__fatfn_...` | closure used as a generic ARG (`list<Lambda<int(int)>>`) | no - raw spellings |
| `FuncPtrTypeClass` / `FuncPtrSignatureOf` (LLVMBackend.h:17103) | `iii` | argument-binding proof | yes, but so coarse it proves almost nothing |
| `__c_fn_ptr` / `__closure_fat_ptr` | flavour only | representation dispatch | n/a |

Three measured consequences on `x64/Release/cflat` at `b844137` (repros in `scratch/`):

1. `struct Box<T>`; `Box<int>` and `Box<i32>` emit **two distinct LLVM types** of identical
   layout (`%Box__int = type { i32 }` and `%Box__i32 = type { i32 }`). Monomorphization does not
   normalize type-argument spellings at all.
2. `list<Lambda<int(int)>>` and `list<Lambda<i32(i32)>>` emit
   `%list____fatfn_1_3_int_3_int` and `%list____fatfn_1_3_i32_3_i32` - the same duplication,
   reached through the closure encoder.
3. `int f(function<int(int)>)` and `int f(function<i32(i32)>)` both compile; only
   `_f_int_cfuncptr_int_int_` is ever emitted or called. The second overload is silently
   unreachable. ~~It should be a redefinition error, and a canonical name makes it one.~~
   **Both halves corrected 2026-08-02**: at scalar level BOTH overloads were reachable with
   selection scrambled, and a canonical name does NOT make it an error - no duplicate detection
   existed. FIXED, see below.

(1) is also the open item that parked the branch: the signature proof compared `Box__int`
against `Box__i32` and hard-rejected a program master runs, printing a diagnostic that claimed
two names were two types when they are one.

### (1) is a bug in its own right, and it is the real root cause

`doc/LANGUAGE.md:113` states outright: "`i32` and `int` are the same type and are freely
interchangeable." Monomorphization does not honour that. Measured on `b844137`:

```cflat
struct Box<T> { T v = default; };
int takeInt(Box<int> b) { return b.v; }
extern int main() { Box<i32> q = default; q.v = 7; return takeInt(q); }
```
```
mono_bug2.cb(3,58): no overload of 'takeInt' matches the given arguments.
    [0] Box__i32 <unnamed>          Candidates: _takeInt_int_Box__int_(Box__int b)
```

Same failure for `Box<char>` vs `Box<i8>`. So this is not a funcptr issue at all - the funcptr
proof merely became the first mechanism to notice. Two spellings of one type produce two
generic instantiations that do not interconvert, plus duplicate LLVM types, duplicate method
bodies and duplicate destructors for every generic in the program.

**Fixing it at the source (Stage -1 below) is strictly better than working around it in the
mangler**, and it removes the need for layer 0 to de-mangle and re-canonicalize generic
arguments at all.

## The type lattice being named

```
function<R(A...)>   thin   __c_fn_ptr           8B, no env, non-owning POD, C ABI: R(*)(A...)
Lambda<R(A...)>     fat    __closure_fat_ptr   16B, {code, env}, OWNING value
```

Thin subdivides by how the type was written, **not** by what it is:

- **anonymous** - spelled inline: `function<void(void*, bool)>`
- **named** - reached through an alias: `using WaitCallback = function<void(void*, bool)>;`

**Named and anonymous mangle identically.** A thin function pointer is C-ABI compatible, and in
C `typedef int (*Cb)(int)` and `int (*)(int)` are one type; making the alias nominal would
reject handing a raw signature to a `WaitCallback` slot - a false rejection, the exact failure
mode this area has produced four times. The alias name survives as a **display name** for
diagnostics only, never in the key. (See "Open decision" - this is the one point where the
alternative is defensible.)

## Design

Three layers. Only layer 0 is hard; layers 1 and 2 are composition.

### Layer 0 - `CanonToken(spelling, pointer) -> {Known, Text}`

The canonical token of one signature component: **the type as lowered**, so that every spelling
of one machine type collapses onto one token, and nothing else does.

`Known == false` means NO PROOF IS AVAILABLE. It is not an error and never contributes a
rejection. This is the whole one-sidedness of the mechanism, concentrated in one bool.

Every rule below is either an ABI fact or a measured false rejection from the branch's review
rounds. Do not add a rule without one of those behind it.

| Input | Token | Why |
|---|---|---|
| `void` | `v` | |
| `bool` | `b` | lowers to `i1`, NOT an integer - `define internal i1 @_isPos_bool_int_` called through an `int(int)` pointer reads undefined bits |
| `int`, `i32`, `u32`, `uint` | `i32` | width only. **Signedness is absent**: not part of an `llvm::FunctionType`; including it false-rejected `function<int(int)>` into a `u32(u32)` slot (round 1) |
| `long`, `ulong` | `i32` on Windows, `i64` on LP64 | target-native, via `longBits_` |
| `float` / `double` | `f32` / `f64` | |
| `string` | `Pi8` | normalize to `char*` BEFORE reading the pointer shape - `string` carries `Pointer == false` yet interconverts with `char*` everywhere |
| alias / enum | token of the resolved target | 8-hop chain, as `ResolveFuncPtrTypeSpelling` already does |
| `void*` | `P?` | wildcard over every pointee, matching the scorer's own implicit-conversion rule |
| pointer to X | `P` + token(X) | one `P` per level; see "pointer depth" below |
| registered struct | `S<len>_<qualified key>` | see below |
| generic instantiation | `S<len>_<base>__<canon arg>...` | **type args recursively canonicalized** - this is what makes `Box<int>` and `Box<i32>` one token |
| interface, unsubstituted generic param, unregistered name | `Known = false` | a struct converts to an interface; a `T` is not yet a type |

Token alphabet is `[A-Za-z0-9_]` and every variable-length piece is length-prefixed, so the
token is symbol-safe and collision-free by construction (the property
`BuildEncodedClosureName` already relies on).

**Generic instantiations.** With Stage -1 in place there is nothing to do: `Box<int>` and
`Box<i32>` already arrive as the single spelling `Box__i32`, so the token is just
`S9_Box__i32` on both sides. Without Stage -1, layer 0 would have to split `Box__int` on `__`,
canonicalize each argument and rejoin - and if the split did not round-trip, emit
`Known = false`, biased WIDE on purpose since over-triggering yields more accepts. Either way
this is strictly better than the "subtraction" the issue file proposed (making every generic
instantiation `Known = false`), because it keeps the genuine memory-safety catch -
`Box<double>*` into a `Box<i32>*` slot writes 8 bytes into a 4-byte object - instead of
surrendering it.

**Structs and the qualified-key problem.** A signature currently stores the raw
`ts->getText()`; no namespace resolution runs on that path, so a bare `Pt` written inside
`namespace NS` arrives as `"Pt"` and could mean global `Pt` or `NS.Pt`. A single token cannot
represent that honestly:

- Picking one key is a guess (memory-unsafe when wrong).
- Treating multi-candidate as `Known = false` is **defeatable**: one unreferenced
  `namespace Zz { struct SquareP; }` anywhere in the program or its imports disarms the proof.
  Round 2 shipped this; round 3 caught it.

So the struct token is **staged**:

- **Stage A** - the token is set-valued: `Known = true`, `Text = "S"`, plus the sorted candidate
  key set (`FuncPtrStructCandidates` on the branch, keep it verbatim). Two struct tokens differ
  only when their sets are DISJOINT. Everything else is a plain string.
- **Stage B** - resolve the spelling to a qualified key at the DECLARATION site, where the
  namespace context is known, and store the qualified key in `FuncPtrParam.TypeName`. The set
  collapses to one element, the token becomes a plain string, and the set machinery is deleted.

Stage B is the point of this design: it is what turns the mangled name into a real *name*.
Stage A exists only so the mangler can land before that refactor.

### Layer 1 - the thin key, through the existing function-name mangler

```
FnSig(T) = ComputeMangledName("", canon(ret), { canon(p) for p in params })
```

Literally `LLVMBackend::ComputeMangledName`, fed `TypeAndValue`s whose `TypeName` is the layer-0
token - same composition function, canonical inputs. `function<int(int)>` yields `__i32_i32_`.

**Why not reuse `ToUniqueString` directly.** Two different equivalence relations are in play and
conflating them is the trap:

- `ToUniqueString` is **source identity**: it must keep `int` distinct from `u32` so
  `f(int)` and `f(u32)` stay separate overloads. ~~Unchanged by this design - no symbol names
  move, no `--init` cache invalidates.~~ **WRONG, corrected 2026-08-02**: it had to be routed
  through the same source canon as monomorphization, symbol names moved, and that turned out to
  BE layer 1. See "What layer 1 actually turned out to be". The `int` vs `u32` half of the
  sentence stands and is preserved.
- `CanonToken` is **ABI identity**: it must merge them, because that is the only question
  binding may reject on.

`FnSig` carries **no thin/fat flavour**: a thin `function<>` widens into a fat `Lambda<>`
parameter of the same signature (`WidenThinToFat`), so the flavour must not participate in
binding. Flavour lives one layer up.

### Layer 2 - flavoured type names; `Lambda` is a monomorphization

```
ThinName(T) = "__c_fn_ptr" + FnSig(T)                    // replaces __thinfn_...
FatName(T)  = MangledGenericName("Lambda", { FnSig(T) }) // = "Lambda__" + FnSig(T)
```

The fat closure is named exactly as any other generic instantiation, through the existing
`MangledGenericName` / `MangleTypeArg` path, with the thin signature key as its single type
argument. `list<Lambda<int(int)>>` becomes `list__Lambda___i32_i32_`, and
`list<Lambda<i32(i32)>>` becomes the same name - defect (2) above closes as a consequence, not
as a separate fix. `BuildEncodedClosureName` and `encodedClosureTypes_` keep their role and
change only which string they key on.

### The comparison rule

```
ProvablyDifferent(a, b) :=
    arity(a) != arity(b)
    OR exists i: Known(a_i) AND Known(b_i) AND TokensDiffer(a_i, b_i)
```

where `TokensDiffer` is string inequality, except that two set-valued struct tokens differ only
when disjoint (Stage A only), and `P?` (`void*`) never differs from another pointer token.

Once Stage B lands this degrades to: reject iff both keys are fully known and the strings
differ. That is the readable end state.

**Polarity, restated because this is where four rounds died:** prove what you REJECT, accept
what you cannot prove. An unknown component on either side accepts. A relaxation is always safe;
a tightening is never safe without a measured before/after.

## Pointer depth - DONE 2026-08-02

`ParseDeclarationSpecifiers` recorded `bool pPtr = param->pointer() != nullptr`, collapsing `*` and
`**`, so `function<void(int**)>` bound a `function<void(int*)>` slot and SIGSEGV'd - on master too.
Closed by `FuncPtrParam::PointerDepth` + `TypeAndValue::FuncPtrReturnPointerDepth`, set at the six
source-parse sites via `PointerDepthOf`/`ReconcilePointerDepth` (the flag wins over the written
star count, because `string` resolves to `char*` with no `*` written), with the `--init` round-trip
in the same change. The issue file is deleted.

**0 means NOT RECORDED, never "not a pointer"** - `Pointer` already answers that, and producers
that cannot count stars (C interop, WinRT, synthesized signatures) leave it 0. Readers treat 0 as
unknown, so the depth proof is one-sided like every other component. Depth reaches four consumers:
the comparator, `ToUniqueString` (one `Ptr` per level - required, or the duplicate-definition
diagnostic false-fires on two genuine overloads), `BuildEncodedClosureName` (whose component pairs
became `<name, int depth>`; a caller passing a bool still yields the pre-depth string), and
`FuncPtrSpellingOf`, so a diagnostic now reads `void(int**)` vs `void(int*)` instead of two
identical-looking spellings.

## Open decisions

1. ~~**Named thin types: structural or nominal?**~~ **SETTLED 2026-08-02: structural.** A
   `using` alias naming a thin function type mangles identically to the inline signature; the
   alias name is display-only. C-typedef semantics, and no new false-rejection axis.
2. **`move` on a parameter** (`FuncPtrParam::IsMove`). It is an ownership contract, not an ABI
   fact. Default: present in `ThinName`/`FatName` (it changes semantics, so it is part of the
   type) but NOT a rejection axis (`HasFunctionWithMoveFlags` already handles the binding side).
3. **Calling convention.** `stdcall` is genuinely part of a C function type on 32-bit x86 and
   Win32 callbacks are marked with it. Default: a token in the key, absent when Default. Verify
   against `core/ui_native/` before committing - a rejection axis there is high-traffic.

## Staging

- **Stage -1 - DONE 2026-08-02.** `CanonicalPrimitiveSpelling` (MainListener.h, next to
  `MangleTypeArg`) folds `int`->`i32` and `short`->`i16` on the peeled base. `Box<int>` and
  `Box<i32>` are now one instantiation (`%Box__i32`), and `list<Lambda<int(int)>>` /
  `list<Lambda<i32(i32)>>` collapsed to one container as a side effect - defect (2) closed for
  free, exactly as predicted. Two error tests asserted on the old mangled name in a diagnostic
  (`GLive__int`, `list__int`) and were updated to the new one; the assertion is unchanged.
  Suite 576 passed / 0 failed / 8 skipped, examples 35 / 0.

  Regression coverage: `testGnCanonicalPrimitiveTypeArg` in `Test/test_generics.cb` (nine value
  legs - cross-spelling parameter binding, cross-spelling assignment, `list<i32>` into a
  `list<int>` parameter, plus four CONTROL legs proving `i32`/`u32` and `i64`/`long` stayed two
  distinct instantiations, via overloads that would collide if they had merged);
  `testClosureListSpellingUnify` in `Test/test_function_ptr.cb` pins the container identity
  through a pointer parameter.

  **Residual: `char` vs `i8` is still two instantiations** (`Box<char>` does not bind a
  `Box<i8>` parameter). Deliberate - `char` carries `DW_ATE_signed_char` where `i8` carries
  `DW_ATE_signed`, so folding them would make a debugger print numbers instead of characters,
  and which spelling won would depend on registration order. `long`/`ulong` likewise, for the
  target-width reason below.

  The original plan for this stage follows.

- **Stage -1 (original plan)** - normalize primitive type-argument SPELLINGS at
  monomorphization, so `Box<int>` and `Box<i32>` are one type everywhere. Chokepoint is
  `MangleTypeArg` (MainListener.h:317): it is the single funnel both `MangledGenericName` and
  every direct caller in **both** passes go through, which satisfies the both-pass rule. Apply
  the map to the peeled BASE only - after `StripOwnershipQualifiers` and after the `*` / `[]`
  suffix is separated - by exact match, so a struct named `Int` and a type named `integer` are
  untouched.

  The equivalence classes here **preserve signedness** and are therefore NOT the layer-0 ABI
  canon. Two different canonicalizations, deliberately:

  | Normalize | To | |
  |---|---|---|
  | `char` | `i8` | documented same type |
  | `short` | `i16` | |
  | `int` | `i32` | `doc/LANGUAGE.md:113` |
  | `u8`/`u16`/`u32`/`u64`/`i64` | themselves | distinct types, not aliases of the signed ones |
  | `long` / `ulong` | **left alone** | target-native width. Mapping `long`->`i64` would make `Box<long>` and `Box<i64>` one type on macOS/Linux and two on Windows, i.e. platform-varying symbol names. Under-normalizing here is the current behaviour, so it regresses nothing |

  This is a TIGHTENING in exactly one place: a program with both `f(Box<int>)` and
  `f(Box<i32>)` overloads starts colliding. That is the correct diagnosis - today one of them
  is silently unreachable, which is defect (3) in generic clothing. Everything else strictly
  gains: half the LLVM types, half the method bodies and destructors, and the documented
  interchangeability actually holds.

  Risk to verify: generic substitution maps are keyed on the argument STRING, so after
  normalization the second instantiation is skipped as already-registered and `T` resolves to
  whichever spelling registered first. That is sound (they are one type) but it is where a
  regression would hide - check with value legs across `list<int>` / `list<i32>` /
  `dictionary<string, int>`. Symbol names move, so `--init-clear` before the suite run.
- **Stage 0 - LAYER 0 DONE 2026-08-02; layers 1-2 not started.** The parked branch
  `fix/funcptr-sig` (`b2f564b`) was brought onto master - `FuncPtrComponent`,
  `FuncPtrComponentOf`, `ComponentsProvablyDiffer`, `FuncPtrStructCandidates`,
  `DescribeFuncPtrSignatureMismatch`, its ten reject legs and ten value legs - and its one open
  blocker was closed on the way in.

  Measured, before and after, on the four generic-pointee crossings:

  | | branch `b2f564b` | now |
  |---|---|---|
  | `Box<int>*` -> `Box<i32>*` slot | hard error (false) | binds, `g1=51` |
  | `Box<int>*` -> `Box<u32>*` slot | hard error (false) | binds, `r=51` |
  | `Box<long>*` -> `Box<i64>*` slot | hard error (false) | binds, `r=51` (LP64) |
  | `Box<double>*` -> `Box<i32>*` slot | rejected (correct) | **still rejected** |

  Stage -1 closed only the first row; signedness and the target widths are deliberately kept by
  monomorphization, so rows 2 and 3 needed `FuncPtrAbiCanonKey` - the ABI canon applied to the
  registered struct key, folding every `__`-separated scalar token onto its lowered token. That
  is layer 0's "generic instantiation -> recursively canonicalize type args" rule, arrived at
  through the mangled key rather than the source spelling.

  It is a MAP over the candidate set, never a filter. The rule rejects on DISJOINT sets, so a map
  applied to both sides can only remove rejections; a filter could invent them. Stated in the
  code comment so it is not "optimized" back.

  The memory-safety catch is NOT surrendered - row 4 is the whole reason to prefer this over the
  subtraction the issue file proposed. Pinned by a reject leg at the end of
  `Test/errors/err_data_pointer_to_closure_param.cb` and three value legs in
  `Test/test_function_ptr.cb`, each with a live `double` sibling arm so they test the scorer.

  Suite 576 / 0 / 8, examples 35 / 0. Residual 3's recorded measurement (`neigh=2333`) is
  unchanged, confirming the map did not disturb the namespace-ambiguity axis.

  **What is left of Stage 0**: layers 1 and 2 - actually emitting a NAME (`FnSig` through
  `ComputeMangledName`, `FatName` through `MangledGenericName`) and retiring
  `BuildEncodedClosureName` onto it. Today the mechanism is still a component comparator that
  happens to be canonical, not a mangler; the comparison is `ComponentsProvablyDiffer` rather
  than string equality. That refactor moves symbol names, so it wants its own change.
- **Stage 1 - LAYER 1 DONE 2026-08-02; the layer-2 rename is NOT done and should not be done as
  written.** See "What layer 1 actually turned out to be" below.
- **Stage 2 - pointer depth DONE 2026-08-02** (+ its `--init` round-trip), together with the
  duplicate-overload diagnostic for defect (3) and the named-function proof at the declaration and
  argument sites. Still open: qualified struct keys at the declaration site (Stage B) and deleting
  the struct sets.

## What layer 1 actually turned out to be (2026-08-02)

Layer 1 was specified as "compose a canonical name through `ComputeMangledName`", with the explicit
note that `ToUniqueString` stays **unchanged** - "no symbol names move, no `--init` cache
invalidates." **That note was wrong, and it was the whole fix.** Stage -1 folded `int`->`i32` at
MONOMORPHIZATION but left `ToUniqueString` keeping them apart, so the compiler held two different
answers to "are these the same type?". Measured consequence, worse than defect (3) as filed - not
"one overload is silently unreachable" but both reachable with selection scrambled:

```cflat
int f(int x) { return 100 + x; }
int f(i32 x) { return 200 + x; }
// f(1) printed 201; an i32 VARIABLE printed 102. Each spelling reached the other's body.
```

So layer 1 is: **route the overload identity through the same source canon monomorphization uses.**
One map (`CanonicalPrimitiveSpelling`, now in `LLVMBackend.h`), two identities funnelling through
it. Symbol names DO move. Signedness stays out of it - this is source identity, not the ABI canon.

**Defect (3)'s stated remedy was also wrong.** "It should be a redefinition error, and a canonical
name makes it one" - it does not. There was no duplicate detection anywhere:
`CreateFunctionDefinition` skipped a second body with a `[verbose]` line, so `int f(int)` written
twice compiled and ran the first body. The canonical name only routes both spellings into that same
silent skip. The diagnostic is a separate mechanism, and it had to ship WITH pointer depth: without
depth, `f(function<void(int*)>)` and `f(function<void(int**)>)` mangle alike, so the new diagnostic
reported two genuine overloads as a redefinition.

### Layer 2 has no defect behind it any more - do not do it as written

`ThinName` / `FatName` replacing `BuildEncodedClosureName` was justified by defect (2), the
`list<Lambda<int(int)>>` duplication. **Stage -1 already closed that.** What remains of the layer-2
proposal is a rename (`__fatfn_1_3_i32_3_i32` -> `Lambda___i32_i32_`) that moves monomorphization
keys and symbol names for no measured behaviour change, and it cannot deliver its stated payoff -
turning `ComponentsProvablyDiffer` into string equality - while struct components are set-valued
(Stage B is not done) and while unknown components and unrecorded depths must stay one-sided. By
this plan's own rule ("Do not add a rule without one of those behind it"), it is not justified.

**What IS still open in this area is layer 0's ALIAS rule, applied to the NAME producer.** The
comparator resolves aliases (`ResolveFuncPtrTypeSpelling`); the name producers do not. Measured
2026-08-02:

```cflat
using MyInt = int;
list<Lambda<int(MyInt)>>  // __fatfn_1_3_i32_5_MyInt
list<Lambda<int(int)>>    // __fatfn_1_3_i32_3_i32   - two types, do not interconvert
list<MyInt>               // list__MyInt vs list__i32 - the SAME gap, no closure involved
```

Note the third line: this is **not** closure-specific. It is defect (1) in alias clothing, sitting
in `MangleTypeArg`, and it affects every generic. Fixing it is a Stage -1-shaped change (resolve
type aliases before mangling, restricted to pure renames so `using Handle = void*` is untouched)
with a blast radius across all monomorphization - not a closure-naming refactor. It deserves its
own decision, not to be smuggled in under "layer 2".

## Verification

Every stage is verified with **value legs, not compile-success**. A relaxation restores a
candidate to the perfect tier and can silently change WHICH overload is selected; a leg that
only asserts "it compiles" cannot see that. Green `./test.sh Release` is the bar on this host.

Related: `internal/issue/p1/funcptr-overload-binding-ignores-signature.md`,
`internal/issue/p1/funcptr-pointer-depth-not-compared.md` (branch only),
the closure-generics-monomorphization fix (landed; compiler bug fix, no doc successor),
`internal/fix-issue-lessons.md`.
