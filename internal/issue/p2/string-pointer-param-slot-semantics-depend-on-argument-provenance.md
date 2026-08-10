# A `string*` PARAMETER's slot semantics depend on the argument's provenance, which the callee cannot see

Filed 2026-08-10 by `fix/rawheap` (review round 3), as the residual of that branch's repair.

Severity: silent use-after-free for one of the two callers. No crash, no diagnostic.

## The ambiguity

`void fill(string* h, string s) { h[0] = s; }` has exactly one body and two incompatible correct
meanings, chosen by the CALLER:

```cflat
string* h = new string[2];  fill(h, a);   // slot must BORROW - the allocation never frees
string[4] arr = default;    fill(arr, a); // slot must deep-COPY - the array's slots are LIVE owners
```

`fix/rawheap` separates those two at the STORE site by provenance on the base local
(`NamedVariable::DecayedFromFixedArray`, set when a raw `T*` local is bound by decay from a fixed
array and propagated across `string* q = p;`). A PARAMETER carries no such provenance: the decay
happens in the caller's frame, and the callee sees only `string*`.

Round 4 INVERTED the gate to positive provenance, so a parameter - which cannot carry its
argument's provenance - is never tagged and keeps the merge base's meaning at BOTH call sites: a
deep-copying store. Both of the parameter's misbehaviours are therefore MASTER's, unchanged by
`fix/rawheap`, and this file is the single record of them:

1. **Heap argument, store side: the deep copy is orphaned.** `fill(h, a)` with
   `string* h = new string[2]` copies into the slot, and nothing frees an element past element 0
   (`delete[_]` frees only the buffer, destructive `delete[n]` is rejected). Measured as +16
   leaked bytes when a `rhstr_param_*` leg calls `delete[_]` instead of letting the local's own
   teardown destruct element 0. The filed rc-133 double free through a param is nevertheless GONE,
   because the CALLER's read of its tagged local now borrows (`scratch/rh_22_param`: rc 133 on
   `b220d54`, rc 0 on the branch) - only the store half is still the caller-dependent one.
2. **Fixed-array argument: master is correct and the branch keeps it correct.** Recorded below
   because it is the other half of the same ambiguity, and because the round-3 negative-provenance
   design got it WRONG (`v=0`); the inversion restored it.

The fixed-array caller under the round-3 negative gate:

```cflat
void fill(string* h, string s) { h[0] = s; }
extern int main()                       // scratch/rh_62_param, reviewer's rv_10_param
{
    string[4] arr = default;
    { string a = "ab" + "cd"; fill(arr, a); }
    printf("v=%d\n", arr[0] == "abcd" ? 1 : 0);
    return 0;
}
```

Measured: `v=1` on `b220d54`, `v=0` on the round-3 commit `f4f358c`, `v=1` again after the round-4
inversion (`scratch/rh_62_param`). The DIRECT spellings are all correct on the branch: `p[0] = a`
through a decayed local, a `?:` join base and a base re-assigned away from its allocation are
`v=1` (`scratch/rh_60`, `rh_70`, `rh_73`), and `h[0] = a` on a `new string[n]` local no longer
double-frees.

## Second cell, pre-existing and unrelated to the choice above

Reading a fixed-array `string` element through a decayed pointer loses the deep copy that the direct
`arr[0]` spelling gets: `string[4] arr; arr[0] = "ab" + "cd"; string* p = arr; string q = p[0];` is
`indep=0` and rc 134 under `--run` on `b220d54` AND on `fix/rawheap` (`scratch/rh_63_decayread`).
`IsOwningArrayStringElementRead` admits a TWO-index fixed-array GEP; the decayed spelling is a
single-index GEP off a pointer, so it never reaches that arm. Same provenance gap, read side.

## Fix direction

Either propagate the provenance across the CALL (a `string*` parameter inherits the argument's
`DecayedFromFixedArray`, which needs it in the signature or a per-callsite specialisation), or make
the two meanings spellable - a `T[]` view parameter already means "the caller's LIVE storage" and
is the existing answer for the fixed-array caller, so the narrower fix may be to steer a raw
`string*` parameter to `string[]` and leave the raw form with the heap meaning. Do NOT resolve it by
making the base-local gate NEGATIVE (admit unless known to be a fixed array): that was tried in
round 3 and leaks a use-after-free at every binding nobody enumerated - a join, a re-assignment -
which is why the gate is positive.
