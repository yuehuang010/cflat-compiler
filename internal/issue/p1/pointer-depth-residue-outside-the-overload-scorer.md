# `T**` still binds a `T*` parameter everywhere the overload scorer is not the judge

## RULING 2026-08-10 (maintainer) - section 5: CARRY REAL DEPTH through substitution. LANDED.

Implemented; section 5 below is CLOSED. `PeelTypeArgSuffix` now accumulates an int depth, both
type-argument spellings (written and substituted) count stars, and `TypeAndValue` no longer carries
`PointerDepthUnknown` at all - the flag had exactly one producer and it is gone. Sections 2, 3,
4's value-side residue and 7 remain open, which is why this file stays.

The ruling: change `PeelTypeArgSuffix`'s single `bool& pointer` to an int DEPTH and let it reach
`MangleTypeArg` intact, so `Box<C**>` mangles `Box__Cptrptr`, becomes its own instantiation, and
the depth gate can claim proof through generics.

**This was a wrong NAME being corrected, not a new naming scheme.** The mangler already expressed
this distinction everywhere else; generic type arguments were the ONLY path that lost it. Measured
on the pre-fix Release binary (`scratch/pd_mangle.cb`):

| path | depth preserved? | evidence |
|------|------------------|----------|
| function parameter mangling | **yes** | `@_byPtr_i32_CPtr_` vs `@_byPtrPtr_i32_CPtrPtr_` |
| closure encoded name | **yes** | `BuildEncodedClosureName` (`MainListener.h:460`) threads an int depth per param and re-appends the stars before mangling |
| `MangleTypeArg` itself | **yes** | `MainListener.h:357` appends `"ptr"` once PER STAR, so `C**` renders `Cptrptr` |
| `PeelAliasPointerStars` | **yes** | `MainListener.h:513` - same job, returns an int |
| generic type argument | **was NO, now yes** | `PeelTypeArgSuffix` popped every star into ONE bool; it now accumulates an int depth |

Pre-fix, `Box<C*>` and `Box<C**>` in one program emitted a single instantiation
`@_Box__Cptr_Box__Cptr__` with one `@_put_i32_Box__CptrPtrCPtr_`, i.e. the `T = C**`
instantiation's method genuinely took a `C*`. The depth was destroyed before the mangler ever saw
it; handed the right string the mangler already produced the right answer.

**Work (done):** the int is threaded through substitution and the downstream consumers of the
collapsed flag (`Pointer` / `ElemPointer` / `PointerDepth` on the substituted parameter) are fixed.
Splitting the instantiation was the POINT: a differential sweep of the whole `Test/` and `example/`
corpus on the pre- and post-fix binaries shows ZERO divergence outside the section-5 cells.

**Alternative considered and rejected:** refusing 2+ stars in a type ARGUMENT. Rejected because it
permanently forbids a spelling the mangler can already name correctly, to avoid fixing one boolean.
`Box<C**>` therefore stays legal; only the 3-star spelling is refused, which is section 6's cap
reaching a spelling that now counts stars.

Sections 2, 3, 4's value-side residue and 7 contain no design fork - they are engineering and can proceed independently.
Section 3's interface door keeps its own precondition: verify on WINDOWS before gating there,
because WinRT synthesizes interface parameters without setting `ElemPointer`, and a true `T**`
recorded `ElemPointer=false` would be a Windows-only false rejection.

Residue split out of `double-pointer-arg-binds-single-pointer-param.md` when the depth gate landed
in `TypeAndValue::IsTypeMatch`. That gate closed the direct-call / method / constructor cells and
the overload-collision cell. Every row below was MEASURED on the post-fix Release binary
(macOS arm64) and still compiles, links and runs, reading the low bytes of a heap address.

**Exit codes below are environment-dependent** (they track the process image, not the build) - do
not read a change in one as a change in behaviour, and never pin one in a test. `2003` / `103` are
the only correct answers where they are named.

Repro corpus: the `pd_r*` files in the fix branch's `scratch/pd/`, reproduced inline here.

## 1. Argument-side depth is NOT RECORDED for an inline `&` - CLOSED

Closed by the `TypeAndValue::PointerDepth` change that landed the mirror gate. `&` now adds one
level to an ALREADY-RECORDED depth, so `&a` over a declared `Circle*` is a proven `Circle**`.
Both cells were re-measured on the post-fix binary and now hard-error with the depth note
(`pd_r01` / `pd_r05`, previously rc 176 each); both are frozen as `expect_error` legs in
`Test/errors/err_double_pointer_arg_single_pointer_param.cb`. Selection changed with them:
`pdPick(&a)` over `{f(T*), f(T**)}` now picks the `T**` overload (`pd_addrof_ptr_arg_picks_ptrptr_overload`,
2 where it used to read garbage through the `T*` body).

Everything below was re-measured on that same binary and is STILL OPEN.

## 2. A PRIMITIVE pointer argument never reaches `IsTypeMatch` at all

A primitive pointer argument carries an EMPTY CFlat `TypeName`, so the scorer takes the
`CompareUpconvert` branch, where opaque pointers make every pointer pair identical.

```cflat
int deref(int* p) { return 100 + *p; }
extern int main() { int x = 3; int* p = &x; int** pp = &p; return deref(pp); } // pd_r15, garbage rc
```

Same shape as the by-value family's `PointerArgIntoByValuePrimitiveParam`, which exists for
exactly this reason; a depth twin needs a positive proof that does not read `TypeName`.

## 3. Three ARGUMENT-position doors bypass the scorer entirely

Each was measured broken; none routes through `ComputeOverloadFunction`. This is the
"an argument-position gate has four doors" inventory in `internal/fix-issue-lessons.md`.

- Interface dispatch (`ResolveInterfaceMethodSlot` / `CallInterfaceMethod`), `pd_r16`, rc 132:
  `interface IUse { int use(Circle* c); } ... i.use(pp)`.
- Operator overload, `pd_r17`, rc 100: only the RECEIVER reaches the scorer; the operand does not.
  `class Vec { int operator+(Circle* o) ... } ... v + pp`.
- Indirect `function<>` call (`CreateIndirectCall`), `pd_r18`, rc 36:
  `function<int(Circle*)> f = byPtr; ... f(pp)`.

The interface door already has the pattern to copy - `PointerArgIntoByValueParam` +
`DiagnoseProvableInterfaceArgMismatch` in `LLVMBackend_WinRT.cpp`. It was NOT taken in the same
change because WinRT synthesizes interface parameters without ever setting `ElemPointer`, and that
path cannot be exercised on a macOS host: a parameter that is truly `T**` but recorded
`ElemPointer=false` would be a Windows-only false rejection. Verify on Windows before gating there.

## 4. Depth caps at 2, so `T***` into `T**` is invisible - DECLARATOR HALF CLOSED

The WRITTEN half is closed together with section 6, by REMOVING the spelling rather than lifting
the cap: a written depth of 3+ is now a hard error at the site where the stars appear.
`byPP(ppp)` no longer compiles - `Circle*** ppp` is refused at its declarator. The old value leg
`pd_deref_of_triple_ptr_into_ptrptr_param` is gone with it (replaced by
`pd_ptrptr_local_at_cap_reads_through` in `Test/test_basic.cb`); the refusals are frozen in
`Test/errors/err_double_pointer_arg_single_pointer_param.cb` and its mirror.

Still open: a depth-3 VALUE needs no declarator. `&pp` over a `Circle** pp` produces one, and the
value side still proves it as `Circle**`, so it binds a `Circle**` parameter silently:

```cflat
int byPP(Circle** c) { return 2000 + (*c)->r; }
extern int main() { Circle c = default; Circle* a = &c; Circle** pp = &a;
                    return byPP(&pp); }        // compiles; garbage rc (2007 expected)
```

Measured identically on the pre- and post-guard binaries, so this is untouched residue, not a
regression. `Circle** q = &pp;` is accepted the same way. Closing it is value-side depth work
(`ValuePointerDepth` over address-of), not another written-star guard.

## 5. Generic substitution collapses depth - CLOSED

`PeelTypeArgSuffix` takes an `int& pointerDepth` and accumulates one level per star; both places
that build a type-argument string count stars instead of flagging them (`ResolveTypeArgEntry`,
`ForwardRefScanner::ResolveForwardTypeArg` - the two MUST stay identical or the shell name and the
instantiation name diverge), as do the tuple-element sites. The substituted declarator combines
that real depth with the declarator's own stars, so `Box<C**>::put`'s `T x` is a proven `C**`.
`PointerDepthUnknown` is deleted: it had one producer and every `IsProven*` predicate now judges
substituted pointers directly.

Measured (`scratch/pdg5/`, pre = merge-base Release binary, post = this branch):

| cell | pre | post |
|------|-----|------|
| `Box<C*>` + `Box<C**>` in one program | ONE instantiation `Box__Cptr`, `put` takes `C*` | `Box__Cptr` **and** `Box__Cptrptr`, `put` takes `C*` / `C**` |
| `idput<C**>(pp)` | `@_idput__Cptrptr_i32_CPtr_` (name split, param collapsed) | `@_idput__Cptrptr_i32_CPtrPtr_` |
| `list<C**>` / `dictionary<int,C**>` | `list__Cptr` / `dictionary__i32__Cptr` | `list__Cptrptr` / `dictionary__i32__Cptrptr` |
| `Box<Box<C**>>`, `Pair<C*,C**>`, `Box<int**>` | inner arg collapsed | depth preserved at every level |
| overload on `Box<C*>` vs `Box<C**>` | "redefinition" (one type) | two overloads, each picked correctly |
| `Box<C*>.put(pp)` (a real mistake) | compiled | refused with the depth note |
| `dpByPtr(deep.get())` | compiled, garbage | refused with the depth note |
| `Box<C***>` | collapsed to `Box<C*>`, compiled | hard error - the type-argument spelling now COUNTS stars, so it joins section 6's cap |
| `Box<C*>`, `list<C*>`, `Box<int>`, `list<string>` | accept | accept, byte-identical mangling |

The four `pdg_*` accept-set programs still compile and now produce CORRECT results; seven value
legs and three `expect_error` legs are frozen (`Test/test_generics.cb`,
`Test/errors/err_double_pointer_arg_single_pointer_param.cb`). The 3-star cap is applied to the
WRITTEN type-argument stars only: a substitution that composes past the cap (`list<C**>` feeding
its own `T* _data`) must stay legal, so it clamps silently and claims no depth, exactly as it did.

Still collapsed, NOT part of this section: `ResolveSigComponentCodegen` folds a substituted
component's stars into a bool, so `function<void(T)>` with `T = C**` encodes depth 1. The scanner
counterpart sees no substitutions at all, so making it count would need both passes changed
together. Concrete effect (review probe): a `function<int(T)>` member of `Box<C**>` is typed
`int(C*)`, so binding a correct `int(C**)` function to it is refused. Pre-existing, unchanged by
this fix.

Also still collapsed: a `using`-declared FUNCTION-TYPE alias used as a type argument re-encodes as
`encodedAlias + "*"` (one star regardless of depth, `MainListener_Declarations.cpp:125`), so
`Box<Fn*>` and `Box<Fn**>` remain one instantiation while `Box<C*>` / `Box<C**>` split. The forward
scanner now emits the real star count for that same entry, so the two passes name it differently -
they already did before this fix (the scanner names the alias, codegen the encoding), and a probe
round-tripping an `Fn**` through such a box behaves identically on both binaries.

## 6. Depth 3 truncates SILENTLY in the plain spelling - CLOSED

Ruling: **3+ written stars are a hard error at every spelling that counts stars**, matching what
the alias branch already did. The type model holds two levels (`Pointer` + `ElemPointer`); anything
deeper could only be clamped, and a clamped value is worse than no value - it is the thing that
made both of the overload dump's remedies wrong advice. The cap is now enforced instead of
silently applied, so the asymmetry between the plain and alias spellings is gone.

Sites (all in `cflat/MainListener_Declarations.cpp`, message built by `PointerDepthCapMessage` in
`cflat/MainListener.h`):

- a pre-loop guard at the head of `MainListener::ParseDeclarationSpecifiers` - fires before any
  branch consumes the stars, so it covers local / parameter / field / global / return-type / cast
  and the simd and generic branches at once (each of those clamped silently before);
- both `function<>`/`Lambda<>` signature sites (return and parameter), whose stars ride the
  signature rather than a declarator;
- the `using` alias RHS, so an alias naming an unspellable type is refused at its declaration and
  not only at a use site that may never be written.

`ForwardRefScanner::ParseDeclarationSpecifiers` deliberately keeps clamping silently, exactly as it
already did for the alias cap: the pre-pass is opportunistic and must not pre-empt the codegen
pass's diagnostic ordering. Consequence: a `T***` inside a template that is never instantiated is
not reported (uninstantiated template bodies are not checked at all).

Accept set re-measured after the guard, all unchanged: `C*`, `C**`, `int**`, `void**`,
`using CP = C*; CP* x;` (depth 2), `C*[N]` slots and `&arr[0]`, `Box<C**>`, plus the whole suite
(650/0/8) and `example_mac.sh` (35/0).

## 7. A `T*[N]` fixed array binds a `T*` parameter

Adjacent, and NOT a depth-of-this-value question - `Pointer`/`ElemPointer` on an array describe its
ELEMENT (`MainListener.h:3891`), which is why the landed gate excludes arrays and views.

```cflat
int firstR(Circle* c) { return 90 + c->r; }
extern int main() { Circle*[2] arr = default; arr[0] = new Circle(); arr[0]->r = 7;
                    return firstR(arr); }         // pd_a27; correct 97, measured 122 here
```

The measured value is garbage that tracks the process image, not the build: the same program on
ONE binary written to four different paths produced 26/154/186/218 when this was first filed, and
122 when re-measured. The standing warning about environment-dependent exit codes, demonstrated.
