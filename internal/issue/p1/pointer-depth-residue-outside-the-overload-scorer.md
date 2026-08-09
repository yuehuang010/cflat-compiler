# `T**` still binds a `T*` parameter everywhere the overload scorer is not the judge

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

## 4. Depth caps at 2, so `T***` into `T**` is invisible

`ValuePointerDepth()` documents the cap: `ElemPointer` is one bit. `T***` into `T*` IS refused (it
is a proven `T**` as far as the model can see, and the diagnostic spells it `T**`, which is
understated rather than wrong-signed); `T***` into `T**` is indistinguishable from a correct call.

```cflat
int byPP(Circle** c) { return 2000 + (*c)->r; }
extern int main() { Circle* a = new Circle(); Circle** pp = &a; Circle*** ppp = &pp;
                    return byPP(ppp); }                                     // pd_r07, garbage rc
```

`TypeAndValue::PointerDepth` now exists but the model still CAPS at 2, so a `Circle*** ppp`
declarator records `0` (NOT RECORDED) rather than a clamped 2 - a clamped value stepped down by a
`*` would falsely prove depth 1 and reject the correct `byPP(*ppp)` (frozen as the value leg
`pd_deref_of_triple_ptr_into_ptrptr_param`). So this hole is unchanged: over the cap the model
claims nothing. Lifting the cap means lifting it in `Pointer`/`ElemPointer` and `GetType` too, and
needs its own accept set - see section 6.

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

Consequence: a genuine `T**`-into-`T*` mistake is INVISIBLE anywhere a generic type argument is the
parameter's type. Closing it means carrying a real depth int through substitution, which changes
the monomorphization key (`Box<C*>` and `Box<C**>` would stop being the same instantiation) - a
much larger change than this issue, and the reason the one-sided decline was taken instead.

## 6. Depth 3 truncates SILENTLY in the plain spelling, so BOTH remedies can be wrong

Measured on the post-fix binary: `C*** x = nullptr;` compiles (rc 0) - the plain declarator branch
clamps to `**` with no diagnostic - while the same depth reached through an alias hard-errors:

```
using CP3 = C**;  CP3* x;
  -> pointer alias resolving to 'C' produces pointer depth 3, but the type model caps at 2
     levels ('*'/'**'); use fewer indirections
```

Because of that asymmetry BOTH of the overload dump's remedies are wrong advice for a `T***`
argument, and each was measured on the post-fix binary rather than reasoned about. "Declare the
parameter as `T**`" (`byPP(ppp)`) compiles and exits 240. "Dereference it with `*` at the call
site" (`byPtr(*ppp)`) also compiles - the deref clears `ElemPointer` over an unrecorded depth (the
`T***` declarator records none), so the still-`T**` value scores as a clean `T*` and NO error is
re-raised. (Both re-measured after `PointerDepth` landed; both unchanged in kind. Both exit codes
are the environment-dependent garbage of section 7 and are deliberately not quoted.) The correct
answer is 2003 (rc 211) in both cases. The message cannot currently tell the two apart, because nothing in the type model
can. Making the plain branch reject
3+ stars the way the alias branch already does would fix both this and the `T***`-into-`T**` hole
in section 4, and needs its own accept set (`C***` locals compile today).

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
