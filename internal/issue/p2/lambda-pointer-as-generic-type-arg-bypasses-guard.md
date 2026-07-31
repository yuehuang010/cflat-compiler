# `Lambda<T>*` as a GENERIC TYPE ARGUMENT bypasses the declarator guard and hits the verifier

Filed 2026-07-31 by the round-1 review of `lambda-behind-pointer-invalid-bitcast`.
**Pre-existing**, verified on the master binary at `8c29ca7` (before the `Lambda<T>*` rejection
landed) and re-confirmed by the reviewer on the fix branch - the fix does not change it either
way, so this is a residue, not a regression.

Severity: hard compile failure with **no source diagnostic**. The only output is an LLVM
module-verifier dump naming an internal value and no `file(line,col):` prefix.
Nothing miscompiles.

## Repro

```cflat
import "function.cb";
struct Box<T> { T item = default; };
extern int main() {
    Lambda<int(int)> f = (int x) => x + 1;
    Box<Lambda<int(int)>*> b = default;
    b.item = &f;
    printf("via generic ptr=%d\n", (*b.item)(5));
    return 0;
}
```
Observed on master `8c29ca7`:
```
  %8 = load %__closure_fat_ptr, %__closure_fat_ptr %6, align 8

Error: module verification failed.
```
exit 1.

## Root cause

The `Lambda<T>*` rejection added for `lambda-behind-pointer-invalid-bitcast` lives in the
`functionPointerSpecifier` and `using`-alias branches of the MainListener copy of
`ParseDeclarationSpecifiers` (`cflat/MainListener.h` ~3772 and ~3879). That guard covers every
DECLARATOR spelling the reviewer could route through it - struct field, file-scope global,
function parameter, return type, namespace member, struct method, `new` expression, double
pointer - all correctly diagnosed with a source location.

**Generic type-argument resolution does not route through `ParseDeclarationSpecifiers`.** The
`*` inside `Box<Lambda<int(int)>*>` is resolved on the generic-substitution path, which never
consults that branch, so the fat-closure-behind-a-pointer reaches codegen and dies in the
verifier exactly as the declarator spellings used to.

## Fix direction

Find where a generic type argument's pointer depth is resolved during substitution and apply
the same check there, reusing the existing message so the two paths stay worded alike:

```
pointer '*' is not supported on closure type 'Lambda<int(int)>'; pass the closure by value
or use a fixed size 'Lambda<int(int)>[N]' instead
```

The rejection path is already the RATIFIED direction for this family (see the landed design
record for `7536bdc` in `interface-issue-queue.md`): a fat closure behind a pointer has no
working lowering, so there is no capability to preserve. `Lambda<T>[N]` and by-value both work
and are the documented alternatives.

**The THIN spelling is broken here too - verified, and it changes the fix.** Everywhere else
`function<T>*` is a single machine pointer that works correctly, but as a generic type argument
it fails identically:

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
struct Box<T> { T item = default; };
extern int main() {
    function<int(int)> g = dbl;
    Box<function<int(int)>*> b = default;
    b.item = &g;
    printf("thin generic=%d\n", (*b.item)(6));
    return 0;
}
```
also gives `Error: module verification failed.` on `8c29ca7`.

So the generic-substitution path drops pointer depth for BOTH thin and fat function types, and
the two halves want OPPOSITE outcomes:

- **fat `Lambda<T>*`** - REJECT, matching the declarator guard (no working lowering exists).
- **thin `function<T>*`** - SUPPORT, matching every other context where it already works.
  Rejecting it would remove a capability that is legal everywhere else, which is the mistake a
  previous round in this repo nearly made on this exact type family.

That asymmetry means this is NOT a one-line copy of the declarator guard, and it is why the
issue sits in P2 rather than P1: the real work is making generic substitution carry the pointer
depth and the thin/fat distinction, with the rejection falling out only for the fat case.

## Test coverage

None. Wants a leg in the existing `Test/errors/err_lambda_array_view.cb`, alongside the
declarator legs already there.

Related: [[interface-issue-queue]]
