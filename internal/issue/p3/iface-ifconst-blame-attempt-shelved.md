# SHELVED ATTEMPT: naming the guarding `if const` in the zero-implementor rebox error

Companion to `iface-ifconst-base-clause-implementor.md`, which states the underlying
problem and is still open. This file records a serious implementation attempt that was
SHELVED on 2026-07-27 after eight review rounds surfaced nine distinct defects.

**Read this before starting a tenth round or a fresh implementation.** The branch is not
wrong-headed; the design invariant survived a hostile independent audit. It was shelved
because the defect rate did not converge, and the feature is diagnostic quality only -
master's plain `no class implements it` is unhelpful but NEVER wrong, which is the safe
side to be on.

## Where the work is

Branch `fix/iface-ifconst` @ `23418c2`, a SINGLE commit on top of master `853cb87`, in the
linked worktree `../cflat-fix-iface-ifconst`. Left in place deliberately. Suite was green
at the time of shelving: **514 passed / 0 failed / 8 skipped** (macOS Release), verified by
the main session, not just self-reported. Its `scratch/` holds the entire repro corpus
(~60 files: `repro_*.cb`, `p_*.cb`, `q*.cb`, `rv/*.cb`) - that corpus is the single most
valuable artifact here and is worth more than the diff.

Note the branch DELETES `internal/issue/p2/iface-ifconst-base-clause-implementor.md`. If it is
ever revived, that deletion is premature until the feature actually ships.

## The design, and what survived

At scan time every class inside an `if const` subtree is recorded with a CHAIN of the levels
it sits under, outermost first. As MainListener decides each arm it retracts (arm taken,
class is live) or peels ONE level (a nested `if const` gets its own decision). Invariant:

> **the front of a surviving chain is an arm nobody was shown to take**, so naming it is truthful.

SURVIVED a heavy round-4 assault and a hostile independent review at round 8. The design is
sound. Every one of the nine defects was an unenumerated syntactic SHAPE, or a speculative
value leaking into a set that must hold only facts - never the invariant itself.

Also independently established (do NOT re-litigate):
- An `if const` node's only rule-children are the expression, the arm blocks, and the
  chained-else if-const. A nested `if const` is always separated by an intermediate context.
- An interface body cannot hold a `classDefinition` (`interfaceMember` is closed over
  method / field / if-const), which is why `AppendIfConstInterfaceMembers` needs no peel.
- `ifConstGuardedImpls_` is diagnostic-only and never serialized, so the CLAUDE.md `--init`
  serializer rule genuinely does not bite. Warm-cache diagnostics are byte-identical.
- The `interfaceTable` filter at BLAME time is safe despite the ForwardRefScanner ordering
  hazard, because it runs at finalization after every interface is registered. Verified with
  an interface registered only inside a later, taken `if const` arm.

## The nine defects, in the order they were found

1-5. Assorted `else` / chain / nesting shapes. Resolved by making a chained `else if const`
   a real chain level, composed as `the else path of an 'if const (X)'`, deliberately
   distinct from the brace-else `the else arm of`.
6. **Generic class/struct TEMPLATE bodies.** Members are reconciled zero times (template
   never instantiated) or N times (N instantiations), and `PeelIfConstGuardedInterfaceImpl`
   is NOT idempotent, so neither count preserves the invariant. Fixed by suppressing BLAME
   (never uncertainty) for classes under a template.
7. **A file-scope `expect_error` block that FIRES.** `LogError` throws, the catch swallows
   it, the rest of the subtree is never walked, so its arms are never decided and stale
   entries survive with a chain front the walk never reached. Fixed with
   `ForgetIfConstGuardedImpls` - an unconditional erase, NOT a peel.
8. **Over-broad candidates reaching the blame list.** The full
   `AppendInterfaceNameCandidates` set (enclosing-namespace guesses plus a last-component
   fallback) fed BOTH the suppression set and the blame set. Over-broad is SAFE for
   suppression (it can only weaken an impossibility proof) and FABRICATES CLAIMS for blame.
   Introduced by the round-6 conflict resolution.
9. **The residue of 8, and why it BLOCKED the merge.** The round-7 fix
   (`ResolveGuardedBaseCandidate` picks the first candidate registered in `interfaceTable`)
   removed the namespace guesses but left the last-component fallback in the same vector.
   When the qualified spelling is unregistered and an unrelated interface shares its last
   component, the fallback becomes "the first registered candidate".

   ```cflat
   interface IHandleRoot { int root(); };
   interface INative : IHandleRoot { int h(); };
   interface IHandleChild : INative { int hc(); };
   if const (__WINDOWS__ && __MACOS__) {
       namespace Win32 {
           interface INative { int nativeHandle(); };
           class WinFile : Win32.INative { int v = 0; WinFile() {} int nativeHandle() { return v; } };
       }
   }
   IHandleRoot widen(IHandleChild c) { IHandleRoot e = c; return e; }
   ```
   branch: `... 'IHandleRoot' - the only class implementing it, 'Win32.WinFile', is declared
   inside an 'if const (__WINDOWS__ && __MACOS__)' branch that is not taken in this build`
   master (correct): `... 'IHandleRoot' - no class implements it`

   `Win32.WinFile` implements `Win32.INative` only. The message even contradicts itself: it
   prints `Win32.WinFile`, so `scope` tracked the inner namespace, while the candidate list
   was built from the outer `namespaceName`.

   Cause: `cflat/MainListener.h:605-606` (fallback pushed into `candidates`) plus
   `cflat/MainListener.h:609` (the same vector serves suppression AND blame).
   **Known fix**: keep the fallback for `RecordUncertainInterfaceImpl` only; build the blame
   list from the `AppendInterfaceNameCandidates` prefix. That makes it a MISSED blame, the
   safe direction, and leaves `err_iface_rebox_ifconst_unrelated_iface.cb` green. Ideally
   also pass `scope` rather than `namespaceName` into `AppendInterfaceNameCandidates`, so a
   namespace opened INSIDE the arm gets real candidates.

## Why blame escapes suppression (the non-obvious reachability argument)

The blame set is a subset of the uncertain set, so a false blame on X ought to imply X is
uncertain and therefore no error fires. It is reachable anyway: `FindIfConstGuardedImplementor`
matches through `InterfaceInheritsFrom`, and uncertainty is deliberately NOT propagated up
the inheritance chain, so **converting to a PARENT of a surplus candidate escapes
suppression**. Both defect 8 and defect 9 use this route. Any future fix must be checked
against it.

## Open handoff items from the round-8 independent review

1. **Double `ResolveTypeAlias`.** `ResolveGuardedBaseCandidate` alias-resolves a candidate
   that `ResolveInterfaceName` has already alias-resolved once. Not falsifiable in plain
   CFlat (a name cannot be both a registered interface and an alias key in one file), but
   UNTESTED against `import package "*.h"` and WinMD projection, where a second alias hop is
   most plausible.
2. **`scratch/q5_nsdeep_shadow.cb` needs a PRODUCT DECISION.** The blamed class resolves
   against the global interface and is blamed for its parent; with the arm taken the compiler
   errors `class 'deep.QZG' does not implement 'IQz::gz'`. So the message names a class that,
   had the arm been live, would not have compiled. The resolver agrees with the diagnostic,
   so it is technically truthful, but "the only class implementing it" is doing work the base
   clause does not support.
3. **Truncation quality.** The 120-byte cut lands mid-identifier and reads as corruption.
   Untested: a condition containing an unbalanced quote or `*/`, echoed verbatim.
4. **`ForgetIfConstGuardedImpls` is called from only ONE of the five
   `ExpectedErrorReceived` catch sites** (`cflat/MainListener.h:4325`). The other four are
   argued unreachable because the grammar puts no `classDefinition` in a function body. That
   is a "dead code needs no handling" justification - a deferred bug the moment the grammar
   loosens. See defect 7 for what that costs.
5. **LSP re-analysis and batch mode** were verified by READING only. `ResetForReanalysis`
   clears the registry; repeated `--check` in one process and multi-file batch were not run.

## Process notes specific to this attempt

- Rounds 6 and 7 each INTRODUCED the next defect while fixing the previous one. Self-review
  missed both; an independent reviewer or the main session found both. **Never let the agent
  that wrote a fix be the only one to hunt for its consequences.**
- When an agent cites a justification, check it still holds AFTER the change it justifies.
  Round 6's "the Mark site feeds only `uncertainInterfaceImpls`, which can only weaken a
  proof" was true when written and false after its own edit - that is exactly defect 8.
- Make reviewers prove the two binaries differ before trusting any A/B result. The round-8
  reviewer did it by grepping for a branch-only diagnostic literal.
