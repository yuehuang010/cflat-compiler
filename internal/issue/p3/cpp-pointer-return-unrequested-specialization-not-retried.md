# C++ method / operator-> returning a POINTER to an unrequested specialization is never retried

Found 2026-09-28 by run D3. Reference and by-value returns of an unrequested class-template
specialization bind on use through the refused-member retry (d8d21f18); pointer returns do not:
`R2<C2>* ptr()` -> "no overload of ptr matches", and `R1<C1>* operator->()` -> `a.get()` "function a is not
known". Repro: scratch/repro_keep/d3/opa.h + a1.cb / a2.cb (main checkout).
Fix direction: the refusal reason for a pointer-to-specialization return must be recorded as retryable like
the by-value case (find where the retry decides eligibility), plus legs for a method and for operator->.

More gaps in the same family, from the D3 review (2026-09-28, probes in cflat-fix-d3 scratch/rv2/, copied to scratch/repro_keep/d3rv2/). All are pre-existing and outside D3's matrix:
- A global-namespace class whose free operator returns a specialization is refused, for both `*=` and binary `*`.
- A free operator on a base class is refused for a derived receiver. The plain-return control is refused on master too.
- `(a *= x).get()` fails because the compound expression ignores the operator's declared return type.
- A pointer-returning `operator*=` is never retried; this is the original item above.
- `op3.W d = a * 3.0;`, which converts through a template converting ctor, fails with the wrong diagnostic "cannot cast an aggregate value - a fixed array decays...". `W(a * 3.0)` works.
- A class with only a binary `@` (or a deleted or private `@=` beside a public binary `@`) silently falls back to `a = a @ b`, while clang refuses. That is a CFlat compound-fallback design question for C++ classes: needs a ruling before it is changed.
