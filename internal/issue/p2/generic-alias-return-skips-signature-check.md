# Generic `alias T` return of a closure skips the return-type check (run-time crash)

Found by the T279 review (round 3, 2026-10-15). Pre-existing on master.

## Repro
```cflat
alias T pass<T>(alias T src) { return src; }
// main: int k = 4; Lambda<int(int)> n = [k](int x) { return x + k; };
//       Lambda<int(int)> w = pass<Lambda<int(int)>>(n); return w(7);
```
Compiles rc 0, crashes at run time (139 as a binary, 138 under --run), master and after T279. Same for r22 (an explicit
`alias Lambda<int(int)>` parameter through the generic). Probes: scratch/repro_keep/wt_t279/ (r20, r22).

## Root cause (review)
The non-generic form `alias Lambda<int(int)> pass(alias Lambda<int(int)> src) { return src; }` is refused with
"cannot return this value: its type does not match the return type of function 'pass'"; the monomorphized generic
alias-T return skips that check. A generic alias-T return of a thin global (non-generic twin
`alias Lambda<int(int)> get() { return g_fn; }` is refused) is accepted and returns the right value by luck.

## Fix direction
Run the same return-type / alias-return check on the instantiated generic body as on the non-generic twin; the
generic forms must be refused with the same text. Refusal legs in Test/errors/err_funcptr_value_slot_signature_mismatch.cb.

## T285 attempt (2026-10-15, Haiku) - PARKED, needs a maintainer ruling
Root cause: the non-generic `alias Lambda<int(int)>` return carries `returnFnPtrTV.IsFunctionPointer`, so
`CoerceToFuncPtrReturn` (MainListener_Statements.cpp ~2277) loads the fat closure and the ABI backstop
(LLVMBackend_MoveDataflow.cpp ~2161) refuses it. In the generic instance the return TV has no IsFunctionPointer; T279's
`encodedAliasReturnSlot` branch gates the coercion off (`&& !encodedAliasReturnSlot`), so a raw pointer passes.
Candidate (scratch/repro_keep/wt_t285/t285_tmp/candidate_gate_removal.patch): drop `&& !encodedAliasReturnSlot` ->
r20/r22/thin global refused ("does not match the declared return type"), generic int/pointer/struct returns unchanged.
CONFLICT: core `list<Lambda<int(int)>>::get` (core/list.cb:146, `alias T get`) takes the same path; master accepts
`Lambda<int(int)> w = xs.get(0);` (value 14, used by Test/test_function_ptr.cb), the candidate refuses it. Its
non-generic twin is ALSO refused on master, so "exactly like the twin" breaks list<Lambda>::get.
Ruling options:
(A) refuse every generic alias closure return (breaks list<Lambda>::get - not acceptable without a ruling).
(B) refuse only when the returned place is frame-rooted (an alias Lambda param that the ABI passes by value, a callee
    local) via the existing aliasSourcePointsIntoFrame walk, consistent with the &ref() frame-rooted gate ruling
    (2026-09-24); element/member places (list.get) and globals stay accepted - and then the NON-generic twins of
    list.get / the global return should be accepted too (they are refused on master today).
Main-session recommendation: (B). First confirm r20's crash is the by-value alias param (frame-rooted) and not an ABI
mismatch on a valid place.
