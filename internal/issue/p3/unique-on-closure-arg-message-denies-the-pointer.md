# `unique` on a closure generic type argument: correct rejection, factually false message

Filed 2026-08-05 by the round-1 review of `fix/lamptr-generic`. **Diagnostic quality only** - the
REJECTION is correct and must stay. A closure or function pointer does not own an allocation, so
`unique` on one is meaningless; the declarator path already says exactly that
(`Test/errors/err_lambda_array_view.cb`: "'unique' on field 'FH.p': a function pointer or closure
does not own an allocation"). The generic-type-argument path rejects the same shape with a
different message that is untrue of the program.

## Repro

```cflat
import "function.cb";
using TA = function<int(int)>;
struct RBox<T> { T item = default; };
extern int main() {
    RBox<unique TA*> b = default;
    printf("x6\n"); return 0; }
```

On the `fix/lamptr-generic` binary:
```
(5,9): unique requires a pointer or interface type
```

`TA*` IS a pointer. The message denies the one thing the source plainly states, so it sends the
reader looking for a missing `*` that is already there.

## Both sites, and why this is exposure rather than a regression

The wording lives at two sites in `MainListener::ResolveTypeArgEntry`, one per closure arm:

- the `functionPointerSpecifier` arm - the DIRECT spelling, `RBox<unique function<int(int)>*>`.
  Measured IDENTICAL on master `4c06cce` and on `fix/lamptr-generic`: both print
  `unique requires a pointer or interface type`. Pre-existing, untouched.
- the `functionTypeAliases` arm - the ALIAS spelling above. On `4c06cce` this arm was gated
  `!hasPointer`, so a pointer fell past it and the file failed earlier with `unknown type 'TA'`;
  `fix/lamptr-generic` re-gated it to `!hasArrayView` so the alias now reaches the same check.

So the alias spelling's message is newly REACHABLE, but the wording itself is unchanged and was
already wrong at the direct site. `fix/lamptr-generic` deliberately did not touch the `unique`
check - changing a rejection's wording is a separate concern from carrying pointer depth, and the
rule in this repo is that a guard is not edited in a commit that is not about it.

Note the check is unconditional inside both arms: it fires whether or not a pointer was written,
which is why the message reads as if no pointer could have been present.

## Fix direction

Say what is actually wrong, and reuse the declarator path's wording so the two agree:

```
'unique' on a generic type argument: a function pointer or closure does not own an allocation
```

Do it at BOTH arms in the same change - they are copies of one predicate, and the alias arm is the
one a reader is most likely to hit now. Keep the rejection; only the text changes. A leg belongs
in the existing `Test/errors/err_lambda_array_view.cb`, which already carries the declarator-side
`unique`-on-a-closure-field leg.

Related: [[interface-issue-queue]]
