# A `?:` / `??` join defeats the CLOSURE-WIDEN gate, so a data pointer is widened and CALLED

Filed 2026-08-05 by `fix/joinledger` while auditing the neighbours of the code-value gates. This is
the MIRROR of the issue that branch closed, on the gate that asks the opposite question, and it is
NOT closed by that fix.

Severity: **P1 - memory-unsafe, silent.** A data pointer lands in a fat closure's CODE slot and is
then called. Exit 139, no diagnostic.

## Repros - both exit 139 on `d93c359` AND on `fix/joinledger`

Free function (`scratch/jl/n_lambda_param_datajoin.cb`):

```cflat
import "function.cb";
int applyL(Lambda<int(int)> f) { return f(5); }
extern int main(int argc, char** argv)
{
    int q = 1;
    void* vp = &q;
    void* vq = nullptr;
    printf("v=%d\n", applyL(argc > 0 ? vp : vq));   // exit 139 on both binaries
    return 0;
}
```

Method spelling (`scratch/jl/n_lambda_method_join.cb`): `d.lam(argc > 0 ? vp : vq)` on a
`class D { int lam(Lambda<int(int)> f) ... }` - same result, exit 139 on both binaries.

**The BARE spelling is diagnosed on both binaries**, which is what makes this a join defect rather
than a plain hole:

```
cannot pass a non-function pointer value to closure parameter 'f': only a named function,
a 'function<>' value or a lambda converts to a closure
```

`applyL(vp)` and `d.lam(vp)` both produce that message on `d93c359` and on `fix/joinledger`.
Wrapping the same `void*` in a `?:` makes it widen again.

The `??` spelling behaves the same and was MEASURED, not inferred: `applyL(vq ?? vp)` is exit 139 on
both binaries (`scratch/rev2/newp1/w4_nullcoal.cb`, review round 1). So both join kinds are in
scope, exactly as they were for the code-value gates.

Legal neighbour, measured identical on both binaries and exit 0 (`v=6`): a join of two
`function<int(int)>` values into the same parameter (`applyL(argc > 0 ? a : b)`). Any fix must keep
this compiling - it is the first accept-set cell.

## Root cause

`WidenToClosureFatChecked` gates on argument PROVENANCE: it widens only what it can PROVE is a
function (a named function, a `function<>` value, a lambda). That polarity is correct - "prove what
you reject, accept what you cannot prove" - and a join is exactly the shape where nothing can be
proven, so the gate accepts and the widen happens.

So this is the same erasure as `join-erases-code-value-evidence-at-every-gate`, seen from the other
side, and it needs the OPPOSITE evidence. That issue's fix ledgers values proven to be CODE
(`codeValues_` in `LLVMBackend.h`) and resolves a join by asking whether ANY arm is code. Closing
this one needs values proven to be DATA, and the join question becomes "is EVERY arm data" - a
different ledger, a different quantifier, and its own accept set. Folding it into that change would
have meant writing a second guard past a frozen accept set, so it was filed instead.

## Fix direction

Symmetric to `codeValues_`, and reusing its join walk:

- Ledger a value proven to be DATA where the facts are local - the same `LoadNamedVariable` site,
  gated on the declared type being a non-function pointer.
- Give the closure-widen gate a join-aware reader that answers "this join delivers data" only when
  EVERY arm is ledgered data (an unledgered arm proves nothing and must keep widening). Note the
  quantifier is the reverse of the code-value one, and getting it backwards is a false rejection on
  every mixed join.
- The `??` arms come from `nullCoalesceJoins_`, exactly as they do for the code-value walk.

Accept set to build FIRST (all pass today, all must keep passing): a join of two `function<>`
values, of two named functions, of two lambdas, of a `function<>` and a lambda, and a join with one
unledgered arm (a call result, a cast) - the last is the cell a wrong quantifier breaks.
