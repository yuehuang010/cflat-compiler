# Split the owning-temp ledgers: `ownedReturnTemps_` fails UNSAFE, `ownedNewTemps_` fails SAFE

Filed 2026-07-25. Architectural follow-up, deliberately deferred. Not a live bug: every reader is
correct as of the commit "Free a non-escaping owning-pointer temp passed as a call argument". This
records WHY three consecutive review rounds found the same shape of defect, so the next change in
this area starts from the right structure.

## Status

KNOWN / OPEN by choice. No user-visible symptom today.

## The asymmetry

Two value-keyed ledgers in `cflat/LLVMBackend.h` answer overlapping questions:

- `ownedNewTemps_` (`Value` -> `TypeName` / `AllocAlign`). Retiring an entry means "nobody may
  free this". Erasure FAILS SAFE: a reader that never learns about an entry emits no free, and the
  worst outcome is a leak.
- `ownedReturnTemps_` (`Value` -> `FnName` / `TypeName` / `AllocAlign` / `IsOwningPtr` /
  `CallerReleaseSuppressed`). It serves TWO purposes at once - the mandatory-nodiscard DIAGNOSTIC,
  which must see a value even when nothing may free it, and RELEASE eligibility. Because the
  diagnostic needs the entry to survive, "not freeable" had to be expressed as a QUALIFIER
  (`CallerReleaseSuppressed`) rather than as erasure. Presence therefore FAILS UNSAFE: a reader
  that consults presence without the qualifier concludes "owning" and emits a free.

Every defect found in review rounds 1-3 is an instance of that: a reader (the '?:' arm predicate,
the call-argument registration, the nested-join predicate) treated ledger PRESENCE as ownership.

## Mitigation already in place (not a fix)

`FindOwnedReturnEntry` is now the flag-CHECKED accessor - a suppressed entry reads as absent - and
the raw lookup was renamed `FindOwnedReturnEntryForDiagnostic`, used only by the no-discard path
and by suppression-preserving propagation. Forgetting the qualifier now costs a leak instead of a
double free. That makes the default safe but leaves the two concerns fused in one record.

## End state

Two genuinely separate ledgers:

- a DETECTION ledger, `Value` -> `FnName`, consumed solely by `DiagnoseDiscardedOwningReturn`;
- a RELEASE ledger, `Value` -> `TypeName` / `AllocAlign`, ideally UNIFIED with `ownedNewTemps_`
  since a raw `new` and an owning return are already treated identically at every free site.

Suppression then becomes plain erasure from the release ledger, `CallerReleaseSuppressed`
disappears, and there is no qualifier left to forget.

## Cost

Touches roughly ten functions in `cflat/LLVMBackend.h` - `RegisterOwnedReturnTemp`,
`PropagateOwnedReturnTemp`, `FindOwnedReturnEntry`, `FindOwnedReturnEntryForDiagnostic`,
`FindOwnedReturnTemp`, `RegisterOwnedPtrTemp`, `IsOwningPtrTempValue`, `SuppressCallerRelease`,
`TernaryArmJoinsOwning`, `PropagateTernaryOwnership` - plus the `BuilderState` snapshot/restore
struct (the `ownedReturnTemps` / `ownedNewTemps` members and their save/restore) and the `_ =`
explicit-discard site in `cflat/MainListener.h`.

`ownedReturnTemps_` is transient per full expression and is NOT part of the `--init` cache
round-trip, so the serializer rule does not apply to this rework.

## Why deferred

Three review rounds in, the priority was a provably-closed, minimally diffed fix for the
call-argument leak. The accessor rename captures most of the safety benefit at near-zero risk; the
split is a mechanical but wide refactor that should land on its own, with the full leak-test bar
run before and after.
