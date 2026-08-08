# A temp's `unique` field still escapes when the callee RETURNS it or is called INDIRECTLY

Filed 2026-08-08 by `fix/temp-uniq-plain-param`, which closed the plain-`T*` parameter for every
callee whose body PROVABLY stores the pointer into memory that outlives the call. These are the
two shapes that guard's callee-side fact structurally cannot answer. Both were accepted before
that fix too, so this is residue rather than regression - P2 under the residue-not-regression
precedent ([[unique-field-to-field-interface-receiver-residues]]).

Severity: **silent use-after-free** in both sub-cases. Compiles clean, runs, exits 0.

## Sub-case 1: the escape is through the RETURN VALUE, not through a store

`ParameterProvablyRetainsArgument` proves an escape only from a STORE into memory that outlives
the call (a global, or memory reached through another parameter). A `ret` is deliberately NOT
proof: `int rd(Node* n)` and `Node* passthru(Node* n) { return n; }` are both correct when the
result is consumed inside the statement, and `rd(passthru(makeBox().t))` is a frozen accept
(`temp_uniq_accept_plain_param_passthru_read` in `Test/test_move.cb`).

A CONSTRUCTOR is the shape where that costs coverage. cflat lowers a by-value constructor to a
function that `insertvalue`s the parameter into the returned aggregate and returns it - there is
no store at all - so both spellings below still compile and dangle:

```cflat
struct Node { int v = 0; };
struct Box<T> { T t = default; };
Box<unique Node*> makeBox() { Box<unique Node*> b = default; b.t = new Node(); b.t->v = 70; return b; }
struct Slot { Node* q = nullptr; Slot(Node* z) { this.q = z; } };

extern int main()
{
    Slot s = Slot(makeBox().t);          // measured: v=99 same=1 dtors=1
    Slot* h = new Slot(makeBox().t);     // measured: v=99 same=1 dtors=1  (also by-value ctor)
    return 0;
}
```

Measured `-o` + run on the merged `fix/temp-uniq-plain-param` binary with the allocator-reuse
witness (`Node* fresh = new Node(); fresh->v = 99;` then the read gives 99 and `same=1`) and a
destructor counter (`dtors=1`). `scratch/tup_r13_ctor.cb` and `scratch/tup_r14_newctor.cb` in the
fix worktree hold both cells; every other cell in that corpus is now diagnosed.

**Why round 1 could not reach it cheaply.** The missing fact is two-sided: a CALLEE fact ("this
parameter flows into the return value") plus a CALL-SITE fact ("this call's result outlives the
statement"). `Slot(makeBox().t)` bound to a local is a dangle; `readSlotQ(Slot(makeBox().t))` is
correct code consumed inside the statement, and rejecting it would be exactly the false rejection
the whole guard is built to avoid. The result-lifetime half has no existing machinery - the
`OwnedReturnTemp` ledger answers about OWNERSHIP of the result, not about where it is bound.

## Sub-case 2: there is no callee to ask - function pointer and interface dispatch

`RecordTempUniqueFieldArgs` reads `CallInst::getCalledFunction()`, and
`OwningPtrProvablyEscapes` treats a null callee as "no proof". So an indirect call accepts:

```cflat
function<void(Node*)> f = keep;   // void keep(Node* n) { g = n; }
f(makeBox().t);                   // measured: dtors=1, dangles

ITake it = keeperC;               // class KeeperC : ITake { void take(Node* n) { this.p = n; } }
it.take(makeBox().t);             // measured: dtors=1, dangles
```

`scratch/tup_a11_fnptr.cb` and `scratch/tup_a12_iface.cb` in the fix worktree. Both are recorded
as ACCEPT cells there, deliberately: unknown-accepts is the guard's chosen polarity and these are
the honest unknowns.

The interface half is the more closable of the two - the set of implementors IS known at end of
module (`interfaceTable`), so an interface method could be judged as "every implementor of this
slot provably stores", which is the same shape `ResolveMaterializedInterfaceUses` already runs at.
A `function<T>` value has no such closed world short of a points-to analysis.

## Sub-case 3: a SELECT / PHI of two global addresses launders the store

`MemoryOutlivesCall` resolves the store destination with `llvm::getUnderlyingObject`, which does
NOT look through a `select` or a `phi` - it returns the select itself, which is neither a global
nor an argument, so the walk finds no proof and accepts:

```cflat
Node* g = nullptr;
Node* g2 = nullptr;
void keepsel(Node* n, int c) { Node** p = c > 0 ? &g : &g2; *p = n; }   // ACCEPTED, dangles
void keepsel1(Node* n)       { Node** p = &g; *p = n; }                 // rejected correctly
```

Measured `-o` + run: `keepsel(makeBox().t, 1)` gives `v=99 same=1 dtors=1` on BOTH `0047297` and
the merged `fix/temp-uniq-plain-param`, while the single-global indirection one line below it is
diagnosed - so this is specifically the join, not indirection. `scratch/rvw_f8_selectglobal.cb`
in the fix worktree (reviewer probe). The fix is small and local - answer a `SelectInst` /
`PHINode` by recursing on its operands with an ANY-arm rule, which is the same polarity
`JoinCarriesOwningTempUniqueField` already uses one file over - but it widens a REJECTION, so it
needs its own accept set (a join of a global address with a LOCAL address must not be proof).

## Why these are one file

They are two mechanisms, not one root, and are filed together only because they are the exact
complement of one guard: what `ParameterProvablyRetainsArgument` answers is "a KNOWN callee that
STORES", and these are "a known callee that does not store, it returns" and "no known callee".
Fixing either one alone is a self-contained change. Do not consolidate them further.

## Also known, and deliberately accepted

`OwningPtrProvablyEscapes` follows a load back out of a stack slot only while every store into
that slot is a value the walk already tracks, so `Node* p = n; if (c) { p = other; } g = p;`
proves nothing and accepts. That is the safe direction and is not a filed bug.

Related: [[interface-issue-queue]]
