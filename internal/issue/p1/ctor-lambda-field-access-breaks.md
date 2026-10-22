# A lambda inside a constructor cannot reach the struct's fields

Summary: inside a native constructor body, a lambda that writes a field fails:
- `this.id = i;` in the lambda -> "Module verification failed: Referring to an instruction in another function!"
  (the capture reuses the ctor's `this` value directly instead of capturing it). Release exe; would assert on Debug.
- bare `id = i;` in the lambda -> "Undefined variable id." (implicit-this field lookup is not done in a ctor lambda).
The same lambda inside an ordinary method (`void Set(int i)`) compiles and runs. Pre-existing: reproduces on
pre-const-phase-0 builds (1903f02fd). Found during const phase 2.

Repro: scratch/repro_keep/ctor_lambda/this_field.cb, bare_field.cb (`struct S { int id = 0; S(int i) {
Lambda<int()> f = () => { this.id = i; return 0; }; f(); } }; int main() { S s = S(4); return s.id; }`, expect 4).

Fix direction: route ctor-body lambdas through the same `this` capture and implicit-field lookup the method path uses;
the verifier failure needs a real capture, not a guard. After the fix, re-check the const phase 2 ConstructingScope
exemption: a const field written from a ctor lambda must still be refused (#2, err_const_field_write.cb).

## PARKED 2026-10-20 (T345, 9 rounds) - branch fix/t345-ctorlambda (worktree cflat-fix-t345-ctorlambda), keep/t345-r5
r1 (this-capture in ctor lambdas) works and r6 builds default locals / field inits / array elements in place, BUT accepting ctor
lambdas newly exposes the self-referential-copy problem: an object whose stored closure captures `this` dangles when copied (by-value
arg, assignment, by-value return). Rounds tried:
- r2b-r5: change the native by-value ABI (indirect params / sret) - ABANDONED, 4 P1 ownership regressions (Lambda move, thin
  function<>, field adopt double owner, borrowed-param return).
- r7-r9: refuse copies of a fresh S() whose ctor stores a this-capturing closure; the "stores a this-capturing closure" property is a
  SYNTACTIC pre-pass scan (AggregateCtorStoresThisClosure) - every review found more name-resolution edge cases (sibling-scope lambda
  locals hiding a capture, foreach bindings, nested lambda params). Do NOT retry syntactic resolution.
Direction for the next attempt: decide the property from REAL name resolution (the r1 capture code knows exactly when `this` is
captured) but order-independently - e.g. emit every native ctor body (or at least its lambdas' capture analysis) before any other
function body of the module, or defer the copy checks to end of module and report there. By-value returns of such objects stay the
separate p2 ctor-lambda-returned-value-dangles (filed on the branch; copy it to master with the fix). Reviews: the branch's
scratch/briefs/t345_review1..7.md; final open findings in t345_review7.md.
