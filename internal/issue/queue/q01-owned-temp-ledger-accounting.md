# q01: Owned-temp ledger accounting

9 items. The compiler tracks "a temporary that someone must free" in value-keyed ledgers. The
ledgers conflate two different questions and are not trimmed on every exit path, so readers both
leak and double-free depending on which side they land on.

## Shared root cause

`ownedReturnTemps_` answers "is there a nodiscard diagnostic to emit?" AND "may this be released?"
with one entry, so PRESENCE fails unsafe. `ownedNewTemps_` fails safe. Readers written against one
mental model are wrong against the other. Separately, several exit paths (aborted ternary arm,
lambda body end, non-dominating null-conditional block) never run the trimming/flush step at all.

## Members

- `p2/owning-temp-ledgers-should-be-split` - the design anchor. FIX FIRST; the rest are cheaper
  once presence means one thing.
- `p2/detection-ledgers-not-discarded-on-aborted-arm` - `DiscardOwnedTempsSince` predates
  detection-only ledgers and never trims them.
- `p2/owning-temp-in-null-conditional-arm-never-destructed` - `?.` arg block does not dominate the
  resume block, so `FlushOwnedTemps` skips it.
- `p2/discarded-copy-result-in-a-call-chain-leaks` - receiver registration excludes `string`.
- `p2/deferred-end-of-body-checks-skip-lambda-bodies` - lambda bodies bypass the named-function
  completion hook, so two pending checks are dropped unanalyzed at module end.
- `p3/owning-temp-parent-misroutes-chained-alias-access` - `OwningTempParent` does not propagate
  alias-ness across a second member-access hop.
- `p3/raw-delete-guard-does-not-retire-a-rebound-borrow` - guard's borrow test has no retirement
  check, unlike its move-spelling sibling.
- `p3/raw-heap-string-array-owned-temp-into-a-non-zero-element-leaks` - store shape is
  indistinguishable from a container-internal move store.
- `p3/nodiscard-residual-gaps` - indirect (closure/funcptr) owning-return calls are never ledgered.

## Fix direction

1. Split the ledger per `p2/owning-temp-ledgers-should-be-split`: a diagnostic-presence set and a
   release-eligibility set, so erasure always means "nobody frees this".
2. Audit every reader for presence-implies-ownership; the review history says this shape recurs.
3. Make trimming/flushing a single funnel that every scope exit goes through (aborted arm, lambda
   body, non-dominating block), instead of per-site calls.

## Adjacent

q05 (assignment arms consult these ledgers), q07 (retirement), q02 (join arms read presence).
