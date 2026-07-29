# Interface issue queue

The tracker for the interface-related entries in `internal/issue/`. Two files already
linked `[[interface-issue-queue]]` before this existed; this is that file.

Not a separate issue - an index. Each row points at the file that owns the detail.
When an issue is fixed its file is deleted (the repo convention), so delete its row
here in the same change.

Last updated 2026-07-28.

## Closed in the 2026-07-28 session

- **`as` cast of a stack value to an interface crashed the compiler** - fixed. Root cause
  was `elemType` propagation: `ParseMultiplicativeExpression` populates
  `TypedValue::elemType` only for pointer sources, so a stack class value reached
  `GenerateSafeCast` with a null `elemType`, fell through to the interface-source path,
  and `CreateExtractValue(value, {1u})` ran on a class aggregate. Stack values now join
  the statically-resolved concrete branch. Three review rounds; the fallout is the four
  `as-*` files below, which are all PRE-EXISTING gaps the fix surfaced rather than caused.
- **Named arguments ignored on the interface call path** - fixed. The interface arm never
  called `namedArgument->Identifier()`, so `VariableName` was never set and `MatchFunction`
  saw no named arguments. Fixing it made call-site index and declared-parameter index
  diverge on that path for the first time, which exposed three downstream sites that had
  silently relied on them being equal (duplicate-name crash, lambda expected-type seeding,
  positional brace-init resolution). Two further pre-existing bugs surfaced while auditing
  the fields the interface arm failed to copy: a FALSE REJECTION of legal `alignas` code on
  a `move` parameter, and a SILENT MISCOMPILE where `u8 200` through an interface widened to
  `-56`. Both fixed. Residue is [[named-arg-replay-reports-losing-candidate]],
  [[iface-slot-replay-blames-wrong-slot]], [[iface-thin-function-param-no-lowering]], and
  [[iface-arg-lambda-fnptr-type-not-propagated]].

## Open - crashes and silent miscompiles

| Issue | Severity |
|---|---|
| [[as-cast-pointer-ternary-operand-compiler-crash]] | SIGSEGV, zero output, `--check` passes clean. Ordinary source. |
| [[duplicate-constructor-signature-hangs-compiler]] | Hang/OOM (exit 137), no diagnostic. Namespaced classes newly route onto this path. |
| [[generic-interface-registered-as-opaque-struct]] | LLVM verifier failure + false rejections. `IFace<T>` unusable in most positions. |
| [[as-cast-array-shaped-source-no-diagnostic]] | Compiles clean, exe segfaults. Sibling of the pointer-`?:` crash. |
| [[as-boxing-skips-ownership-transfer]] | asan double-free / use-after-free. Four manifestations, one root cause. |
| [[generic-interface-namespace-scope-limit]] | Silent miscompile. DELIBERATE scope limit of `c9acb6c`, recorded so it is not lost. |
| [[iface-thin-function-param-no-lowering]] | Module verification failure, no diagnostic. Any `function<>` interface parameter. |
| [[interface-return-dangle-defeated-by-intermediate-local]] | Dangling fat pointer, no diagnostic. Both spellings. |
| [[as-boxing-skips-pointer-shape-rejection]] | Silently boxes element 0 of an array view. |

## Open - false rejections and accept-set problems

| Issue | Severity |
|---|---|
| [[bare-interface-name-resolves-outward-before-namespace]] | Makes the documented namespace workaround awkward. |
| [[iface-ifconst-base-clause-implementor]] | Implementor inside a non-taken `if const` -> "no class implements it". |
| [[unique-array-view-accepted-as-generic-type-argument]] | Inconsistent accept set, no miscompile shown. |

## Open - diagnostic quality

| Issue | Severity |
|---|---|
| [[named-arg-replay-reports-losing-candidate]] | Reports a losing candidate's name miss instead of the real failure. |
| [[iface-slot-replay-blames-wrong-slot]] | Message names a parameter that IS declared. Same root shape as the row above; fix together. |
| [[interface-collision-message-prefix-still-basename]] | The `file(line,col):` prefix is still a bare basename. |
| [[paren-as-cast-method-call-not-parsed]] | `(x as IFoo).m()` -> `unknown function '(xasIFoo)'`. Operand-shape independent. |

## Open - latent / no repro found

| Issue | Severity |
|---|---|
| [[core-bitcode-may-cache-bodyless-rebox-thunk]] | Unreachable today; trips when any core file reachable from `runtime.cb` gains an interface-to-interface conversion. |
| [[iface-arg-lambda-fnptr-type-not-propagated]] | No failing shape found; recorded with what was tried. |

## Open - follow-ups and shelved work

- [[iface-namespace-follow-ups]] - items 2-6 of the round-1 review of `c9acb6c`. Item 1 is
  RESOLVED (`853cb87`). Item 5 (annotation/template key split) is the one reachable only
  on the Windows `[uuid]` / `[winrt]` path.
- [[iface-ifconst-blame-attempt-shelved]] - READ BEFORE attempting the `if const` blame
  diagnostic again. A serious attempt shelved after eight review rounds / nine defects.
  Branch `fix/iface-ifconst` @ `23418c2`; the linked worktree was on the macOS box and is
  not present in this checkout.

## The structural theme

Three separate issues above are instances of one pattern, stated in full in
[[as-boxing-skips-ownership-transfer]]: **interface boxing bookkeeping is duplicated
across four sites** - assignment, return, `?:`, and `as` - and shapes keep falling
through the gaps between them. The frame-lifetime check added in the 2026-07-28 `as`
fix is itself the fourth copy, and recovers information by walking emitted IR precisely
because the boxing site recorded nothing.

A third theme, from the named-arguments work: **replay loops report the first failing
candidate rather than the relevant one**. Two entries above are that shape
([[named-arg-replay-reports-losing-candidate]], [[iface-slot-replay-blames-wrong-slot]]).
Both files agree the durable fix is a single `ScoreCandidates(probe)` helper called twice,
which also removes the desync hazard of two hand-maintained loop pairs.

A second, smaller theme: `GenerateSafeCast` / `GenerateIsCheck` decide "concrete source"
by pattern-matching the operand's LLVM type and fall through to the interface-source path
on anything unrecognised. Two crash issues above are that fall-through
([[as-cast-pointer-ternary-operand-compiler-crash]],
[[as-cast-array-shaped-source-no-diagnostic]]). The fix direction both files agree on is a
positive routing decision - classify the source explicitly and `LogError` on anything
matching no category - which closes the whole family at once.

## Adjacent - found during interface reviews, not interface bugs

[[constructor-discriminator-inconsistent-name-only-sites]],
[[array-view-params-unconditionally-noalias]],
[[expect-error-leaves-outer-nullcond-block-unterminated]].
