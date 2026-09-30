# P3: Remaining C++ free-operator and converting-constructor retry gaps

This file now tracks only the D3 review findings not fixed in D6. The original pointer-return
method and `operator->` retry gap is fixed in D6 by requesting an opaque pointer's unrequested
class-template pointee when that member is projected on use. Pointer-returning member `operator*=`
expressions now preserve their declared pointer result type. See `scratch/d6_matrix.md` and
`scratch/briefs/d6_report.md` for measured probes and legs.

## Remaining findings

- A global-namespace class with a free operator returning a class-template specialization is still
  refused for both `*=` and binary `*`. D6 probes `global_compound.cb` and `c_GB.cb` still fail on
  the post-fix compiler with the same errors as master. The issue is in global free-operator lookup
  and binding, outside the member-signature lazy retry path.
- A free operator declared on a base class was reported refused for a derived receiver, including
  a plain-return control on master. The copied D3 fixture's analogous specialization-return and
  plain-return compound operators (`base_free_compound.cb`, `c_KD2.cb`) both pass on master and
  post-fix; the exact failing D3 spelling still needs a focused repro before changing overload
  candidate selection.
- A free or member compound-assignment expression such as `(a *= x).get()` can use the wrong
  declared result type for reference returns. D6 fixed pointer-return member `operator*=` result
  propagation (leg 7562), but `ok_Aexpr.cb` and `ns_free_compound.cb` still fail for reference
  result types. This expression-result path needs separate review.
- `op3.W d = a * 3.0` still reports the incorrect `cannot cast an aggregate value` diagnostic while
  explicit `op3.W(a * 3.0)` succeeds. D6 measured the pair in `op3_convert.cb` and `op3_direct.cb`;
  aggregate converting-constructor resolution is a separate path.
- A C++ class with only a binary `@` operator, or a deleted/private `@=` beside a public binary
  `@`, can silently fall back to `a = a @ b` although `clang++ -std=c++20` rejects it. This is a
  compound-fallback design question and remains unchanged pending a maintainer ruling.
