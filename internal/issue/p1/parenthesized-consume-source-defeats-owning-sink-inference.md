# A parenthesized consume source `(p)` defeats owning-sink inference and double-frees

Filed 2026-08-09 by the review of `fix/parmbrace`. Not a regression of it: measured identical on
`3252c01` (the branch point) and on `fix/parmbrace`, and ALL FOUR consuming store spellings behave
the same, so it is one pre-existing hole in the syntactic scan, not a brace-path asymmetry.

Severity: double free (abort, rc 134).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

void fbrace(UBox p)  { UBox[2] d = { (p) }; }   // brace element    - rc 134
void fasg(UBox p)    { UBox[2] d; d[0] = (p); } // slot store       - rc 134
void fdecl(UBox p)   { UBox o = (p); }          // decl initializer - rc 134
void fmove(UBox p)   { UBox o = move (p); }     // explicit move    - rc 134

extern int main() { UBox a = umk(5); fbrace(a); return 0; }
```

Measured (`scratch/rev_b_paren_brace.cb`, `rev_b_paren_assign.cb`, `rev_i_paren_declinit.cb`,
`rev_i2_paren_move.cb`): every spelling frees once in the callee, then aborts on the caller's `a` -
rc 134 on both binaries. Dropping the parentheses makes all four rc 0.

## Root cause

`CollectConsumedStoreNames` / `CollectUnconditionalMovedNames` (`cflat/MainListener.h:1331`,
`:1267`) are purely SYNTACTIC: they record the source subtree's `getText()` and
`ApplyOwningSinkInference` intersects that set with the parameter names. `(p)` yields the text
`"(p)"`, which never equals the parameter name `p`, so the parameter is not made a sink. The
callee-side consume arms are semantic and see straight through the parentheses, so the callee
consumes while the caller is never told - the same two-halves-disagree shape as the brace-list gap
`fix/parmbrace` closed.

This is the over-approximation running in the UNSOUND direction: the scan is designed to
over-collect (a spurious sink is safe under 8a's total scope-exit drop), but a MISSED name leaves
a real consume unreported.

## Fix direction

Normalize the recorded source text before intersecting: peel redundant parentheses off the
source subtree (walk down through single-child `primaryExpression -> '(' expression ')'` nodes)
and record the innermost bare name, in both collectors, so all four spellings agree with the
unparenthesized form. Do NOT strip anything else - a non-bare source (`v.f`, `v + 1`) must keep
failing the intersection, since only a whole-value consume counts.

## Related

Same family as `internal/issue/p3/discard-position-not-threaded-through-parens-and-ternary.md`
(parenthesization defeating a syntactic position test), but that one is cosmetic and this one
aborts.
