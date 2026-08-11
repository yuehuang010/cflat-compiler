# `T**` still binds a `T*` parameter everywhere the overload scorer is not the judge

## RULING 2026-08-10 (maintainer) - section 5: CARRY REAL DEPTH through substitution.

Change `PeelTypeArgSuffix`'s single `bool& pointer` to an int DEPTH and let it reach
`MangleTypeArg` intact. `Box<C**>` then mangles `Box__Cptrptr` and becomes its own instantiation,
and the landed depth gate can claim proof through generics instead of declining with
`PointerDepthUnknown`.

**This is a wrong NAME being corrected, not a new naming scheme.** The section-5 text below frames
it as "changes the monomorphization key ... a much larger change than this issue". That
over-states it: the mangler already expresses this distinction everywhere else, and generic type
arguments are the ONLY path that loses it. Measured on the current Release binary
(`scratch/pd_mangle.cb`):

| path | depth preserved? | evidence |
|------|------------------|----------|
| function parameter mangling | **yes** | `@_byPtr_i32_CPtr_` vs `@_byPtrPtr_i32_CPtrPtr_` |
| closure encoded name | **yes** | `BuildEncodedClosureName` (`MainListener.h:460`) threads an int depth per param and re-appends the stars before mangling |
| `MangleTypeArg` itself | **yes** | `MainListener.h:357` appends `"ptr"` once PER STAR, so `C**` renders `Cptrptr` |
| `PeelAliasPointerStars` | **yes** | `MainListener.h:513` - same job, returns an int |
| generic type argument | **NO** | `PeelTypeArgSuffix` (`MainListener.h:481`) pops every star into ONE bool |

`Box<C*>` and `Box<C**>` in one program emit a single instantiation `@_Box__Cptr_Box__Cptr__` with
one `@_put_void_Box__CptrPtrCPtr_`, i.e. the `T = C**` instantiation's method genuinely takes a
`C*`. **The depth is destroyed before the mangler ever sees it** - by mangling time `C**` has
already become `C*`. The naming half needs no design work; it already produces the right answer
when handed the right string.

**Work:** thread the int through substitution, then fix the downstream consumers of the collapsed
flag (`Pointer` / `ElemPointer` on the substituted parameter, `MainListener_Declarations.cpp:669`)
and the `typeParameterEntry` walkers. Splitting the instantiation is the POINT, not a side effect -
but its blast radius is the part to scope before writing code.

**Alternative considered and rejected:** refusing 2+ stars in a type ARGUMENT (matching section 6's
ruling for written stars). It is a much smaller guard and costs only the four synthetic `pdg_*`
legs - two-star type arguments have ZERO organic use, all four occurrences repo-wide being in
`Test/test_generics.cb` where they were added to pin this capability. It was rejected because it
permanently forbids a spelling the mangler can already name correctly, to avoid fixing one boolean.

Sections 2, 3, 4 and 7 contain no design fork - they are engineering and can proceed independently.
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

## 5. Generic substitution collapses depth, so the gate is blind through templates

`PeelTypeArgSuffix` (`cflat/MainListener.h:472`) records a type argument's stars as ONE bool, and
`MainListener_Declarations.cpp:669` carries that one bit forward, so the substituted `T x`
parameter of `Box<C**>::put` records `Pointer=1, ElemPointer=0` - byte-identical to a hand-written
`C* x`. `Box<C*>` and `Box<C**>` are also ONE instantiation: both mangle to `Box__Cptr`.

The landed gate therefore marks any substitution-produced pointer `PointerDepthUnknown` and
declines to claim proof on it, in BOTH directions. Without that it hard-errored four programs the
merge base runs correctly - `Box<C**>.put(pp)`, `list<C**>.add(pp)`, `dictionary<int,C**>.add(1,pp)`
and a generic function `idput<C**>(pp)` (found by review, not by the suite; now frozen as the
`pdg_*` value legs in `Test/test_generics.cb`).

The section-6 cap guard deliberately does NOT extend to the type-ARGUMENT spelling: `Box<C***>`
still compiles and still collapses to `Box<C*>`, because `typeParameterEntry`'s stars are already
collapsed to one bool for `C**` too, so rejecting only three stars there would fix nothing while
adding a guard to every walker of that rule. Measured post-guard (probe `a7_generic`): accepts.

Consequence: a genuine `T**`-into-`T*` mistake is INVISIBLE anywhere a generic type argument is the
parameter's type. Closing it means carrying a real depth int through substitution, which changes
the monomorphization key (`Box<C*>` and `Box<C**>` would stop being the same instantiation) - a
much larger change than this issue, and the reason the one-sided decline was taken instead.

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
