# Issue queue

The index for `internal/issue/`. Started as an interface-only tracker and is now the index for
everything here - several entries below say "filed here because it has no other queue", which
is why the family headings replaced the old interface-first framing.

Not an issue itself, and **the only non-issue file in this directory**. Each row points at the
file that owns the detail. When an issue is fixed its file is deleted (the repo convention);
delete its row here in the same change. `internal/issue/` holds ACTIVE items only.

Layout: every issue file lives in a subfolder - **[`p1/`](p1/)**, **[`p2/`](p2/)**,
**[`p3/`](p3/)** by fix priority (P1 highest), plus **[`ui/`](ui/)** for the separate UI / Win32 /
WinRT track, which gates no compiler work and is not ranked against them. This file is the only
thing at the top level. Every file is indexed below; when you re-bucket a row, `git mv` the file
in the same edit.

Two things deliberately live elsewhere:

- **Durable, cross-cutting lessons** - how to review, how to sequence rounds, guard polarity,
  what to distrust in an agent report - are in
  [`internal/fix-issue-lessons.md`](../fix-issue-lessons.md). They outlive any one issue, so
  deleting a fixed issue must not delete them.
- **Suite mechanics** (SKIP list, warm-cache pass, the `--init` serializer rule) are in
  [`internal/testing-notes.md`](../testing-notes.md).

What stays HERE besides the index: the **landed design records** at the bottom - the account of
why the shipped code has the shape it does, which approaches must not be retried, and every
ratified behaviour change that future work must not "fix" back. That section is the convergence
point for the interface/generics work and is the reason this file is long.

State on 2026-08-01: **75 open issues** (11 P1 / 33 P2 / 24 P3 / 7 UI), counted from disk
(`ls internal/issue/p{1,2,3}/*.md ui/*.md | wc -l`) on the merged tree, not from arithmetic.

**Read the P1 count honestly: the 2026-08-01 campaign fixed SIX P1s and P1 went from 11 to 11.**
Fixed and deleted: `funcptr-call-result-into-closure-param-garbage`,
`unique-field-to-field-copy-double-frees`, `delete-of-array-view-over-stack-storage`,
`interface-method-call-on-null-value-segfaults`,
`unique-field-to-field-residue-temp-and-interface-source`,
`generic-unique-field-temp-source-crashes-compiler`. Filed in their place: six new issues, EVERY
ONE found by the ADVERSARIAL REVIEWS of those fixes rather than by the fix work or the original
investigation.

**This is the campaign's central finding, and it should change how the next one is scoped.**
Driving P1 to zero by count does not converge, because reviewing a fix in this area reliably
surfaces one or more neighbouring defects. What DID converge is severity: the six closed were
silent double frees, silent wrong values, and two zero-output COMPILER CRASHES; the six filed are
narrower, diagnosed, and mostly wrong-diagnostic or unprovable-shape gaps. Scope the next campaign
on the SEVERITY MIX (are there silent wrong values left?), not on the count reaching zero.

Two items also ended as deliberate NON-fixes, which is the queue working as designed rather than a
shortfall: `return-dangle-missed-when-slot-has-extra-user` (its own file says do not patch it, and
the prerequisite it names does not actually unblock it - RECLASSIFIED to P3 on 2026-08-02 by the
maintainer, since a permanent non-fix does not belong in the P1 working set). The other,
`ftell-fseek-long-width-on-windows`, was parked rather than declined and is now FIXED - see the
landed design record below. And `interface-field-self-assign-false-positive` was ATTEMPTED and
REVERTED - see its file for the three discriminators that cannot work.
## Resume point

**Current head: the P1 campaign.** macOS arm64 Release **576 / 0 / 8** plus `example_mac.sh`
**35 / 0** and `test_lsp.sh` **152 / 0** (re-measured 2026-08-03; the 554 this line carried was
stale by 22 tests). Six P1s fixed on 2026-07-31 (`99d73f3`, `696060d`, `8c29ca7`, `d65f000`, `4000fa1`,
`4097959`) and two more on 2026-08-01 (`funcptr-call-result-into-closure-param-garbage`,
`unique-field-to-field-copy-double-frees`); the design records are at the bottom. **P1 is being
cleared before P2 starts.** (`iface-ifconst-base-clause-implementor` has since LANDED via the
revived `fix/iface-ifconst` branch - see its landed record at the bottom.)

> Deliberately no commit hash here. Every previous revision of this line named a `master` that
> was stale within the day (it has read `4097959` and `3b6e3e8` in turn, each wrong by the time
> anyone read it), and a stale hash asserting itself as current state is worse than no hash.
> Run `git log --oneline -1` instead.

- `interface-method-call-on-null-value-segfaults` - SETTLED and now FIXED; see the landed design
  record below. The ratified rule survives there, not here: reject at COMPILE TIME as far as is
  provable, NO per-dispatch runtime guard, `?.` is the answer for anything not provable. Residue is
  LIVE and now tracked in
  [`internal/plan/null-interface-access-widening.md`](../plan/null-interface-access-widening.md) -
  it was `p1/null-interface-access-residue-unproven-receivers`, merged into that plan and deleted
  2026-08-03.
- `ftell-fseek-long-width-on-windows` - FIXED on a Windows host 2026-08-02 and deleted; see the
  landed design record below. The >2 GB half it deferred is now `p2/file-offsets-capped-at-2gb`.
- `llvm-cannot-select-sign-extend-on-const-array-index` - FIXED and deleted 2026-08-03 by
  `b7181e6` (verified an ancestor of HEAD). All four spellings the file listed as live now reject
  with a clean front-end diagnostic and exit 1, where they previously died at SIGSEGV 139 with no
  output - including the two the file recorded as SILENTLY MISCOMPILING (`%s` whole-row read and a
  runtime index), which are now caught upstream at the row assign. **Carry this forward:** the
  BACKEND node was never diagnosed. Only the front-end shapes that feed it are closed, so a new way
  to store a folded `ConstantExpr` into fixed-array storage could reach it again. That is the
  standing rule from CLAUDE.md - an LLVM-level failure gets a front-end `LogError`, and the axes
  (decl-init, assignment, compound assignment, parameter, return, field) get ENUMERATED before
  anyone writes "no live repro".
- `generic-type-alias-arg-not-resolved` - FIXED 2026-08-02 and deleted; see the landed design
  record below (fix/alias-mangling). The pure-rename alias set is now pre-registered ahead of BOTH
  passes, which is the part a future session must not undo.

Next P1s, and the sequencing that matters:

- ~~`auto-binding-of-fixed-array-loses-shape` and `fixed-array-copy-invalid-bitcast`~~ - LANDED.
  `fix/array-shape` shipped: `fixed-array-copy-invalid-bitcast` is fixed and its file deleted, and
  `auto-binding-of-fixed-array-loses-shape` was narrowed and RE-RANKED P1 -> P2 (it now sits in the
  P2 table, not here). The two language decisions settled going in are ratified and still binding:
  `auto x = <fixed array>` deduces the VIEW `T[]` (a borrow - `auto` introduces no storage), and
  `int[3] b = a;` IS a copy lowered as a memcpy (the declared type allocates its own storage, so it
  cannot alias). Stale-bullet corrected 2026-08-03.
- `interface-boxing-keyed-on-source-binding` and `return-dangle-missed-when-slot-has-extra-user`
  are the next group. (`interface-type-alias-not-resolved-in-is-as-target`, formerly grouped
  here, is fixed on `fix/iface-alias` - not yet merged to master.)
- `ifconst-const-global-condition-corrupts-ir` is fixed and merged (`4c2b2d3`).
  `null-conditional-args-eval-order` is fixed on `fix/nullcond-order` for every
  pointer-guarded '?.' spelling; its two residues are narrowed to
  `p3/null-conditional-args-eval-order-hresult` (HResult/COM half) and
  `p3/nullcond-guard-skips-move-argument-cleanup` (the move-argument leak the guard
  introduces on the null path).

The four function-pointer / closure P1s fixed on 2026-07-31 left FIVE residues, all filed rather
than implied. `fix/funcptr-arg-accept-set` closes ONE of them outright
(`data-pointer-into-thin-function-param-segfaults`, deleted) and NARROWS a second:
`funcptr-overload-binding-ignores-signature` is closed for the type-CLASS axis and rewritten in
place for what remains (width, signedness, pointee, aggregates - see the landed record below).
That file went on to be narrowed twice more and was **fixed and deleted 2026-08-03** by
`fix/funcptr-close`, leaving one residue,
`funcptr-refuted-candidate-rebinds-onto-pointer-sibling` (P1) - itself fixed and deleted later the
same day by `fix/funcptr-rebind` (landed record at the bottom).
`funcptr-call-result-into-closure-param-garbage` (P1) is now fixed and deleted - see the landed
record at the bottom. Still open unchanged:
`data-pointer-returned-as-closure-not-gated`, `shape-mismatched-funcptr-arg-binds-silently` (P2).
They are one defect family - argument and return lowering for callable values - and a future pass
should scope them together rather than one at a time.

**`fix/iface-ifconst` has LANDED** (2026-08-04). The shelved attempt was revived, its one
outstanding defect (the blame fallback fabrication, defect 9) fixed by the known fix on record,
and the branch rebased onto master and merged. See the landed design record at the bottom.

---

**The generic namespace key space is DONE - all four layers, committed as `e2a23d5`.** That was
the head of this queue for four sessions; its issue file and its three corpora are deleted, and
the account of what shipped is under "Landed design records" below. Nothing in that family is
open except the separately-filed gaps listed there.

Verified on macOS arm64 Release at `e2a23d5`: `./test.sh` **536 / 0 / 8**, `./test_lsp.sh`
**152 / 0**, `Test/test_generics.cb` 132/132 (was 102), `Test/test_interface.cb` 92/92 (was 90).

**One claim in that work is UNSETTLED, and it is the thing to check first if `--init` ever
misbehaves: the `decl_ns` cache round-trip for the generic FUNCTION family.** A review probe
showed it load-bearing; the implementer could not reproduce that and showed the first probe was
vacuous (it put a caller next to the template, which pre-instantiates it, so the call resolved
through the cached `functions` table and the template read path never ran). Redone without the
caller, the read path is inert on macOS for a different reason: only `runtime.cb` is implicitly
imported, so every other core template comes from a file the program also `import`s, and
`ProcessImports` re-parses it and overwrites the lazily-registered entry - stripping `decl_ns`,
renaming the entry, deleting it, and filling its cached `source` with garbage all leave the
result unchanged. Both agree the conclusion (**the round-trip does not need to move**), which
rests on the code citations plus a byte-identical PRE/NEW `core_macos.bc`, not on either probe.

- The narrow, honest claim: `decl_ns` has a real write path, a real read path and **no live test
  on macOS** - the same status layer 1's `decl_ns` and layer 2's `interfaceTable` leg already
  have. It goes live the first time a core file declares a namespaced generic.
- **The decisive experiment nobody ran**: declare the namespaced generic in `runtime.cb` itself
  rather than another core file, `--init`, then compile a program that imports nothing. That is
  the only configuration where the lazily-registered entry is not overwritten by a re-parse.
- On Windows `core/os.windows.cb` declares 14 structs inside `namespace os.windows`, so dotted
  keys do reach the cache there and both legs are plausibly LIVE - a concrete cross-platform
  gap, not a routine disclaimer.

Other live state:

- `stash@{0}: review-fixes-and-untracked-plans` is **recovered and DROPPED** (2026-07-30). There
  are no stashes left. It held three untracked docs -
  [[macos-header-import-and-framework-link]], `internal/plan/macos-gui-cocoa.md`,
  `internal/plan/os-abstraction.md` - all three restored and now tracked; plus changes to four
  TRACKED files (`cocoa` host, `fedit`, `example_mac.sh`, `internal/plan/ui-native-framework.md`)
  whose content **had already landed on master**. The stash was based on `c287034`, 12+ commits
  back, and `doc/UI.md` plus `cocoa.cb` already carried its `performSelectorOnMainThread:`
  marshaling change; applying it conflicted only because those files were later moved and
  rewritten. Its commit was `d2db363`, recoverable with `git stash apply d2db363` until gc prunes
  it - but nothing unique remains in it.
- **`fix/iface-ifconst` LANDED on 2026-08-04** after the shelved attempt's defect 9 was fixed;
  the design record at the bottom carries what the shelving doc used to hold.
- The `as` / `is` family is **DONE**: routing (2 issues), boxing guards (2 issues) and the
  return dangle are all closed. What remains under `as` is diagnostic quality.
- The "may a user file-scope interface share a name with a core interface" product question is
  **RESOLVED** (hard error, shipped as `853cb87`). Do not reopen it.

## Open issues, by fix priority

`internal/issue/` is now bucketed by **fix priority**, one folder per bucket, plus the separate
UI track. **P1 is the highest.** The bucket is a fix-order judgment, not a restatement of
severity - re-bucket a row when the judgment changes, and move its file in the same edit.

| Bucket | Folder | Rule | Count |
|---|---|---|---|
| **P1** | [`p1/`](p1/) | The compiler produces a WRONG PROGRAM, or dies with no usable diagnostic. Silent wrong values, miscompiles, SIGSEGV/abort, verifier failures, missed lifetime errors. | 8 |
| **P2** | [`p2/`](p2/) | Legal code is REJECTED, a feature is unavailable, or an ownership guard has a hole that does not (yet) produce a wrong value. The program does not run, but nothing lies to you. | 40 |
| **P3** | [`p3/`](p3/) | Diagnostic quality, latent/no-repro, deliberate deferrals, and shelved attempts. Real, filed, and not blocking anyone. | 27 |
| **UI** | [`ui/`](ui/) | Separate track - UI / Win32 / WinRT parity. Gates no compiler work; not priority-ranked against the compiler buckets. | 7 |

Counts re-verified from disk on 2026-08-01 (`ls internal/issue/p{1,2,3}/*.md ui/*.md | wc -l` per
bucket) on the MERGED tree, after both of this round's P1 fixes landed:
**11 P1 / 33 P2 / 24 P3 / 7 UI = 75 total**, up from the 73 recorded 2026-07-31.

Net movement: two P1s fixed and their files deleted (`funcptr-call-result-into-closure-param-garbage`,
`unique-field-to-field-copy-double-frees`), and four new issues filed - three P1
(`unique-field-to-field-residue-temp-and-interface-source` and
`generic-unique-field-temp-source-crashes-compiler`, both since FIXED by `3d33bfe`; plus
[[interface-field-self-assign-false-positive]], still open) and one P2
([[generic-funcptr-return-poisons-enclosing-return]]). Every one of the four was found by the
ADVERSARIAL REVIEWS of the two fixes, not by the original investigation - the same ratio this
file recorded on 2026-07-31.

> Counted on the merged tree deliberately. Each branch recounted from a disk that lacked the
> other's files, so BOTH branch headers were wrong (one said 9 P1 / 33 P2, the other 12 P1 /
> 32 P2). When two branches that both touch this file are in flight, the only trustworthy count
> is the one taken after the second merge. Not hand-arithmetic - counted directly per the
> command above.

Counts re-verified from disk on 2026-08-02 (same `ls | wc -l` per bucket), on `fix/global-positional`:
**9 P1 / 37 P2 / 27 P3 / 7 UI = 80 total.** The bucket table above was stale before this
recount (it still read 11/33/24, but the true pre-existing disk count on `master` at the time
was already 10 P1 / 34 P2 / 26 P3 - drifted from unrelated fixes/filings between 2026-08-01 and
this round, never caught because nothing had re-verified the table since). Net movement this
round: one P1 fixed and deleted ([[global-struct-positional-init-silently-zeroes]]), and four
new issues filed in its place - three P2 ([[class-no-ctor-default-construct-returns-undef]],
[[struct-field-default-brace-list-discarded]], [[interface-typed-global-brace-init-discarded]])
and one P3 ([[global-struct-no-initializer-ignores-field-defaults]]). All four were found by
review of that P1's fix, not by the original investigation - the same pattern this file has
recorded before (see the 2026-08-01 paragraph above).

A further review round of the same fix filed a FIFTH: `pointer-decl-field-init-brace-corrupts-pointer-storage`
(P1 - `S* p {a=1};` writes a nonsense address into `p` itself). **FIXED and deleted 2026-08-02** by
`fix/ptr-fieldinit`; see the landed design record below. That fix in turn filed two more from its own
Phase A enumeration - `empty-brace-initializer-never-seeds-and-crashes-on-defaults` (P1, since
**FIXED and deleted** by `fix/emptybrace` / `b844137`; see the landed design record below) and
[[string-literal-containing-braces-retyped-as-string]] (P2) - which is the same pattern again.
(`ftell-fseek-long-width-on-windows` fixed 2026-08-02; its >2 GB half re-filed as
[[file-offsets-capped-at-2gb]] at P2.)

Counts re-verified from disk on 2026-08-03: **8 P1 / 40 P2 / 27 P3 / 7 UI = 82 total.** The bucket
table above read 10/38/27/7 before this recount and is now corrected. Two integrity checks were run
alongside the count, and BOTH should be re-run whenever this table is touched, because the count
alone does not catch either failure:

- **Every table row resolves to a file** - no row naming a deleted issue. (Clean.)
- **Every file has a table row.** This one FAILED: `file-offsets-capped-at-2gb` had been filed in
  the narrative above and never given a P2 row, so it was invisible in the priority list for a day.
  Row added 2026-08-03. A file with no row is the failure mode this section had no guard against -
  a bare `ls | wc -l` recount would have counted it and still left it unlisted.

Net movement this round: **one P1 fixed and deleted** (`llvm-cannot-select-sign-extend-on-const-array-index`,
by `b7181e6`), and no new issues filed. The remaining 8 P1s were each re-run against the current
Release binary on 2026-08-03 and every one still reproduces exactly as its file documents - including
`neigh=2333` on `funcptr-overload-binding-ignores-signature`, unchanged since the funcptr rounds.
The P2/P3/UI buckets were NOT re-triaged this round; their rows carry whatever their last
verification said, so do not read this recount as a statement that they are all still live.

Later the same day, `fix/funcptr-close` closed the last two items of
`funcptr-overload-binding-ignores-signature` and that file was deleted, with one residue split out
as `funcptr-refuted-candidate-rebinds-onto-pointer-sibling` (P1). **The bucket count was unchanged
at 8 P1** - one file out, one file in - which is exactly the case a bare recount cannot
distinguish from "nothing happened", so both integrity checks above were re-run and are clean.
`fix/funcptr-rebind` then fixed and deleted that residue the same day. That change is
count-neutral for P1: one file out, and
[[code-value-into-data-pointer-outside-overload-resolution]] in, recorded rather
than left implicit when that fix was scoped to argument binding. It also added
`p2/c-binder-misses-decorated-function-pointer-parameter`, found while verifying its oracle.

### P1 - wrong programs and crashes (`p1/`)

| Issue | Severity |
|---|---|
| [[interface-boxing-keyed-on-source-binding]] | NARROWED 2026-07-31, then CLOSED 2026-08-04: `delete` of a BORROWED interface box now diagnoses at the boxing site via a positive keeps-owner proof (six clauses, join rule for `??=`; see the file for the accepted gaps). No longer a P1 - only the preventive `RegisterInterfaceBox` dedupe remainder is left. |
| null-interface-access residue - **FIXED 2026-08-03 in three stages**; design record at [`internal/plan/null-interface-access-widening.md`](../plan/null-interface-access-widening.md), remainder filed as [[null-interface-access-remaining-storage-kinds]] | Was SIGSEGV (139), no diagnostic. Stage 1 (`9e7ffc4`) re-keyed the proof on (frame-local alloca, constant index path), closing struct-field and array-element receivers - and `PHolder h;` / `h = {}`, which turned out to be provably null via the synthesized default ctor. Stage 3 (`727f53d`) made it cross-block with a **MUST** lattice (intersection, NOT `nulldf`'s union - see the plan's section 5; a MAY merge false-rejects accept leg 2) plus control-dependence containment, closing the four local spellings where intervening control flow dropped the diagnostic. Stage 2 closed whole-global receivers via a whole-module never-written fact AND the CD test - **neither alone is sound**, see probe e3. A field/element of a global aggregate was closed 2026-08-03 (new `ResolveIfaceStorageGlobal` walks a `GEPOperator` chain back to the `GlobalVariable` base). Still open and filed separately: a bare `PLive lv;` with no initializer (exit 133, a genuinely uninitialised read, not a null one). |
| [[code-value-into-data-pointer-outside-overload-resolution]] | Exit 138, no diagnostic, identical on `904f026` and on `fix/funcptr-rebind`. `Rec* r = w;`, `return w;` and a `b.p = w;` field store all put a function-pointer VALUE into a data pointer and write through it. Recorded by review round 1 of `fix/funcptr-rebind`, which closed the OVERLOAD-BINDING path of the same defect class (`ComputeOverloadFunction` plus its variadic short-circuit) and deliberately did not reach the store paths - the shared predicates `ArgumentIsCodeValue` / `ParameterStoresData` exist, what is missing is a destination-side reader. Build the accept set first; an explicit cast must keep working. |
| [[interface-field-self-assign-false-positive]] | Silent abort (exit 134), no diagnostic. An interface-field-to-interface-field copy with the SAME field name on both sides is misread as a self-assign, suppressing the reject. **ATTEMPTED AND REVERTED 2026-08-01** - see the file for the three discriminators that cannot work (names, box storage, and a bare `Value` compare of the field address all fail at least one witness). A sound test needs real dataflow through the box to the underlying data pointer. |
| [[unique-field-to-field-array-element-receiver]] | Silent abort (exit 134), no diagnostic. **NARROWED 2026-08-02** by `fix/uniq-array-elem`: root cause CONFIRMED by instrumentation (`FieldName`/`CallerName` name the CONTAINER, so `selfFieldAssign` swallowed a genuine two-owner store), and every element pair whose addresses are constant-provably different now rejects - local, generic-substituted, nested, array-as-field, through-pointer, view, and global arrays. BOTH copies of the name comparison were fixed; the second (`sameField`) also guards the reassignment-destruct, and fixing only the first traded an abort for a leak. Residue: any index not constant in the emitted IR - a runtime subscript, and a `const` integer (`const` is unenforced, so folding it would be unsound). |
| [[unique-field-global-struct-self-assign-false-positive]] | Silent abort (exit 134), no diagnostic. `gA.slot = gB.slot` on two file-scope structs. **Mechanism CORRECTED 2026-08-02** (the first filing read optimized IR and blamed `destIsStructField`; at `--no-opt` that predicate is TRUE and the stack IS entered): a global-struct field read carries an EMPTY `CallerName`, so `selfFieldAssign` reads two different globals as one slot - the same failure as the interface receiver, through a third receiver kind. Renaming the fields apart rejects on both binaries. Closable by extending `ProvablyDifferentSlots` to distinct `GlobalVariable`/`AllocaInst` roots, which is sound but is a widening of a predicate every field store flows through - do it with its own sweep. |
| [[temp-unique-field-into-borrow-slot-use-after-free]] | Use-after-free, compiles clean and exits 0 on both binaries. A temp's `unique` field bound into a NON-unique (borrow) slot with a destructor-less pointee: the temp's `Box` destructor frees the pointee before the load. No `unique` claim on the destination for a guard to key on. |

### P2 - false rejections, unavailable features, ownership holes (`p2/`)

| Issue | Family | Severity |
|---|---|---|
| [[chained-nullcoalesce-not-boxed-into-interface]] | false rejection | `take(z ?? y ?? a)` and `IShape j = z ?? y ?? a;` do not compile - the outer join's arm is the inner join's LOAD, which names no class. Spans EVERY position that boxes a `??` (decl-init, assignment, return, argument), so it predates the return/argument work. Fix at the ledger by splicing a nested join's arms. Filed 2026-07-31. |
| [[pointer-arg-binds-by-value-class-param]] | miscompile | `byVal(a)` with a `Circle*` and a by-value `Circle` parameter scores a PERFECT match and lowers a raw pointer into a struct slot - module-verifier dump, NO source location, exit 1. `IsTypeMatch` compares TypeName and ignores `Pointer`; the sibling `IsTypePromotion` does gate on it. PRE-EXISTING and language-wide, no join involved. Scorer change - wide blast radius, wants the corpus sweep. Filed 2026-07-31. |
| [[generic-wrapper-over-function-type-llvm-fatal]] | feature gap | `Box<function<int(int)>>` raises LLVM fatal `Cannot select: AArch64ISD::CALL` (exit 134) when the substituted field is INVOKED. Store-only may be fine - check that first. Borderline P1 (dies with no usable diagnostic); filed P2 because nothing lies to you. Filed 2026-07-31. |
| [[generic-funcptr-return-poisons-enclosing-return]] | false rejection | Legal code rejected with a P1-grade diagnostic: a raw module-verifier dump (`%thinret`) and NO source location. CALLING a generic function that returns `function<>` routes the ENCLOSING function's `return` through `CoerceToFuncPtrReturn`. `main` does NOT escape; `void` and POINTER returns escape (the latter by a no-op ptr-to-ptr bitcast, which is why this is P2 and not P1 - no spelling is silently wrong). A second, different failure on the no-value-parameter spelling is recorded in the file; do not conflate. Filed 2026-08-01. |
| [[multidim-fixed-array-has-no-brace-initializer]] | feature gap | A multi-dimensional fixed array has no working brace initializer on either binary: nested braces are a PARSE error, a flat list counts against the OUTER dimension only (`int[2][3] a = {1,2,3,4,5,6}` -> "too many initializers for 'int[2]'"), and string-literal elements hit the fixed-array pointer-store reject. `= default` plus element assignment works. Matters because `fix/mdview`'s diagnostic points at `T[N][M]`. Fix the FLAT list first (multiply through `ConstInnerDimensions`); nested braces need a grammar change. Filed 2026-08-02. |
| [[auto-binding-of-fixed-array-loses-shape]] | feature gap | RESTORED and narrowed, re-ranked P1 -> P2. The non-pointer half is fixed; `auto v = <T*[N]>` now REJECTS because `T*[]` collapses to `T[]` in both parser copies. Representation is free - no new field needed. |
| [[extern-decl-drops-fixed-array-return-size]] | silent wrong ABI | `extern char[8] extmk();` compiles clean on BOTH `ca5a02a` and `fix/array-storage` - the `[8]` is dropped and the declaration binds to a symbol returning one `char`. The by-value fixed-array-return reject landed on the DEFINITION path only; this is the one remaining spelling of that axis. Not a regression. Filed 2026-08-02. |
| [[char-array-from-string-literal-has-no-spelling]] | feature gap | `char[N] b = "literal";` now has a clear diagnostic and three suggested spellings, but no direct replacement for the C idiom. Master miscompiled it silently. |
| [[array-view-params-unconditionally-noalias]] | latent miscompile | Latent `-O2` miscompile hazard - UB handed to LLVM. P1 the moment a witness exists. |
| [[data-pointer-returned-as-closure-not-gated]] | miscompile | Silent miscompile then SIGBUS (exit 138), no diagnostic. `CoerceToFuncPtrReturn` is the one caller of `WidenBareOrThinToClosureFat` never routed through the `ce9858e` provenance gate, so a data pointer returned as a closure lands in the CODE slot and is called. Filed 2026-07-31. |
| [[shape-mismatched-funcptr-arg-binds-silently]] | miscompile | Silent miscompile then SIGBUS (exit 138), no diagnostic. A `function<T>*` binds where a plain `function<T>` value is expected; the scorer now detects the shape mismatch but still lowers the mismatched arm when no better-shaped candidate exists. Filed 2026-07-31. |
| [[lambda-pointer-as-generic-type-arg-bypasses-guard]] | diagnostic | Hard compile failure with no source diagnostic - raw module-verifier dump. `Lambda<T>*` as a generic type argument (`Box<Lambda<int(int)>*>`) bypasses the declarator guard that already rejects this shape elsewhere. `function<T>*` as a generic type argument fails identically but should be SUPPORTED, not rejected - the two halves want opposite outcomes. Filed 2026-07-31. |
| [[list-of-function-element-into-closure-param-fails-verifier]] | diagnostic | Hard compile failure with no source diagnostic - raw module-verifier dump. Passing a `list<function<>>` element to a closure parameter fails; building the list and invoking the element directly both work. Likely shares a root with [[generic-wrapper-over-function-type-llvm-fatal]]. Filed 2026-07-31. |
| [[incomplete-layout-message-blames-c-interop]] | diagnostic | **Raised above its severity.** One emission site, three unrelated causes, and the wording names the cause that is usually absent. Two ratification records cite a C-interop cause on files with no C interop. |
| [[overload-replay-blames-wrong-candidate]] | diagnostic | Factually false message on two paths; on the interface-slot path it converts a success into a failure. Merged 2026-07-30. |
| [[variadic-free-generic-function-does-not-link]] | false reject | Compiles, does not link - raw JIT symbol dump, not a diagnostic. |
| [[namespaced-struct-static-method-not-dispatched]] | false reject | A whole dispatch form is unavailable inside a namespace. |
| [[namespaced-interface-shadowed-by-global-is-broken]] | false reject | False rejection with a nonsense diagnostic. Non-generic controls fail on both binaries. |
| [[namespaced-using-alias-leaks-globally]] | false reject | Name leak / silent shadowing. Also the reason a layer-2 accept-set limit is only conditionally safe. |
| [[tuple-sugar-in-namespace-does-not-compile]] | false reject | A whole syntax is unavailable inside a namespace. |
| [[paren-as-cast-method-call-not-parsed]] | false reject | `(x as IFoo).m()` -> `unknown function '(xasIFoo)'`. Parser, not diagnostics. |
| [[generic-interface-name-vetoed-by-core-template]] | false reject | A core generic template vetoes a same-named user generic interface. Two tie-breaks tried, both reverted - records why none can work. |
| [[generic-interface-cannot-inherit-generic-interface]] | false reject | `unknown parent interface` on INSTANTIATION, not on the declaration. |
| [[fixed-array-parameter-not-callable]] | false reject | A `T[N]` parameter registers as a bare `T`, so no call resolves. |
| [[sizeof-of-generic-instantiation]] | false reject | `sizeof(B<int>)` -> `unknown type`. The operand skips the generic mangling/queue path. Check `alignof` and cast operands too. |
| [[function-type-as-generic-interface-type-argument]] | false reject | `C<function<int(int)>>` fails on both binaries. Clean failure. |
| [[bare-interface-name-resolves-outward-before-namespace]] | false reject | Outer scope wins for non-generic interface names, opposite to the ratified generic rule. |
| [[macos-header-import-and-framework-link]] | false reject | Two gaps block first-class Apple-API binding: header import hard-codes a Linux triple on Darwin (`objc/runtime.h` registers 1 of ~80 functions), and there is no `-framework` / `-F` link channel. The macOS demos work around both with dlopen + typed `objc_msgSend` casts. |
| [[unique-assign-syntactic-owned-rhs-leaks]] | ownership | Owning value laundered through a BORROW-returning call still leaks. |
| [[alias-borrow-local-launder-gaps]] | ownership | An `IsAliasBorrow` owning-struct local launders its borrow through `=` and through `move`. |
| [[delete-borrow-via-named-local]] | ownership | Opt-in spelling closes it; the bare case is still open. |
| [[deref-of-moved-pointer-guard-inside-callee]] | ownership | False positive: guarded only by a conditionally-terminating callee. |
| [[owning-temp-ledgers-should-be-split]] | ownership | `ownedReturnTemps_` fails UNSAFE, `ownedNewTemps_` fails SAFE. |
| [[detection-ledgers-not-discarded-on-aborted-arm]] | ownership | Detection-only ledgers survive an aborted `?:` arm. |
| [[class-no-ctor-default-construct-returns-undef]] | miscompile | A `class` with no user-written constructor default-constructs to IR `undef`, not zero - the synthesized zero-arg constructor's body never stores anything before returning. The `struct` twin (same fields, no constructor) is correct (real zero-init). Found while reviewing the fix for `global-struct-positional-init-silently-zeroes` (FIXED and deleted - see the `fix/global-positional` landed record below); unrelated root cause. Filed 2026-08-02. |
| [[struct-field-default-brace-list-discarded]] | miscompile | A struct FIELD's own `= { x = 1, y = 2 }` default brace list is silently discarded when the containing struct is default-constructed - the field lands all-zero instead. Found auditing the same fix for neighbouring shapes; different code path (a field's default expression, not a variable declarator). Filed 2026-08-02. |
| [[interface-typed-global-brace-init-discarded]] | miscompile | `I gi = { a = 1 };` on an interface-typed global compiles clean with the brace list silently dropped, no diagnostic. The `fix/global-positional` guard (that P1 is fixed and deleted; see its landed record below) cannot see it - `GetDataStructure("I").StructType` is null for an interface name, so this falls through unguarded. Found by review of that fix. Filed 2026-08-02. |
| [[file-offsets-capped-at-2gb]] | silent wrong value | `core/filesystem.cb` narrows every offset through `int`, so `File.size()`/`tell()`/`seek()` truncate past 2 GB on ALL platforms - the public surface is `int` too, so widening the internals alone is not enough. Split out of `ftell-fseek-long-width-on-windows` when that P1 landed 2026-08-02; NOT the `long`-width defect, which is fixed. Had no row in this table until 2026-08-03 - it was filed in narrative only. |
| [[string-literal-containing-braces-retyped-as-string]] | miscompile + false rejection | A string literal whose CONTENT contains a brace pair (`"a = {} b"`) is typed `string` instead of `char*`. At a call it stops every `char*` overload matching and the diagnostic blames the call; at a VARIADIC it is a SILENT MISCOMPILE - `printf("a = {} b\n");` compiles and runs rc 0 printing binary garbage, and the dedicated `cannot pass 'string' to the variadic '...'` guard does not fire. Identical on both binaries. Filed 2026-08-02 in `p2/` for the rejection face; the miscompile face may warrant P1. |
| [[simd-type-spelling-unusable-outside-declarations]] | feature gap | `simd<T,N>` is recognised only in `ParseDeclarationSpecifiers` and as a `primaryExpression`, so a cast target, a lambda parameter and a tuple/`function<>` signature component all say "unknown type 'simd<float,4>'", and `simd<T,N>[]` silently DROPS the empty bracket and compiles as a plain vector local. Measured identical on `904f026` and `fix/simdptr`. Wants one encoded-name mechanism (mirroring `BuildEncodedClosureName`), not four patches. Filed 2026-08-03. |

### P3 - diagnostics, latent, deliberate deferrals (`p3/`)

| Issue | Family | Severity |
|---|---|---|
| [[owning-temp-parent-misroutes-chained-alias-access]] | diagnostic | RE-RANKED P1 -> P3 2026-08-02, on the file's own "re-rank freely" and on a re-measurement: the VERDICT is right (two `unique` owners really is an error) and only the WORDING is wrong, so no program's accept/reject status changes. A wrong message is P3 by this table's own rubric; it sat at P1 only for visibility to whoever next touched the `unique` field-store routing, and that work has landed. Still live on `ca5a02a`: the call-result message fires on a container-element shape, stating a false mechanism and naming a remedy that aborts 134. |
| [[return-dangle-missed-when-slot-has-extra-user]] | deliberate deferral | RECLASSIFIED P1 -> P3 2026-08-02 by the maintainer, rationale kept intact. Missed dangle, no diagnostic - but the shapes were ALL accepted before `2bcc5a0` too, so this is residue, not a regression, and its own file rules out the obvious remedy (widening the extra-user whitelist re-introduces false rejections). Stays open as a record of what the pass cannot see. |
| [[nullcond-guard-skips-move-argument-cleanup]] | latent | INTRODUCED by the `null-conditional-args-eval-order` fix, and accepted rather than fixed. A `move` argument to a '?.' call on a NULL receiver never runs, so nothing takes ownership - but the source is still statically marked moved, so scope exit frees nothing either, and the allocation leaks (`frees=0` vs master's `frees=1`). Not memory-unsafe and not observable in-language; reading the source after the call is rejected identically on both binaries. Filed 2026-07-31. |
| [[null-conditional-args-eval-order-hresult]] | latent | Residual of the fixed P1 `null-conditional-args-eval-order`. On an `HResult<T*>` receiver '?.' means "propagate the failure code", and its `chain.ok`/`chain.fail` lowering still runs after the argument list is evaluated - so a failed HResult skips the call but not its arguments' side effects. COM/winrt only, therefore Windows only; not reachable from any in-repo `.cb` on macOS. Narrowed 2026-07-31. |
| [[mangled-generic-name-leaks-into-diagnostics]] | diagnostic | `Box__unique_Itemptr` shown where the user wrote `Box<unique Item*>`. Also a TEST-FRAGILITY problem: pinning an `expect_error` to a mangled name pins it to the mangling scheme. **Prefer prefix-pinning until fixed.** No demangler exists and `MangleTypeArg` is lossy one-way, so the fix is to STORE the source spelling. Filed 2026-07-31. **BROADENED 2026-08-02**: a second site (function-pointer signature mismatch prints `'void(Box__i32*)'` vs `'void(Box__double*)'`, plus internal `__c_fn_ptr` tokens), and the predicted test-fragility FIRED - two unrelated `expect_error` legs broke when `MangleTypeArg` changed. Fix now needs a mangled-key -> spelling registry, not just a `StructData` field. |
| [[function-pointer-to-fixed-array-not-rejected]] | diagnostic | `function<T>[N]*` silently accepted while `int[N]*` is correctly rejected - the funcptr branch breaks before the `ArrayPtrOf` check. Two-line fix in BOTH `ParseDeclarationSpecifiers` copies; fold into whatever next touches that branch. Filed 2026-07-31. |
| [[sizeof-of-sized-array-type-parsed-as-cast]] | diagnostic | `sizeof(T[N])` is parsed as a cast and rejected with a message about CASTS - blaming a construct the user never wrote. Not multi-dim specific (`sizeof(int[3])` fails too); `sizeof(variable)` works. Filed 2026-07-31. |
| [[funcptr-fixed-array-vs-view-overloads-collide]] | diagnostic | Silent overload loss, no diagnostic: `function<T>[N]` and `function<T>[]` overloads of the same name collide onto one mangled key and one shape, so the last-registered overload silently wins. Low severity - the two spellings are arguably the same parameter type. Filed 2026-07-31. |
| [[generic-function-call-diagnostics-are-misleading]] | diagnostic | Three defects on one path: a PHANTOM candidate invented for an undeclared generic function, wrong type-arg arity reported as "unknown function 'D3.id__int__float'", and a mangled name leaking into user-facing text. Pre-existing, identical before `e2a23d5`; filed 2026-07-30 out of the layer-4 review. |
| [[simd-array-error-wording-differs-from-plain-arrays]] | diagnostic | `simd<T,N>[N]` gets DIFFERENT wording than a plain array for the identical rejection (whole-array assignment, global fixed-array init): two `!IsSimd` guard exclusions in `MainListener.h` became reachable when `fix/simdptr` started recording the dimension, so a second guard catches the shape and prints a different message. Both spellings are still rejected - wording only, no miscompile. Deliberately deferred: removing `!IsSimd` WIDENS a rejection predicate and needs an accept-set exercise first. Filed 2026-08-04. |
| [[simd-array-view-decl-verifier-failure]] | diagnostic | `simd<T,N>[] v = a;` (view bound to a `simd<T,N>[N]`) emits `Invalid bitcast ptr to float` and dies in module verification with NO located diagnostic - CLAUDE.md requires a `LogError` before the verifier trips. Identical pre-amend and at `fix/simdptr` HEAD; on `master` the same file exits 0 only vacuously (the `[2]` was dropped, so there was no array to view). Distinct from the p2 spelling issue: the spelling parses, the view BIND lowers against the lane type. Filed 2026-08-04. |
| [[interface-collision-message-prefix-still-basename]] | diagnostic | The `file(line,col):` prefix is still a bare basename. |
| [[as-cast-unbound-pointer-shape-generic-message]] | diagnostic | Correctly rejected, generic wording. Struct field and LOCAL `T*[N]` only. |
| [[constructor-discriminator-inconsistent-name-only-sites]] | diagnostic | Name-only outside a lock/program body, null-declarationSpecifiers inside one. |
| [[expect-error-leaves-outer-nullcond-block-unterminated]] | diagnostic | Raw verifier dump instead of a clean diagnostic. |
| [[failed-expect-error-type-poisons-its-name]] | false reject | Contained to the declaring file, and test-only. Not repairable from the generic accept set. |
| [[unique-array-view-accepted-as-generic-type-argument]] | accept set | Inconsistent accept set, no miscompile shown. |
| [[duplicate-generic-template-name-silently-accepted]] | accept set | Undocumented "struct wins" tiebreak, no diagnostic. `Test/test_generics.cb` depends on the collision, so the obvious backstop cannot ship. |
| [[nodiscard-residual-gaps]] | ownership | Value-identity detection gaps. |
| [[thread-cannot-go-raii]] | ownership | Two independent blockers on giving `Thread` a destructor. |
| [[pools-no-destructor-shutdown-ordering]] | ownership | The pools stay manual - deliberately. |
| [[core-bitcode-may-cache-bodyless-rebox-thunk]] | latent | Unreachable today; trips when any core file reachable from `runtime.cb` gains an interface-to-interface conversion. |
| [[interface-lookup-alias-asymmetry-latent]] | latent | Follow-up from the now-closed `is`/`as`-alias-target fix (`fix/iface-alias`, not yet merged). Of 46 direct `interfaceTable.find/count` sites, 6 (`HasInterfaceMethod`, `FindInterfaceMethod`, `InterfaceDtorSlotIndex`, `EmitInterfaceFieldAddress`, etc.) are unreachable today because `TypeAndValue.TypeName` is always pre-resolved by declaration time - but 2 MORE (interface type-switch `case AliasX:` / arm-style `case AliasX* v`) are reachable RIGHT NOW, pre-existing on master; 32 sites remain untriaged. |
| [[iface-arg-lambda-fnptr-type-not-propagated]] | latent | No failing shape found; recorded with what was tried. |
| [[insert-block-liveness-not-audited-repo-wide]] | latent | `GetInsertBlock() != nullptr` is used repo-wide as a liveness test and is not one - nothing clears the insert point at end-of-function, so at declaration scope it points at the PREVIOUS function's terminated last block. `if const` was one instance (fixed, `fix/ifconst-ir`). SMALLER than it sounds: of 49 `GetInsertBlock()` uses in `LLVMBackend.h` only 3 are null-compares and only ONE (`12488`) is on the shared builder - and that one is unreachable by construction, since the only declaration-scope route needs a non-constant global initializer, which is rejected earlier. Read the fix-direction section before reaching for the one-line "just clear it" fix. |
| [[nondeterministic-ir-switch-case-order]] | methodology | No miscompile - a METHODOLOGY hazard. Read it before using "0 IR diffs" as proof. |
| [[iface-namespace-follow-ups]] | follow-up | Items 2-6 of the round-1 review of `c9acb6c`. Item 1 is RESOLVED (`853cb87`); items 4 and 5 were fixed by `15809e0`. Item 5's remainder (annotation/template key split) is reachable only on the Windows `[uuid]` / `[winrt]` path. |
| [[global-struct-no-initializer-ignores-field-defaults]] | miscompile | A global struct with NO initializer at all zeroes instead of honoring its fields' own `= default` expressions (`struct S { int a = 9; }; S gs;` reads `0`, not `9`). The LOCAL declarator handles this correctly (calls the default constructor). Lower severity than the sibling P1/P2 findings in this family because a working spelling exists. Found while reviewing the fix for `global-struct-positional-init-silently-zeroes` (FIXED and deleted - see the `fix/global-positional` landed record below). Filed 2026-08-02. |

### UI and Win32 (`ui/`)

Separate track; none of these gate compiler work. Design and staging are in
`internal/plan/ui-*.md`; the user-facing reference is [`doc/UI.md`](../../doc/UI.md).

| Issue | Area |
|---|---|
| [[ui-native-canvas-input-images-win32-winui]] | canvas input + images, Win32/WinUI parity gaps |
| [[ui-native-visual-polish-win32-winui]] | visual polish parity, Win32/WinUI |
| [[ui-boxed-closure-unguarded-null]] | boxed closure with no null guard |
| [[win32-classic-common-controls-v5]] | classic common controls fall back to v5 |
| [[winmd-scrollviewer-statics-vtable-mismatch]] | winmd statics vtable mismatch; same family as the next row |
| [[winrt-self-new-missing-vtable]] | `self` / `new` on a WinRT type has no vtable |
| [[winui-icontrol-get-template-misreads]] | projected interface whose `GetTemplate` misreads |


## The structural theme

**Bookkeeping duplicated across sites, each copy carrying a different subset of the guards.**
This is the single largest source of entries in this queue, and it has now produced two merged
issues rather than a dozen scattered ones:

- Interface boxing was open-coded at four sites - assignment, return, `?:`, `as`.
  `BoxConcreteIntoInterface` now carries every guard for every SINGLE-VALUE source (2026-07-31);
  the two JOIN spellings share `BoxInterfaceJoinArms`.
- Overload scoring is three hand-copied probe/replay loop pairs;
  [[overload-replay-blames-wrong-candidate]] wants one `ScoreCandidates(probe)` helper.
- Generic name resolution had three disagreeing key conventions;
  the generic key space was four layers of it - see the landed design records below.

The lesson worth carrying to the next duplication: **the guards were only as good as the
information reaching them.** Every fix in the boxing family was blocked on plumbing - getting
the source `NamedVariable` to `ParseTypeCheckExpression` - not on the guard logic, which
already existed and was correct. Look for the missing input before writing a new check.

A second theme, CLOSED and worth keeping as precedent: `GenerateSafeCast` / `GenerateIsCheck`
used to decide "concrete source" by pattern-matching the operand's LLVM type and fall through
to the interface-source path on anything unrecognised. The fix was a positive routing decision,
and it closed the family at once. Two transferable lessons:

- **Check the plain spelling before choosing reject-vs-support.** The two fall-through shapes
  needed OPPOSITE answers, and only the plain-assignment control told us which.
- **A guard is only as good as the shapes that can reach it.** Parity with the plain spelling
  was achieved for six source shapes and missed for two
  ([[as-cast-unbound-pointer-shape-generic-message]]) purely because a GEP-derived source has
  no storage key to look up. Provenance recorded AT the boxing site would not have had that
  failure mode.

## Adjacent - found during reviews, not bugs in the feature being reviewed

[[constructor-discriminator-inconsistent-name-only-sites]],
[[array-view-params-unconditionally-noalias]],
[[expect-error-leaves-outer-nullcond-block-unterminated]],
[[generic-function-call-diagnostics-are-misleading]].

## Working notes

The portable lessons from these rounds - reviews, sequencing, guard polarity, agent reports,
tests - now live in [`internal/fix-issue-lessons.md`](../fix-issue-lessons.md). They were moved
out of this file because they outlive every issue in it.

## Landed design records

Nothing here is open. These accounts are kept because they explain WHY the shipped code has the
shape it does, they record approaches that were tried and **must not be retried**, and they hold
the **ratified behaviour changes** - deliberate changes to what already-compiling programs do,
which a future session must not "fix" back without reopening the decision.

| Work | Commit |
|---|---|
| `as` / `is` routing, named args on the interface path | 2026-07-28 session |
| `as` boxing ownership guards; primitive-array boxing; `?:` join return; duplicate ctor; thin `function<>` param | 2026-07-29 session |
| Return dangle laundered through an intermediate local (attempt 4) | `2bcc5a0` |
| Generic-interface registration | `09f1d56` |
| Generic namespace key space, layer 1 (template base) | `15809e0` |
| Generic namespace key space, layers 2-4 (arguments, body, functions) + LSP `expect_error` fix | `e2a23d5` |
| Stack address into a `unique` location rejected | `99d73f3` |
| `function<T>[N]` losing its array size | `696060d` |
| Non-heap (stack/global) addresses rejected at call sites, not just store sites | `8c29ca7` |
| Generic-substituted `unique` field ownership seen by the field-store gates | `d65f000` |
| `function<T>` split from `function<T>*`; `Lambda<T>*` rejected | `4000fa1` |
| Closure widening gated on the direct call path; interface argument slots type-gated | `4097959` |
| Thin `function<>` argument provenance gated; function-pointer SIGNATURE participates in binding | fix/funcptr-arg-accept-set (branch, not yet merged) |
| Fixed-array shape: `auto` deduces a view, `T[N] b = a` is a copy | fix/array-shape |
| `using` interface alias resolved as an `is`/`as` target (`GenerateIsCheck`/`GenerateSafeCast`); residual asymmetry filed as [[interface-lookup-alias-asymmetry-latent]] | fix/iface-alias (branch, not yet merged) |
| Interface boxing keyed on the VALUE, not the source binding; `??` join boxed per arm | fix/iface-boxing (branch, not yet merged) |
| `if const` leaf emission gated on insert-block LIVENESS, not non-nullness | fix/ifconst-ir (branch, not yet merged) |
| `IsUniqueFieldRead` (SOURCE gate) re-keyed onto `IsOwningUniquePointerField`, closing the fully-generic field-to-field leg `d65f000` deliberately left open | fix/unique-f2f (branch, not yet merged) |
| `delete` of an array view rejected when the view was bound from fixed-array storage; provenance tracked DECLARATION-ONLY | `11e85a1` |
| Provably-null interface access rejected at COMPILE TIME; no runtime guard (RATIFIED) | `3311842` |
| Temp-source and fat-interface `unique` field stores rejected; the implied-move pointer guard in all THREE copies | `3d33bfe` |
| Brace-list initializer on a global (or bare-brace local) struct/union/class/container rejected instead of silently discarded (RATIFIED) | fix/global-positional (branch, not yet merged) |
| Brace initializer on a POINTER target rejected at FOUR of the five `EmitFieldInitializer` call sites; the named-argument site audited and left alone because it is CORRECT (RATIFIED) | fix/ptr-fieldinit (branch, not yet merged) |
| Empty `{}` split by TARGET TYPE - seeds a non-pointer in both spellings, REJECTED on a pointer in every spelling as AMBIGUOUS between null and a pointer-to-empty. Supersedes `fix/ptr-fieldinit`'s `S*[N] a = {}` zero-init row (RATIFIED) | fix/emptybrace (branch, not yet merged) |
| Overload identity canonicalized (`f(int)`/`f(i32)` are ONE overload); duplicate definitions diagnosed; function-pointer POINTER DEPTH carried; NAMED functions proved at the argument and declaration sites (RATIFIED) | 2026-08-02, uncommitted |
| A pure-rename `using` alias folded at MONOMORPHIZATION, so `list<MyInt>` and `list<int>` are ONE instantiation; the alias set pre-registered ahead of BOTH passes (RATIFIED) | fix/alias-mangling, uncommitted |

Suite trajectory across the whole sequence: 522 -> 530 -> 536 -> 538 -> 540.

### 2026-08-02 - one TYPE IDENTITY for overloads, and the four sites a funcptr signature is read from

Follows Stage -1 of `internal/plan/funcptr-type-mangling.md`, which folded `int`->`i32` at
MONOMORPHIZATION. That left the compiler holding two different answers to "are these the same
type?", and this round made them one. Four fixes, measured before and after:

| Repro | Before | After |
|---|---|---|
| `f(int)` and `f(i32)` overloads | both compiled; `f(1)`=201 but an `i32` variable reached the `int` body | one overload, then a redefinition error |
| `int f(int)` written TWICE | compiled, ran the first body | redefinition error |
| `function<void(int**)>` into a `void(int*)` slot | **SIGSEGV** | rejected, naming `void(int**)` vs `void(int*)` |
| `slot(fDbl)`, a NAMED `double(double)` into an `int(int)` slot | **101** - wrong body, exit 0 | 202, the right slot |
| `function<int(int)> g = dd;` with `double dd(double)` | `s2=5`, wrong body | rejected |
| `void(C2*)` bound to a `void(S2*)` slot | out-of-bounds read, exit 0 | rejected |

**RATIFIED: `int` and `i32` are ONE overload, so a program declaring both stops compiling.** That
is the point - before, both were emitted and argument selection between them was scrambled. The
canon lives in ONE place (`CanonicalPrimitiveSpelling`, moved to `LLVMBackend.h`) and both type
identities funnel through it: `MangleTypeArg` for instantiation, `ToUniqueString` for overload
symbols. Symbol names moved; nothing else did.

**The plan predicted "a canonical name makes it a redefinition error". That was wrong** - there
was no duplicate detection at all. `CreateFunctionDefinition` had *deliberately* skipped a second
body with only a `[verbose]` line since long before any of this. A canonical name only routes the
two spellings to that same silent skip; the diagnostic is a separate mechanism.

**The duplicate diagnostic discriminates on the source LINE, and deliberately not on the file.**
`currentSourceFilePath_` is not stable across the LSP re-analysis path: a bulk sweep attached
`cocoa.cb`'s `setMenuHandler` twice under two different path values, and comparing files reported
the single definition as a redefinition of itself. Two different files holding the same overload at
the same line therefore go unreported - silence is the safe direction, being what the compiler did
before. Line 0 (compiler-generated: lambdas, trampolines, thread shims) never reports.

**Pointer depth had to land in the SAME change as the duplicate diagnostic, not after it.** They
look independent and are not: without depth, `f(function<void(int*)>)` and `f(function<void(int**)>)`
mangle to one symbol, so the new diagnostic reported two GENUINE overloads as a redefinition. A
fix that is safe alone can be unsafe in either order - check what a new diagnostic makes newly
reachable.

**`PointerDepth == 0` means NOT RECORDED, never "not a pointer".** Only the source-parse sites can
count `Star()`; C interop, WinRT and synthesized signatures leave it 0 and must keep binding. Same
one-sidedness as `Known`.

**The four sites.** A function-pointer signature is read from four different places, and closing
one proves nothing about the others:

1. `ArgumentProvablyMismatchesParameter` and 2. `ComputeOverloadFunction` - argument paths, reading
   `arg.TypeAndValue`.
3. `GetFunctionForFuncPtr` - the declaration/assignment site, reading the destination.
4. `ToUniqueString` - the overload NAME.

The issue file had asserted "do not re-close it on the strength of an ARGUMENT repro, those are all
fixed." **False.** Every recorded argument repro used a `function<>` VALUE; a bare function NAME
carries no signature on its own `TypeAndValue`, so sites 1 and 2 saw an empty
`FuncPtrReturnTypeName`, returned "no proof", and skipped the comparison entirely. Do not trust a
"this path is closed" claim that names a path rather than a site.

**RATIFIED, and the near-miss worth remembering: the bind site uses a DIFFERENT arity rule from the
argument sites.** A callback taking FEWER parameters than the slot supplies binds legally at a
declaration - the synthesized `onStdout` field is `void(char*, int)` while `doc/LANGUAGE.md:2552`
documents the one-parameter form, and `Test/test_program.cb` binds a `void(char*)` to it (under
cdecl the caller cleans up, so the unread argument is ignored). The first attempt reused the
argument path's strict-arity rule and broke that test. The mirror direction - a callee reading a
parameter the slot never supplies - stays proof, as do component types across the shared prefix.
`DescribeFuncPtrBindMismatch` IS the rule: the candidate filter asks it rather than duplicating it,
so the verdict and the message cannot drift apart. **Do not unify the two arity rules.**

Residual 3 (`neigh=2333`, the namespace-intersecting overrun) re-measured unchanged, confirming
none of this disturbed that axis. Suite 576 / 0 / 8, examples 35 / 0, LSP 152 / 0.

### `3d33bfe` - temp-source `unique` field stores, and the implied-move guard in THREE copies

Closes `unique-field-to-field-residue-temp-and-interface-source` and
`generic-unique-field-temp-source-crashes-compiler`. Does NOT close
`interface-field-self-assign-false-positive` - see below.

**The provenance the issue file said was missing already existed.** `MovableTempField` and
`FromOwningTempField` already rode the exact temp spellings in question; the only thing blocking a
diagnostic was the `IsOwningValueType(TypeName)` gate on those branches, false for a
destructor-less pointee. The fix adds sibling legs keyed on the owning-unique-pointer-field shape.
Take this as the standing lesson: **probe whether the signal exists before designing one.**

**The implied-move pointer guard lives in THREE copies, and two rounds fixed only N-1 of them.**
`!TypeAndValue.Pointer` is required at the assignment path (`~12397`), the
declaration-with-initializer path (`~9197`), AND the RETURN path (`~6432`). Each unguarded copy is
a zero-output compiler SIGSEGV (`ConstantAggregateZero::get` on a pointer type, detonating in
`DAGCombiner::visitBUILD_VECTOR` during Mach-O emission - the crash handler cannot see it because a
SIGSEGV in LLVM native code is not a hooked signal). Round 1 fixed one, round 2 found the second,
round 3 found the third. The completeness proof that ended it: **all 19 `ConstantAggregateZero::get`
sites and all 12 `MovableTempField` readers were enumerated - exactly three intersect.** The rest
are gated by `ClassifyOwningAssignSource(...) == AssignSourceKind::Move` under `isStructTy()` and
are off the pointer/temp-field path. Do not re-derive this by sampling; enumerate.

**`interface-field-self-assign-false-positive` was ATTEMPTED AND REVERTED.** A `differentFieldReceivers`
test concluded two receivers were different from variable-NAME inequality, which false-rejected
`alias ISlot ib = ia; ib.slot = ia.slot;` - a program master runs correctly, and `alias` is the same
object by definition. Three discriminators were then shown unusable, and this is the part worth
keeping: **names** fail both witnesses; the **interface locals' storage** fails
`ISlot ia = a; ISlot ib = a;` (two distinct boxes over one object); and a bare **LLVM `Value` compare
of the field address** fails even the TRUE self-assign `ia.slot = ia.slot`, because each access
re-loads the fat pointer and yields two distinct `LoadInst`s - inverting the bug into a false
rejection of the one shape master gets right. A sound test needs real dataflow through the box to
the underlying data pointer. Do not retry any of the three.

**Three diagnostics in this family recommended remedies that do not work**, all caught by review,
none by the suite. A temp message named `move makeBox.t` (invalid syntax, and `move` on a call
result is itself rejected); a written `unique IShape` brace-init named `move a.s` and then emitted
the IDENTICAL message when the user wrote it - a closed loop, about a fat interface it called a
"pointer"; and the container-element half claimed a destructor frees at end-of-statement when the
LIST owns the pointee, with the named remedy aborting 134. **A diagnostic's stated mechanism is a
factual claim - run the remedy before shipping it.** The residual misroute at chained depth >= 2 is
filed as [[owning-temp-parent-misroutes-chained-alias-access]].

One correction worth recording because it inverted an instruction: splitting the two-owner message
on `MovableTempField` does NOT work, because that flag is additionally gated on `IsOwningValueType`
and so is false for a dtor-less pointee - the headline `makeBox().t` shape would have taken the
container branch. The discriminator is `parentOwnsTemp` (parent is an owning temp, not an `alias`
return), now recorded as `NamedVariable::OwningTempParent`.

### fix/null-iface - null interface access rejected at COMPILE TIME, and the runtime guard REJECTED

Closes `interface-method-call-on-null-value-segfaults`. `IFace lv = default; lv.Get();` compiled
clean and SIGSEGVed (139): the zero-initialised `{vtable, data}` fat pointer was dispatched through
with no guard. Fires on a PLAIN non-generic interface; generic interfaces inherit it.

**RATIFIED BY THE MAINTAINER 2026-08-01, and this is the part that must not be re-litigated:
reject at compile time as far as is provable, and there is NO per-dispatch runtime guard.** `?.`
is the language's answer for what the compiler cannot prove - a user who does not statically know
whether an interface value is live writes `lv?.Get()`, which already lowers to a guarded dispatch.
Paying a branch on every dispatch to re-detect what `?.` exists to express was considered and
REJECTED. **Do not re-propose a runtime null-vtable check, in debug builds or otherwise.** The
issue file that carried this decision is deleted; this record is where it now lives.

The no-guard constraint was PROVED, not asserted: `--out-lli` on four programs including the
4,300-line `Test/test_interface.cb` is byte-identical between the pre- and post-fix binaries except
the module-ID hash. Zero codegen change.

**What is rejected** requires all three: the receiver is a named LOCAL's own frame slot accessed
with a plain `.`; the slot's address provably never leaves the frame; and the last write before the
access, IN THE ACCESS'S OWN BASIC BLOCK, is a whole-slot store of a null constant. A basic block has
one entry and no internal branch, so that write is exactly what the access reads. The rule reads
EMITTED IR rather than enumerating assignment sites, so an unrecognized shape degrades to "no
diagnostic" and never to a false rejection - that polarity is the whole design.

**Deliberately ACCEPTED and NOT residue**: `?.`, branch-assigned, loop-assigned, call-separated on
another path, parameters, and folded `if const` arms. A conditionally-null value on which the user
wrote a plain `.` remains a runtime SIGSEGV BY DESIGN. `IFace lv = default;` must also stay legal to
DECLARE - `Test/test_interface.cb` and the `struct H { IFace c = default; }` field shape depend on a
default-initialised slot assigned later. Only the ACCESS on a still-null value is the error.

**The field (property) axis is closed too, and its gate differs from the method axis.** On the
method path the anchor (the vtable load) and the read are two adjacent instructions; on the field
path they are the SAME instruction, so "the access's own block" is *defined as* the block the read
lives in and cannot drift. That holds only while the fat value is a FRESH load - hence the
`interfaceVar.Primary == nullptr` gate. When `Primary` is already set the value may have been loaded
in an EARLIER block, and anchoring in the current block would inspect stores that happened AFTER the
read: a null store following an earlier non-null load would be a FALSE REJECTION. **Anyone widening
this must carry `Primary`'s defining `LoadInst` and require `load->getParent() == access->getParent()`,
not widen the anchor.** The parenthesized spelling `(lv).tag` reaches `Primary != nullptr` and is
therefore undiagnosed while `(lv).Get()` is caught - not fixed here; the paren spelling was
subsequently CLOSED by `78c678b`, and the remaining residue lives in
[`internal/plan/null-interface-access-widening.md`](../plan/null-interface-access-widening.md),
which restates the constraint in this paragraph as the rule any widening must obey.

Two review lessons worth carrying: a claim that each reject leg lived in its own file was FALSE, and
it mattered - the shared file exits at the first failed expectation on the pre-fix binary, so later
legs were not self-proving. Legs are now split by RECORD SITE, the unit that regresses independently.
And an accept leg that assigned the field BEFORE the call could not have tripped any widening onto
field receivers; the shape that would trip it SIGSEGVs on every binary, so it cannot exist as a
runnable leg at all - that is now stated in the test file rather than papered over.

### `11e85a1` - the array-view delete guard, and why it tracks the DECLARATION ONLY

Closes `delete-of-array-view-over-stack-storage`: `int[3] a; int[] v = a; delete[_] v;` compiled
clean and aborted (134) with `free()` handed a stack address. A `T[]` view is a thin `T*`, so the
delete path could not tell a view over `new T[n]` from a view over a decayed fixed array. The
distinction exists at the BIND site (`ConstArraySize > 0 && !IsArrayView`) and is now carried onto
the local as `NamedVariable::ViewOfFixedArrayStorage`.

**The ratified shape, arrived at by fixing a false rejection in review: the flag is set at the
DECLARATION site only, and ANY later plain `=` to that local clears it permanently.** The first
attempt RECOMPUTED the flag on each reassignment, which is walk-order rather than flow-sensitive -
so a `v = <fixed array>` inside any branch poisoned every later `delete` in the function. That
rejected this program, which master compiles and runs correctly:

```cflat
int[3] a;
int[] v = new int[4];
if (argc > 100) { v = a; v[0] = 1; return 0; }   // stack path returns, never deletes
v[0] = 2;
delete[_] v;                                      // reachable only holding the heap alloc
```

**Do not re-propose recomputing on reassignment, and do not attempt full flow-sensitivity here.**
A reassigned local has an origin that is not provable at the delete site, so it must ACCEPT. The
cost is one accepted true-positive (`int[] v = a; ... v = new int[4]` in the other order still
aborts); that is recorded as the must-keep-compiling leg `reassignHeapToStackNotDiagnosed`, not as
a bug to close.

Why the narrowing is SAFE rather than merely convenient: **`int[]*` is not a legal type.** Both
`int[]* p = &v;` and an `int[]*` out-parameter are rejected by a pre-existing gate, so a view local
cannot be rebound through an alias, an out-parameter, or any address-taking route. A plain `=` is
the ONLY rebind path, so clearing on `=` cannot be bypassed. Twelve assignment spellings were
probed against both binaries to confirm it.

Deliberately left ACCEPTED as unprovable: view parameters, struct fields, call results, conditional
joins, view-of-view rebinds, and the cast spelling `delete[_] (int*)v;`. The comment at the guard
says so plainly - an earlier draft claimed the cast case "already picked its own diagnostic path",
which was FALSE and is the kind of justification that must be verified after the change it justifies.

**Two message defects were found by review and both are the same class**: a diagnostic that names
something absent from the user's source. The first printed the literal fallback `"a fixed-array
local"` for a GLOBAL source; the second read the name off the `llvm::GlobalVariable`, which carries
LLVM's `.N` uniquifying suffix whenever the name collides with a symbol the runtime already declared
- so an ordinary `int[3] read;` reported `'read.1'` (also reproduced with `write`, `stat`, `strlen`,
`printf`). The fix takes the identifier text from the AST instead. **Do not recover a user-facing
name from a lowered LLVM artifact**; this is the same failure family as recovering a declaring scope
from a mangled key, which shipped two silent wrong values.

### fix/funcptr-arg-accept-set - the thin `function<>` accept set, and signature-aware binding

Two P1s, deliberately fixed in DIFFERENT LAYERS. Collapsing them into one check was the trap.

**The thin data-pointer hole is a LOWERING bug, not a scorer bug.** `ce9858e` closed the FAT
(`Lambda<>`) parameter arm through one provenance gate; the THIN arm never widens to a
`{code, env}` struct, so it never reached that gate and the argument was bitcast straight into a
bare code slot and CALLED (exit 139). The fix is `CheckThinFnPtrArgProvenance`, a thin-flavoured
sibling of `WidenToClosureFatChecked` sharing the SAME predicate
(`ArgumentIsProvablyDataPointer`), applied at both thin lowering sites - the "extern
C-compatible" else-branch of the funcptr arm in `CreateOverloadedFunctionCall`, which also
serves internal thin params, and the thin case in `LowerByValueArg` (virtual dispatch), where
thin+pointer previously fell through BOTH guards. The scorer's `arg.BaseType->isPointerTy()`
clause STAYS PERMISSIVE on purpose: a legal thin value is an indistinguishable pointer at scoring
time, and `TypeAndValue.IsFunctionPointer` is measurably FALSE for every legal source of one.
Putting the rejection in the scorer would recreate the direct-vs-virtual accept-set divergence.

**The issue file's root cause for the SIGNATURE bug was incomplete, and the correction is the
whole fix.** It said `ComputeOverloadFunction` "compares function-pointer arguments by SHAPE
only". Measured with an instrumented build: the scorer never sees the argument's signature at
all. Call-site argument assembly in `MainListener.h` rebuilds `argVar` field by field and
propagated the function-pointer type ONLY for a lambda literal, so a stored `function<>` variable
arrived with `IsFunctionPointer` false and an empty `FuncPtrReturnTypeName`. The enabling change
is propagating the three signature fields (`FuncPtrReturnTypeName`, `FuncPtrReturnPointer`,
`FuncPtrParams`) in BOTH argument loops - and ONLY those three. `TypeName` and
`IsFunctionPointer` are deliberately left alone in the direct loop: setting `TypeName` would move
the argument out of the scorer's LLVM-type branch into its named-type branch and change
resolution for unrelated calls.

**The comparison is TYPE-level, never SPELLING-level - and the first cut got this wrong.**
`ToUniqueString` encodes the thin/fat flavour (`cfuncptr` vs `funcptr`), so naive encoded-name
equality would have rejected the legal thin -> fat widening that `WidenThinToFat` exists to
support. That much was caught before landing. What was NOT, and what review round 1 caught as a
confirmed P1 false-rejection regression, is that resolving aliases and enums is **necessary but
nowhere near sufficient**: `int`/`i32`, `long`/`i64`, `u32`/`i32` and any two pointer types are
ONE type spelled two ways - the scorer says so itself 40 lines below (`int==i32`), and
`IsKnownTypeName` puts them in one scalar set. A name-equality comparison hard-errored on six
programs master runs correctly, on BOTH proof sites and BOTH paths, including the ordinary
single-candidate call. In-repo code was green only because no `.cb` happens to cross a spelling
boundary at a `function<>` argument - the "name-only discriminator, sweep first" trap, and a
differential sweep over existing corpus files structurally cannot see it.

The shipped comparison therefore reduces each component to a coarse TYPE CLASS
(`FuncPtrTypeClass`): `i` any integer (any width or signedness, plus bool/char and an enum via
its backing type), `f` floating point, `p` any pointer, `v` void, and 0 = unknown for a struct,
an interface or an unsubstituted generic. `FuncPtrSignatureOf` returns FALSE if any component is
unknown. Classes are coarse ON PURPOSE: a width or signedness difference lowers to the same
register class, so it is left binding exactly as before. What remains provable is a component
whose CLASS differs (the filed repro, `double` where `int` is expected) or an arity difference -
never a spelling.

**Signatures are compared only at EQUAL indirection shape.** A shape disagreement is the separate
open [[shape-mismatched-funcptr-arg-binds-silently]] and must stay score-1 bindable, or the
`pickFnPtrShape` / `pickFnPtrShapeRev` legs in `Test/test_function_ptr.cb` (which pin the shape
ranking from `4000fa1`) break.

**Parity forced a SECOND consumer.** The scorer fix alone left the interface path unfixed: a
lone same-arity slot never reaches `ComputeOverloadFunction`, it takes the lone-slot arm in
`ResolveInterfaceMethodSlot`. The same PROOF ("both signatures reduce to known type classes,
shapes agree, TYPE CLASSES differ") was therefore also added to
`ArgumentProvablyMismatchesParameter`, ahead of its closure-evidence early-out - which a genuine
function pointer of the wrong signature would otherwise satisfy.

Ratified behaviour changes. A data pointer into a thin `function<>` parameter is now a COMPILE
ERROR on both the direct and the virtual path; it previously compiled to a segfault.

A function pointer whose signature differs from the parameter's in a component's TYPE CLASS (or
in arity) at the same indirection shape is now a compile error too - **but only when no
same-arity sibling can take the call.** The rejection lives in the overload SCORER, where
`result = -1` means "this candidate does not match", a preference verdict and not a validation
one. With a sibling that absorbs a pointer (`int lam(void* p)`, a variadic `int lam(char* fmt,
...)`, or the same pair as interface slots) the call silently REBINDS to the sibling instead:
master printed `b=5`, calling the callee at the wrong type; this prints `b=999`. Neither answer
is right, and the rebind is pre-existing behaviour of the pointer-permissive scorer rather than
something this change introduced - what changed is only that the funcptr candidate stopped being
viable. It is pinned by the `rebindProbe` legs in `Test/test_function_ptr.cb` and recorded in the
narrowed issue file; do not read the ratified line as "a mismatched signature always errors".

RESIDUAL, and the reason the second issue file was rewritten rather than deleted: the type
classes are coarse ON PURPOSE, so a mismatch WITHIN a class is still invisible - floating-point
WIDTH (`float` into a `double` slot, the same SHAPE as the closed repro), integer width and
signedness, POINTEE type (memory-unsafe: reads past the end of the object), and aggregates. All
silent, all identical on master, none of them regressions. See
the `fix/funcptr-sig` and `fix/funcptr-close` records below, which absorbed that issue file's
repros and constraints when it was closed and deleted on 2026-08-03 - including the explicit
instruction NOT to retry a widened SPELLING comparison.

Left alone by that pass and fixed separately since: a `function<>` returned BY VALUE from a call
into a fat `Lambda<>` parameter yielded garbage on the DIRECT path. See the
"fix/funcptr-callresult" record below - the root was the `CallerName` re-resolve firing on a
value that is not a named function.

### fix/funcptr-callresult - the `CallerName` re-resolve only fires on a NAMED FUNCTION

Closed `funcptr-call-result-into-closure-param-garbage` (file deleted). The filed lead was
RIGHT about the mechanism and WRONG about the value: the printed number is a function address,
not uninitialized memory, and the substituted code slot is a SHIM over the producer rather than
its bare address.

The mechanism. `CreateOverloadedFunctionCall`'s fat-closure argument arm re-resolves the
argument by `arg.CallerName` to skip method overloads sharing a key and to pick the `move`-flag
match. A CALL RESULT keeps the CALLEE's name in `CallerName` (the postfix walker sets it when
the primary resolves to a function, then clears `Storage` and overwrites `TypeAndValue` but not
`CallerName`), so `GetFunctionForFuncPtr("make", 1, ...)` ran on a value that is not a function
at all. Its single-overload early return ignores `expectedParamCount`, so `val` became the
`llvm::Function` for `make`; `WidenBareOrThinToClosureFat` then saw an `llvm::Function` and
built `{__shim__make, null}` instead of `insertvalue`-ing the returned pointer. `f(5)` called
`make(5)`, which returned `&dbl`, printed as an int.

The fix is one condition: the re-resolve requires `llvm::isa<llvm::Function>(val)`. It is
narrower than either option the issue file proposed, and it mirrors the guard `MainListener.h`
already applies at its own two re-resolve sites (`:9339`, `:11876`) - the argument arm was the
one place the pattern was missing. Clearing `CallerName` on call results was NOT taken -
`CallerName` also feeds `ScoreMoveAgreement`'s rvalue test, the bond-source ledger, and move
tracking, and blanking it changes all of them. Touching `GetFunctionForFuncPtr`'s early return
was not taken either, for the reason the file gives.

What the axis sweep settled, and why a narrower test would have shipped half a fix:

- The defect is DIRECT-call + FAT (`Lambda<>`) slot only. The interface path is correct because
  `LowerByValueArg` has no re-resolve, and the THIN (`function<>`) slot takes the extern-C arm,
  which has none either. Both are parity legs, not the bug.
- Producer kind is irrelevant EXCEPT for a method receiver: free function, `static`, a
  namespaced callee, a `using Cb = function<int(int)>` return, and an arity-overloaded producer
  all failed identically. `d.lam(m.makeMethod())` was already correct only by accident -
  `CallerName` there is the RECEIVER `m`, which resolves to nothing in the function table.
  Do not read that leg as evidence the method path is structurally safe.
- Taker shape is irrelevant too: a fat parameter at argument index 1, a method on a GENERIC
  class, and a namespaced free function all failed. Testing only "fat parameter at index 0 of a
  plain method" would have proved much less than it looks.
- TWO MORE SILENT WRONG VALUES fall out of the same fix, neither of them in the issue file. A
  local `function<>` whose NAME collides with a global function called the GLOBAL and discarded
  the local's value (500 instead of 15). And a producer with a SAME-ARITY sibling of its own
  name returned 1005 - a plausible number, not a stray address, which makes it the worst shape
  in this family: nothing about the output says "miscompile".

Coverage, and the trap in writing it. The first version of the regression test asserted that a
named argument "still re-resolves" using a function declared exactly ONCE. That is vacuous:
`GetFunctionForFuncPtr`'s single-overload early return answers first and the multi-candidate arm
is never entered - a build with the re-resolve DELETED OUTRIGHT ran the whole suite green. The
discriminator both re-resolve legs need is TWO OVERLOADS OF THE NAME, which is what escapes that
early return; what separates the survivors then differs per leg, and the two are not the same
test. The ARITY leg has two candidates ENTER and exactly ONE survive the count filter (the
wrong-arity arm is declared first, so a count-blind pick is visibly wrong). The `move` leg has
both candidates survive the count filter - same name, same arity - so `moveFlagsMatch` is the
only thing that can separate them. Do not copy "two candidates survive the arity filter" onto a
new leg; it is true of the `move` leg only. The
`move` REJECTION had no coverage anywhere in the repo and is now the last block of
`Test/errors/err_data_pointer_to_closure_param.cb` - the existing closure-PARAMETER
argument-binding family, same arm and same call path. It lives behind this same guard, so
without that block deleting the lookup would drop a diagnostic silently. It is NOT the
diagnostic `err_funcptr_move_mismatch.cb` pins: that one is raised on the ASSIGNMENT path and
reads "'move' modifiers of function pointer signature".

Found while sweeping, NOT part of this defect and filed separately - a generic function
returning `function<>` poisons the ENCLOSING function's return coercion. See
[[generic-funcptr-return-poisons-enclosing-return]]. It is why the regression test has no
generic-producer leg.

Still open in this family and NOT addressed here: the `move` flags are only checked when the
argument is a bare NAME, so a `function<int(MP*)>` VARIABLE binds to a
`Lambda<int(move MP*)>` parameter with no diagnostic. Same before and after this fix; it belongs
with [[shape-mismatched-funcptr-arg-binds-silently]].

### fix/ifconst-ir - `if const` leaf emission gated on insert-block LIVENESS

The mechanism, because the symptom points at the wrong file entirely. `EmitAndFoldIfConstLeaf`
(`MainListener.h:7891`) chose "emit into the current block" whenever
`builder->GetInsertBlock() != nullptr`, on the assumption that a null insert block is what
declaration scope looks like. It is not: nothing clears the insert point when a function
definition finishes, so at file / member / interface scope the builder still points at the LAST,
already-terminated block of the previously emitted function. A condition that FOLDS without
emitting (a literal, `sizeof`, `__MACOS__`) never noticed. A condition that must EMIT - a const
global load, an enum member - had its `load` appended after that block's `ret`:

```
ifResume:                 ; in _Test_int_charPtrstringstring_, from Test/test_helper.cb
  call void @printf(...)
  ret i32 0
  %11 = load i32, ptr @GI_NEVER, align 4     <- leaked here from a file-scope `if const`
```

The verifier reports this as "Basic Block ... does not have terminator!" in a function the user
never wrote near the `if const`.

The fix is one predicate: `compiler->IsInsertBlockLive()` (non-null AND unterminated) instead of
non-null. That routes the leaf into the existing throwaway `__if_const_eval_tmp` function - the
path that already existed for the null case and for the owning-sink `forceScratch` scan. It is
one change at the single leaf site rather than a `forceScratch=true` at the file-scope call site,
because all four `DecideIfConstCondition` callers share the hazard: file, member (aggregate) and
interface scope all reproduced on master and are all fixed by the one predicate. Statement scope
is unaffected - a live body block still takes the direct path, so normal `if const` emission is
byte-identical.

Not changed, deliberately: nothing rejects anything. `if const (<const global>)` is legal code and
now works in both polarities. The insert point is still not cleared at end-of-function, and the
other ~30 `GetInsertBlock()` null-checks in `LLVMBackend.h` were not audited for the same
non-null-vs-live confusion. That deferral is filed as
[[insert-block-liveness-not-audited-repo-wide]], which counts the set properly (49 uses, 3
null-compares, exactly one - `LLVMBackend.h:12488` - on the shared builder, and its comment carries
the same false premise), records WHY that one is unreachable today by construction, and says why
the tempting one-line "clear the insert point at end-of-function" fix is the wrong first move.

A bonus the issue did not ask for: at member and interface scope master loses the diagnostic
entirely - a non-constant `if const` condition there produces a raw "module verification failed"
dump. On this branch the same input correctly reports `'if const' condition must be a compile-time
constant expression`, because the leaf now folds in a scratch function instead of corrupting the
module before the decision is reported.

Verification: `./test.sh Release` 554/0/8; `./example_mac.sh Release` 35/0; a differential corpus
sweep over all 512 `.cb` under `Test/`, `example/` and `cflat/core/` with real `-o` codegen plus
program stdout, isolated `HOME` per side, found 8 differing files, all 8 proven nondeterministic
by running the MASTER binary against itself twice (thread-pool throughputs, `rdtsc` deltas,
`CFAbsoluteTimeGetCurrent`, pid). Zero behavioural differences.

### `99d73f3` and `696060d` - the two P1s of 2026-07-31

Both landed via the `fix-issue` skill (worktree -> fix agent -> opus review loop -> `--ff-only`).
Recorded together because the SAME lesson produced both: **every defect that mattered was found
by an adversarial review, never by the fix agent, and never by a green suite.** Nine new issues
were filed out of these two fixes.

**`99d73f3` - stack address into a `unique` location.** Root cause was NOT a compiler bug in the
usual sense: compilation succeeded, and the SYNTHESIZED DESTRUCTOR called `free()` on a stack
alloca, so libmalloc aborted before any diagnostic site was reached - hence total silence at exit
134. The existing `unique` checks all keyed off borrow-provenance flags, and a bare `&local` sets
none of them.

Now rejected across six spellings: plain field, generic-substituted field, local, field-array
element, local-array element, and a bare self-field inside a method.

- **`IsProvableStackAddress` is one-sided BY CONSTRUCTION** - true only when the value, after
  `stripPointerCasts` and a GEP-base walk, `isa<AllocaInst>`. An independent probe of `?:`, `??`
  (which lowers to a LOAD FROM AN ALLOCA here - the classic false-positive trap), local
  round-trips, function returns, struct fields, `list<unique Item*>` and `Box<alias Item*>` found
  ZERO false rejections. **Do not widen it to catch more cases.**
- **The array-element leg is keyed on GEP SHAPE, not on a flag - deliberately.**
  `ElementOwningUnique` looks like the natural key and is WRONG: it is written in exactly one
  place, gated on `IsUniqueTypeArg`, which a written `unique T* f[N]` never sets. Keying off it
  matches nothing. The subscript path zeroes `ConstArraySize` and emits a two-index GEP whose
  source element type is the ARRAY type, which is what the leg tests.
- **RATIFIED:** `unique` on a function-pointer or closure FIELD is rejected
  (`'unique' on field ...: a function pointer or closure does not own an allocation`). Added in
  `696060d`, not this commit - see below.

**`696060d` - `function<T>[N]` losing its array size.** The `functionPointerSpecifier` branch
broke out of the specifier loop without capturing a trailing `[N]`, and `GetType()` had an early
return for `IsFunctionPointer` that bypassed the `[N x T]` wrap. So a 2-element array got storage
for ONE element, and indexing past it was out-of-bounds UB that LLVM legally folded into
`unreachable` - silently discarding most of the enclosing function body while `--check` reported
PASS.

- **RATIFIED - `Pointer` is now set on the function-pointer parser branch.** This makes
  `function<T>*` behave identically to the already-working alias spelling `using Cb = ...; Cb*`.
  A scalar `function<T>*` out-parameter keeps working exactly as before, and a SUBSCRIPTED
  `function<T>*` parameter now works where master segfaulted. Verified by byte-identical IR
  against the previous compiler across every declaration position.
- **DO NOT RETRY: rejecting `function<T>*` wholesale.** Round 2 did exactly that, on the false
  premise that master's support was constant-folding coincidence. It is not - the out-parameter
  has genuine store-through IR. The rejection also removed a working capability and was trivially
  bypassed by a type alias. Round 3 replaced it with the `Pointer` fix above.
- **The fix was HALF DONE for two rounds.** The function-type-ALIAS branch (`using Cb = ...;
  Cb[3]`) had the identical `break`-without-dims bug, so the P1 stayed live under a second
  spelling while its issue file was already staged for deletion. Any fix in
  `ParseDeclarationSpecifiers` must be applied to the alias branch AND the direct branch, in BOTH
  copies - four sites, not two.
- **RATIFIED:** `Lambda<T>[]` (array-view of FAT closures) is rejected with
  `array-view '[]' is not supported on closure type ...; use a fixed size '...[N]' instead`. It
  previously leaked an LLVM verifier dump. Fixed-size `Lambda<T>[N]` arrays WORK and are a new
  capability - do not reject those.
- **A guard fix can open a hole elsewhere.** Setting `Pointer` made a `unique Lambda<T>*` field
  pass the `unique` shape guard, which then freed a CODE address (exit 138) where the previous
  compiler had cleanly rejected it. Caught only by the third review. When a widely-read type flag
  changes, audit every guard that reads it.

### `8c29ca7` - non-heap addresses rejected at call sites, not just store sites

Three silent-abort shapes (exit 134, no diagnostic) all stem from a stack or global address
reaching a location whose synthesized teardown will `free()` it - a value that was never
heap-allocated there, so the free is undefined. `99d73f3` had already closed the STORE sites;
this commit found the same hole at CALL-argument lowering and widened the underlying probe to
globals.

- A `move T*` parameter takes ownership and frees the pointee at scope exit, but the existing
  stack-address guard never ran at the call site, only at stores. The check is now applied to
  call arguments too, gated on `IsMove` (explicit `move`) or the same `uniqueAutoSink` condition
  (`IsUniqueTypeArg && !IsAlias && !IsBorrowOfUniqueElement`) used at function entry - this also
  catches a generic container's synthesized owning-sink parameter (e.g.
  `list<unique T*>::add(T value)`) with no `move` keyword at the call site, which is the only
  place such an argument's origin is still visible before it becomes an opaque SSA value inside
  the callee.
- **RATIFIED: a GLOBAL address is exactly as un-`free()`-able and exactly as provable as a stack
  address.** `IsProvableStackAddress` is renamed `IsProvableNonHeapAddress` (moved to
  `LLVMBackend` so both `LLVMBackend.h` and `MainListener.h` can share it) and widened to also
  accept a `GlobalVariable` base. The shared diagnostic wording changed to match, and
  `Test/errors/err_unique_stack_address.cb`'s pinned substrings were updated for it.
- Left unresolved: none of the three repros in the two source issue files - both issue files' fix
  directions were confirmed correct by this commit.

Regression legs added to `Test/errors/err_unique_stack_address.cb`: a global into a unique
field/local-array, a stack/global address into a `move` pointer parameter, and a stack address
into a `list<unique T*>` element via `add()`.

### `d65f000` - generic-substituted `unique` field ownership seen by the field-store gates

A `unique` field made owning by GENERIC SUBSTITUTION (`Box<unique Item*>::t`) is freed by the
synthesized destructor exactly like a written `unique` field, but the ownership rejects on the
field-store paths never saw it: substitution sets `IsUniqueTypeArg`, not `IsUnique` (the latter
is reserved for the written qualifier), and destructor synthesis is the only consumer that ORs
the two (`LLVMBackend.h` ~4614). Storing a second owner there made two owners of one pointer and
aborted at run time (exit 134) with no diagnostic at all, while the identical mistake in a plain
struct was diagnosed cleanly.

- **STANDING HAZARD, named explicitly by this commit: `IsUnique` (written qualifier) and
  `IsUniqueTypeArg` (generic substitution) are two separate flags, while destructor synthesis ORs
  them.** That split is the root of this bug and is worth treating as a recurring source of
  "diagnosed in a plain struct, silent in a generic instantiation" defects - any new ownership
  check added against `IsUnique` alone should be checked against `IsUniqueTypeArg` too.
- Fix: add `IsOwningUniquePointerField` and re-key the DESTINATION gate of the affected legs onto
  it, in BOTH field-store paths - `=` in `ParseAssignmentExpression` and brace-init via
  `EmitOneFieldInit` - closing Trap A (a borrowed parameter into a unique field, and its
  `?:`-laundered twin) and the MIXED field-to-field shape (a written `unique` SOURCE field into a
  generic DESTINATION field).
- The type-arg arm mirrors the `uniqueAutoSink` ownership rule (`LLVMBackend.h` ~3581) and is
  further narrowed to the exact scalar-pointer shape whose free is synthesized, so it rejects only
  a slot that is PROVABLY freed. Legitimate-borrow shapes are excluded by construction, not by an
  allowlist: an `alias` type argument and a borrow-of-unique-element never set `IsUniqueTypeArg`
  (`MainListener.h` ~3918), and a container's own `T* _data` buffer has it cleared by the
  explicit-star rule (~3974). A generic container's own setter (`void set(T v) { t = v; }`) stays
  accepted.
- **Deliberately NOT closed: the FULLY generic field-to-field copy** (`c.t = a.t` between two
  `Box<unique Item*>`) still compiles and double-frees. `IsUniqueFieldRead` independently requires
  `IsUnique` on the SOURCE, so that shape short-circuits there regardless of the destination gate;
  widening the source predicate has a broader blast radius than this destination-only change and
  is filed as its own issue.
- No new `TypeAndValue` field, so no `--init` cache change was needed - every flag the new
  predicate reads already round-trips in `LLVMBackend.cpp`.

Regression legs added to the two existing Trap A files, prefix-pinned to keep the
`Box__unique_Nodeptr` mangling out of the assertions: the borrowed-parameter and mixed
field-to-field shapes, in the `=` form in `err_unique_borrow_into_field.cb` and the brace form in
`err_unique_brace_init_borrow.cb`. Both files also gained legal heap-allocated and
`move`-transferred generic-unique-field inits, so the reject cannot widen unnoticed.

### fix/unique-f2f - the SOURCE gate re-keyed onto `IsOwningUniquePointerField`, closing what `d65f000` left open

`d65f000` deliberately left the FULLY generic field-to-field shape open (`c.t = a.t` between two
`Box<unique Item*>`): `IsUniqueFieldRead` required a written `unique` on the SOURCE, so that
shape short-circuited there regardless of how widely the destination gate had been widened. This
commit re-keys `IsUniqueFieldRead` onto the same `IsOwningUniquePointerField` predicate already
used for the destination - both field-store paths (`=` in `ParseAssignmentExpression` and
brace-init via `EmitOneFieldInit`) now apply the identical owning-slot test to BOTH sides.

- **Ratified behaviour change**: a generic-substituted `unique` field read out of a `move`
  PARAMETER (`void adopt(move Box<unique Item*> other) { t = other.t; }`, where only `other`, not
  the field, was moved) now REJECTS instead of silently double-freeing. `move other.t` is the
  correct spelling and was verified to compile, transfer, and exit 0. Do not "fix" this back to
  compiling - it was a silent double-free, not a false rejection.
- **What actually closed, confirmed by direct repro, wider than the headline shape**: a plain
  local (`c.t = a.t`), a pointer-to-struct source (`bp->t`), a by-value `Box<>` parameter, a
  fixed-array element, a nested field, a generic CLASS (not just a generic struct), a bare
  self-field read inside the owner's own method (`other.t = t`), a type-alias spelling, a chained
  assignment, a global source, and the `move`-parameter shape above. All of these share one
  property: the source read lands on a 2-index struct GEP off a named, alloca-backed local (the
  `IsUniqueFieldRead` shape test).
- **Do NOT retry - reflexively widening `IsUniqueFieldRead`'s GEP-shape test to close the
  residue below.** Two source shapes still miss the shape test and remain open, tracked in
  [[unique-field-to-field-residue-temp-and-interface-source]]: a temp/call-result source
  (`c.t = makeBox().t`) and a container-element source (`list.get(0).t`) - both reduce to the
  same root cause (no GEP to test). A THIRD shape, a fat-interface generic source
  (`Box<unique IShape>`), misses for a DIFFERENT reason (`tv.Pointer` is false on that shape, at
  two independent sites) and is also tracked there. Widening the GEP-shape test risks
  over-matching a borrow read through a cast (see the existing `IsUniqueFieldAlias` carve-out in
  that function) - each residue shape needs its own provenance signal, not a blanket loosening.
- A SEPARATE, pre-existing defect was found while probing this area and is explicitly NOT part of
  this change: [[interface-field-self-assign-false-positive]] - an interface-field-to-field copy
  with the SAME field name on both sides is misread as a self-assign (both receivers carry an
  empty `CallerName`), suppressing the new leg (and, in principle, four sibling traps that share
  the same `selfFieldAssign`/`selfUniqueFieldAssign` computation, though only this one is
  reachable for that shape today - see that file for the per-trap reachability check).
- No new `TypeAndValue` field, so no `--init` cache change was needed.

Regression legs added to `err_unique_borrow_into_field.cb`: the fully-generic field-to-field
shape via `=` and via brace-init (`EmitOneFieldInit`, the second, independently-gated field-store
path), plus a positive `move`-transfer leg between two generic fields. Both new `expect_error`
legs were confirmed to fail on the pre-fix binary for the right reason (not vacuously).

### `4000fa1` - separating `function<T>` from `function<T>*`; rejecting `Lambda<T>*`

Two related function-pointer/closure declarator bugs, both silent (no diagnostic) before this fix.

1. **`function<T>` and `function<T>*` were treated as the same overload, then SIGSEGV.** Three
   causes, all needed:
   - `ToUniqueString()` returned early for `IsFunctionPointer` and never folded in
     `Pointer`/`IsArrayView`, so both spellings produced the same mangled key and the second
     declaration was absorbed by the first before any duplicate check saw two signatures. The
     marker now rides the generated prefix (`cfuncptrPtr_`, `cfuncptrArr_`) rather than the tail,
     where it would collide with a trailing pointer parameter (`function<int(int*)>` vs
     `function<int(int)>*`).
   - `ComputeOverloadFunction`'s function-pointer fast path scored ANY function-compatible
     argument as a PERFECT match, ignoring indirection, so both candidates tied and the
     first-declared one won regardless of the call. **RATIFIED: the scorer now compares a
     three-state indirection SHAPE** - array (`function<T>[]` view or fixed `function<T>[N]`) /
     pointer (`function<T>*`) / plain value - via the new `FunctionPointerShapeOf`. A fixed array
     reaches a call site with every `TypeAndValue` shape flag cleared, so its array-ness is
     recovered from the storage behind it; a subscripted element has a GEP rather than the alloca
     and correctly scores a value.
   - Shape comparison alone was not enough: a non-function-pointer argument can demote every arm
     out of the perfect tier (an int literal is only a promotion match), and the promotion/implicit
     tier ignored per-argument quality entirely, picking by declaration position. So
     `pick(arr, 3)` bound a fixed array to the `function<T>*` arm and read the array as if it were
     the pointer, silently. That tier now prefers the arm with fewer function-pointer shape
     mismatches before falling back to the existing move-score/last-wins rule, which leaves every
     overload set WITHOUT such a mismatch resolving exactly as before.
2. **`Lambda<T>*` (a pointer to a fat closure) failed the LLVM module verifier** with "Invalid
   bitcast ... to %__closure_fat_ptr" and no source location. A fat closure is a by-value
   `{code,data}` struct, so a pointer to one was loaded as if the pointer were the struct. Every
   spelling was broken (local, parameter, `&arr[i]`). **RATIFIED: the declarator is now rejected up
   front** with a diagnostic pointing at the working alternatives, in both function-pointer
   branches of the MainListener copy of `ParseDeclarationSpecifiers`. The THIN `function<T>*` is a
   real machine pointer and is unaffected.
- **Measured scope of the tier change, and a correction to what master's prior behaviour actually
  was:** an overload set needs only ONE function-pointer parameter to accumulate a shape mismatch,
  so single-fn-ptr-arm sets can rebind too - verified `pick(function<int(int)>)` vs `pick(int*)`
  called with a `function<T>*` returns 100 on `8c29ca7` and 200 here. Master's choice there was
  itself a bind into a miscompile (the lone `function<T>` arm SIGBUSes on both builds, exit 138),
  so the new choice is at least well typed, not a behaviour change away from something correct. A
  whole-corpus A/B over 369 files in `Test/` and `example/` found no such overload set in the repo
  - the only IR deltas were two intended symbol renames.

Tests: `Test/test_function_ptr.cb` gains `testFuncPtrPointerOverload` (value/pointer arms in both
declaration orders, named-function binding, an out-param write seen through the pointer arm, and
the `[]`-vs-`*` pair in both declaration orders - every leg asserts a value a mis-bound arm cannot
produce). `Test/errors/err_lambda_array_view.cb` gains the `Lambda<T>*` rejection for both the
direct and the `using`-alias branch; its existing `unique` leg moves to the thin `function<T>*`
spelling, since the fat spelling is now rejected at the declarator before `ValidateUniqueField`
can run.

### `4097959` - closure widening gated on the direct call path; interface argument slots type-gated

Two coupled holes, fixed together because they share one accept set and letting the direct and
virtual arms diverge was already a regression once:

1. **A fat `Lambda<>` parameter on a DIRECT call accepted any data pointer and called it.**
   `CreateOverloadedFunctionCall` widened with `isPointerTy()`, which under opaque pointers is
   true for EVERY pointer, so a `void*` landed in the closure's CODE slot (SIGSEGV, no
   diagnostic). The virtual path was already guarded (`LowerByValueArg`). Both arms now route
   through one `WidenToClosureFatChecked`, so the two accept sets are identical by construction.
2. **`CallInterfaceMethod` did no argument type-matching at all.** `ResolveInterfaceMethodSlot`
   took a same-arity slot unconditionally and the argument loop lowered by bit pattern, so an
   `int` reaching a closure slot emitted `inttoptr` into a code slot and called it (SIGBUS). Both
   slot-picking arms - the lone-slot arm and the multi-candidate first-slot fallback - now share
   one gate; gating only the first left the miscompile reachable through a second same-arity
   overload.

- **RATIFIED: the gate proves a MISMATCH, it does not require proof of a match.** An earlier
  revision gated on "the overload scorer found no match for these arguments" - **WRONG POLARITY**,
  and it broke legal code: the scorer has no int -> floating-point promotion rule, so it declines
  to rank `io.absorb(3)` against a `double` parameter, and every int-like argument to a
  floating-point interface parameter started failing to compile. **Scorer silence is absence of a
  rule, not proof of incompatibility.** `ArgumentProvablyMismatchesParameter` instead takes exactly
  one kind of proof - an integer or floating-point VALUE reaching a function-pointer/closure
  parameter, which can never be code - and accepts everything else.
- **Gating only one of the two interface slot-picking arms left the headline miscompile
  reachable.** Adding a second SAME-ARITY overload to the issue's own repro still exited 138. A
  test leg for that arm is only valid if both overloads share an arity - an arity-2 second
  overload leaves one candidate after the arity filter and silently retests the other arm.
- Not changed, and filed alongside this commit as residues of the same defect class needing their
  own accept-set discussion: [[data-pointer-into-thin-function-param-segfaults]] (the THIN
  parameter arm never widens to a fat struct, so it does not route through the new gate) and
  [[data-pointer-returned-as-closure-not-gated]] (the RETURN path, `CoerceToFuncPtrReturn`, is a
  third caller of the same widening helper and was out of scope for this fix).

### fix/array-shape - the fixed-array shape on the decl-init path

Closed [[fixed-array-copy-invalid-bitcast]] outright, and the NON-POINTER half of
[[auto-binding-of-fixed-array-loses-shape]] - whose pointer-element half is restored at P2 as a
feature gap (see below). They
were one worktree because both are the fixed-array SHAPE being dropped on the SAME binding path
(`ParseDeclaration`'s decl-init in `cflat/MainListener.h`), which the index had flagged as a
plausible-but-unprobed shared root. Probing confirmed it for these two and refuted it for
[[fixed-array-parameter-not-callable]], which stays open.

**Two ratified language decisions.** Both are deliberate; do not "fix" them back.

1. **`auto x = <fixed array>` deduces the array VIEW `T[]` - a borrow, not a copy.** CFlat is
   borrow-by-default and infers copy/move rather than having the user annotate it, and `auto`
   introduces no new storage, so it must not copy. A write through the deduced view IS visible in
   the source array, and vice versa; `Test/test_basic.cb` asserts both directions
   (`auto_fixed_array_is_borrow`, `auto_fixed_array_sees_source`). Anyone who reads the aliasing
   as a bug is reading the decision, not a defect.
2. **`T[N] b = a;` IS a copy**, lowered as a memcpy of the extent. The declared type allocates its
   own storage, so it cannot alias `a`; `fixed_copy_is_independent` / `fixed_copy_source_untouched`
   assert independence in both directions, because a copy that secretly aliased would pass a naive
   read-back check.

**The interface symptom was fixed by the DEDUCTION, not by the guard.** `IShape t = s;` after
`auto s = gInt;` used to emit a raw `Invalid bitcast ... to %__iface_fat_ptr` with no source
location. Nothing in the boxing guards changed: once the binding says `int[]` again, the existing
primitive-array guard sees a pointer-shaped source and produces the correct diagnostic on its own.
Widening that guard to reject un-shaped sources was explicitly considered and rejected - it is the
accept-everything-unproven polarity the guard exists to preserve.

**One rejection was NARROWED, not added.** The "value-typed local assigned a pointer" check
(`Foo f = new Foo()`) fired on `Foo[3] b = a;` with a factually wrong message ("declare it as
`Foo* b`"). It is now exempt for a fixed-array destination, which is a strict reduction in what is
rejected; the shape then reaches the copy path, which validates it properly.

**The copy branch only INTERCEPTS an array-shaped source** - a fixed array or a view. A scalar
RHS (`Foo[3] b = new Foo();`, `char[8] b = "hello";`) is left to the pre-existing checks, whose
diagnostics are the accurate ones for it. The first cut intercepted every pointer RHS and so
replaced a correct, actionable message ("cannot initialize value of type 'Foo' with a pointer of
type 'Foo*'; declare it as 'Foo* b = ...'") with one that was wrong in both halves - it called the
scalar `Foo*` an `Foo[]` and recommended binding a view. Narrowing the interception, not rewording
the message, is the fix; the pre-existing diagnostic is restored verbatim.

**What the copy path rejects, and what master ACTUALLY did with each.** Measured against the
`4097959` binary, not assumed - the first cut of this record claimed all of them were verifier
dumps, and that was false twice over:

| rejected shape | master's real behaviour |
|---|---|
| element/star/extent mismatch (`int[4] b = a`, `int[2] b = <int*[2]>`) | verifier dump, no source location |
| view source (`int[3] c = v`) | verifier dump, no source location |
| owning element type (`Owner[2] b = a`) | a LOCATED LogError, but a factually wrong one ("declare it as `Owner* b`") - never accepted |
| string literal (`char[8] b = "hello"`) | **compiled, linked, ran, exit 0, printed garbage** |

So only ONE shape is a true "master accepted this, we now reject it", and it was a silent
miscompile: a string literal is an `llvm::Constant`, so the bad cast folds into a **ConstantExpr**
that the verifier does not check at the instruction level, and the emitted body then extracts the
bytes of the POINTER VALUE. (Same Constant-vs-Instruction divergence as the primitive-array boxing
record above.) The indexing form crashed the compiler outright at exit 134. Rejecting it is a
strict improvement; it gets its own message that mentions neither extents nor views, and the
missing capability is filed as [[char-array-from-string-literal-has-no-spelling]] with the crash
filed separately as [[fixed-array-storage-guards-miss-four-axes]] (renamed from
`llvm-cannot-select-sign-extend-on-const-array-index` on 2026-08-02, when the fatal error it was
named for stopped reproducing and four other axes were measured in its place).

**On a fixed array the element's pointer-ness lives on `Pointer`, not `ElemPointer`.** `GetType`
applies both flags to the ELEMENT before wrapping it in the ArrayType, so `int*[2]` carries
`Pointer` and `int**[2]` carries both. The first cut read `ElemPointer` alone, which answers 0 for
`int*[2]`; conjoined with `!typeAndValue.Pointer` it made the whole branch UNREACHABLE for every
pointer-element array, leaving `int*[2] b = a;` on the original verifier dump and leaving two
conjuncts dead. Both now do real work and each is pinned by a probe: the star comparison fires in
three spellings (`int[2]` <- `int*[2]`, `int*[2]` <- `int[2]`, `int*[2]` <- `int**[2]`), and the
`destStars == 0` guard on the owning-element arm is what correctly lets `Owner*[2] b = a;` memcpy
rather than be rejected - an array of addresses runs no element destructors.

**Two shapes are REJECTED rather than implemented, both converting a silent miscompile into a
located diagnostic.**

*Pointer-element `auto`.* `auto v = <T*[N]>` produced an unmaterialised shapeless binding that
indexed nothing (garbage), and defeated the interface guard the same way the non-pointer case
did. It is NOT fixable by setting the right flags: `T*[]` COLLAPSES TO `T[]` at parse time in
both `ParseDeclarationSpecifiers` copies, so the explicit spelling does not work either - a
`int first(int*[] v)` parameter accepts a plain `int[3]` and indexes it as `int`, printing
`r=99` on both binaries. Pointer-element array views are an UNIMPLEMENTED FEATURE. Implementing
them means both parser copies plus a full audit of every `ElemPointer`/`IsArrayView` reader,
which the lessons file says costs three review rounds when a widely-read type flag changes. The
representation is not forced and needs no new field - recorded in
[[auto-binding-of-fixed-array-loses-shape]], which is RESTORED at P2 as a feature gap.

*Whole fixed-array assignment.* Closing the decl-init axis left the ASSIGNMENT axis open, and
nobody enumerated it for a full review round: `char[8] b = default; b = "hello";` still printed
garbage, and its indexed-read form still killed the compiler at exit 134. `b = <anything>` on a
whole fixed array is now rejected, with a string-literal wording variant. This breaks no
compiling program - every non-literal spelling ALREADY failed module verification, and the
literal spelling only ever produced garbage or a crash. Element assignment (`b[i] = ...`) is
untouched.

**One pre-existing hole becomes reachable through one more spelling.** `delete[_]` on a view over
stack storage aborts (exit 134, no diagnostic) - always has, via the explicit `int[] v = a;`
spelling (`delete-of-array-view-over-stack-storage`, since fixed by `11e85a1` - see its record
above). Master rejected the `auto` form only
because the broken binding looked like a value type; that was an accident, not a safety check.
Deducing the view makes `auto` behave exactly like the spelling it deduces, which is the point.

**Deferred: the multi-dimensional case.** The `auto` deduction is guarded on
`ConstInnerDimensions.empty()` because there is no correct 2-D target to deduce to - the `T[][]`
view spelling is itself broken on master. The multi-dimensional COPY works and is asserted
(`fixed_copy_multidim`); only the view/`auto` half is deferred. SETTLED by
`fix/mdview` below: the guard stays, and both `T[][]` and `auto` over a multi-dimensional
fixed array are now REJECTED - see that record for why the filed "carry the inner extent"
direction cannot work.

**Blast radius, measured rather than argued.** A 419-file differential sweep (`Test/` + `example/`,
both binaries, isolated `HOME` per side) found exactly 3 differences, all of them the intended new
test legs. Because a corpus sweep that only COMPILES cannot see a silent value change - and the
`auto` defect was exactly that - the sweep was backed by an instrumented build that printed a
marker whenever either new code path fired: across `Test/`, `example/` and all 64 `core/*.cb`, the
paths fire ONLY in the three files this change edits. No pre-existing program in the repo reaches
either one.

### fix/iface-boxing - boxing keyed on the VALUE; the `??` join boxed per arm

Closed the two BINDING ERASURES that
[[interface-boxing-keyed-on-source-binding]] was consolidated on, and routed the last two
open-coded boxing sites through `BoxConcreteIntoInterface`. The issue file survives, NARROWED onto
a different root (see below).

**The two erasures, and how each was closed.**

1. **Parentheses erase the binding, so the ownership transfer never ran.** Every guard in
   `BoxConcreteIntoInterface` keyed off the source `NamedVariable`, and a parenthesized primary
   arrives with `Storage` / `IsOwning` / `CallerName` dropped. `IShape s = (c);`,
   `IShape s = (c) as IShape;` and the assignment-statement `s = (c);` therefore built the box and
   left `c` owning: `delete s` plus `c`'s scope-exit free was a DOUBLE FREE (exit 133/134, no
   diagnostic). The fix is `RetireOwningSourceOfBoxedValue`, keyed on VALUE identity: a pointer
   `LoadInst` whose slot IS the `Storage` of a live owning binding is nulled and marked moved.
   Widening `SoleCastOperandOf` to see through parentheses was explicitly rejected - a syntactic
   walk closes one spelling and the next binding-eraser reopens it.
2. **`??` joins through a SLOT, so nothing was boxed at all.** Its result is a plain `LoadInst`
   with no `TypeName` and no arms recoverable from the IR, so every boxing branch was skipped and
   a raw `ptr` was stored into the fat slot - a module-verifier dump with NO source location.
   `UpcastTernaryPhiToInterface` was generalized into `BoxInterfaceJoinArms` (arms + join point),
   the `??` lowering ledgers its arms (`LLVMBackend::RegisterNullCoalesceJoin`, same clear/park
   points and the same raw-`Value*` invariant as `interfaceBoxRecords_`), and
   `UpcastNullCoalesceToInterface` reads them back. An unresolvable arm now gets the `?:`
   diagnostic with the operator the source actually wrote named in it.

**RATIFIED BEHAVIOUR CHANGE: the paren and `as`-through-paren spellings now MOVE their source.**
`RetireOwningSourceOfBoxedValue` calls `MarkVariableMoved`, so programs that COMPILED AND RAN on
`4097959` are now hard errors:

```cflat
IShape s1 = (c); IShape s2 = (c);   // base: runs. branch: "use of moved variable 'c'"
for (int i = 0; i < 3; i++) { IShape s = (c); }
                                    // base: runs. branch: "use of moved variable 'c'
                                    //                      (moved on an earlier loop iteration)"
```

This is intended. The no-paren spelling `IShape s1 = c; IShape s2 = c;` ALREADY produced exactly
that error on the base binary, so the change makes the two spellings agree; boxing an owning value
into an interface genuinely IS a move, and the old acceptance is precisely what double-freed on
`delete`. Do not "fix" it back without reopening this decision.

A second, milder consequence of the same move: an owning source retired through a paren spelling
whose box is never `delete`d LEAKS, because interface locals are not auto-destructed
(`IShape s = argc > 99 ? ((c) as IShape) : ((e) as IShape);` runs 2 destructors on base and 1 on
the branch). Pre-existing for the no-paren spelling, which already leaks the same way.

**DO NOT RETRY: a per-arm ownership transfer for the `?:` join.** The issue file prescribed it as
the fix for the `?:` double free. It was implemented, measured, and reverted, and an independent
review then rebuilt the branch WITH it enabled and measured it again. Both runs agree:

- It does fix the headline shape (`IShape s = k ? a : b; delete s;` goes 134 -> 0)...
- ...and it breaks `Test/test_move.cb`'s `iface_ternary_thin_borrow_arm_*` legs, which exit 139 at
  `owner->area()` because the borrowed arm's `unique CiMove* owner` was nulled inside its own arm
  block. `test.sh` went 540/0 -> 539/1. It trades a double free for a use-after-null.
- **The existing `armNotOwned` guard cannot rescue it.** That guard rejects only
  `IsProvablyNonOwningPointerLoad`, and `owner` GENUINELY OWNS, so it never fires. Neither would an
  all-arms-own guard, for the same reason.
- **The fact that distinguishes "consume this arm" from "borrow this arm" lives on the
  DESTINATION, not on the arm.** A join into a plain interface local is a BORROW by design - the
  same rule the declaration side already states by rejecting
  `unique IShape s = k ? a : b;` ("cannot initialize unique 's' from a borrowed value"). A per-arm
  transfer at that site is therefore unfixable IN PRINCIPLE, not merely unfinished.

**So the surviving `?:` double free is a DIFFERENT bug**, and the issue file was re-rooted onto it
rather than deleted: `delete` of a borrowed interface box is not diagnosed. It needs no join at
all - `int f(Circle* p) { IShape s = p; delete s; ... }` is exit 139 on both binaries.

**Two sites were ROUTED, not rewritten.** The assignment-STATEMENT boxing and
`CoerceInitValueToInterface` (brace / element init) were open-coded copies of implements-check ->
`RejectPointerShapedInterfaceUpcast` -> data-pointer selection -> `BuildInterfaceFatValue`. Both
now call `BoxConcreteIntoInterface`, so the "this is the only place" claim in its comment is true
again for every SINGLE-VALUE source. A new `adoptsOwnership` parameter is false where the
destination runs its own ownership bookkeeping - a FIELD store refcounts an escaping `new` in
`TransferPointerOwnershipOnStore` instead of nulling the source - so routing did not move
ownership from one mechanism to the other. Routing changed no behaviour EXCEPT through the shared
value-keyed retirement described above.

**Blast radius, measured.** A 505-file differential sweep (`Test/`, `example/` and all of
`cflat/core/`, both binaries, isolated `HOME` per side, comparing stdout+stderr+exit for compile
AND run) found 493 byte-identical, 12 differing: 10 proven nondeterministic (each self-differs
across two runs of the SAME binary - timings, PIDs, ASLR addresses, `HOME` paths) and 2 the
intended new test legs. An instrumented build showed the rerouted assignment site firing in 17
files and the brace-init site in 1, all with identical output.

**What the sweep does NOT bound.** The instrumented build showed `RetireOwningSourceOfBoxedValue`
firing in exactly one file - but that bounds nothing, because NO file in the corpus boxes an
owning value through parentheses. The move consequence above was found by hand-written probes
against the base binary, not by the sweep, and a future change to this path needs the same
treatment.

**Filed rather than closed at the time, and FIXED since** - see the `fix/iface-join-return-boxing`
record below. Only the DECL-INIT and ASSIGNMENT spellings of `??` were wired to the new helper
here; `return p ?? q;` emitted a raw `Function return type does not match operand type of return
inst!` and `take(z ?? b)` was a false rejection. Both now work for a SINGLE `??`, mixed-class arms included.
A CHAINED `a ?? b ?? c` still does not box in any position - filed as
[[chained-nullcoalesce-not-boxed-into-interface]], and note it never worked in the decl-init
position either, so it is not a residue of the return/argument work.

### `fix/iface-join-return-boxing` - `??` joins in RETURN and CALL-ARGUMENT position

Closes the P1 residue of `d1935a2`. Two different mechanisms, because the two positions differ in
what they know and when.

**RETURN - reroute, plus ownership threaded through.** The return site called
`UpcastTernaryPhiToInterface`, which only matches a PHI; a `??` result is a LoadInst, so nothing
boxed and a raw `ptr` reached the `ret`. It now calls the `UpcastPointerJoinToInterface` wrapper,
which tries both spellings. **The trap that had to be avoided:** the wrapper and
`UpcastNullCoalesceToInterface` took no `transferArmOwnership` / `armNotOwned` parameters and the
`??` half hardcoded `false, nullptr`, while the return site passes real ownership handling
(`currentFunctionReturnsOwned`) because a `move`-interface return ESCAPES THE FRAME. Swapping the
call without threading those through would have silently dropped arm ownership for the `?:` leg
that already worked - a regression indistinguishable from a successful fix. Both parameters are now
threaded, and an `armNotOwned` verdict from the `?:` attempt is FINAL (the `??` attempt must not
run and overwrite it). `Test/test_move.cb`'s `iface_ternary_move_return_*` legs pin the `?:`
behaviour: measured, dropping the transfer takes `..._untaken_arm_freed` from 1 to 2 and then
aborts 134 freeing the taken arm twice.

A BACKSTOP was added at the end of the interface-return branch chain: a pointer operand that no
branch could box now raises a LOCATED `cannot convert this expression to interface '<I>': its
concrete class cannot be determined` instead of falling through to the module verifier. **It was
first shipped claiming to be unreachable, and worded as if only a join could reach it; review
disproved both** with `IShape g(Circle* c) { return c + 0; }` - pointer arithmetic, no join
anywhere, and the message said "the join's arms could not be recovered". The wording is now
generic and names no operator. It stays deliberately DIFFERENT from the per-arm helper's
`cannot convert '??' ARM to interface '<I>': the arm's concrete class cannot be determined`, so a
test pinning either wording proves which site fired;
`Test/errors/err_nullcoalesce_iface_arm_unresolved.cb` pins both (the return-join leg on the ARM
wording, the `c + 0` leg on the backstop's).

**CALL ARGUMENT - resolve the target interface locally, then BOX. Do not stamp a class name.**
The issue file's prescription ("route both sites through `UpcastPointerJoinToInterface`") is WRONG
for this half, and this is worth recording: **there is no upcast site to reroute.**
`take(z ?? a)` fails at SCORING - `ComputeOverloadFunction`'s interface clause requires
`StructImplementsInterface(arg.TypeName, ...)` and a join carries no TypeName, so "no overload
matches" fires before any boxing could run. There is also a chicken-and-egg: the target interface
is not known until after overload selection.

The resolution is to answer the chicken-and-egg locally. `BoxNullCoalesceJoinArgument`
(`MainListener.h`) reads the arms out of the join ledger, looks at the ARITY-FILTERED candidate
parameters at THIS argument position, takes the one interface every arm's class implements, and
boxes the join per arm through `UpcastNullCoalesceToInterface`. The argument handed to the scorer
is then a genuine INTERFACE value. `ComputeOverloadFunction` and `CreateOverloadedFunctionCall`'s
argument arms are untouched, which was a hard constraint (a concurrent branch owns that file).

**Do not retry the bare TypeName stamp.** The first cut stamped the arms' shared CLASS name (plus
`Pointer`) onto the argument and let the existing scorer clause and existing lowering do the rest.
It looked minimal and it passed the whole suite. It is wrong, because
`TypeAndValue::IsTypeMatch` compares `TypeName` and IGNORES `Pointer`
(`LLVMBackend.h:624`; the sibling `IsTypePromotion` twenty lines below DOES gate on it). So:

- `int byVal(Circle c); byVal(z ?? a);` - the by-value parameter scored a PERFECT match on a
  `Circle*` and lowered a raw pointer into a struct slot: `Call parameter type does not match
  function signature!`, NO source location. Master rejects the same call with a located
  diagnostic, so the stamp turned a clean rejection into a verifier dump.
- With BOTH `int f(Circle c)` and `int f(IShape s)` present, the by-value candidate WON - the
  stamp displaced the very interface call it existed to enable.

Boxing has neither failure: an interface-typed argument cannot match a by-value class parameter at
all, so the interface candidate wins on its own merits. The underlying `IsTypeMatch` hole is
PRE-EXISTING and reachable with no join at all (`Circle* a; byVal(a);` dumps identically on
master); it is filed separately as [[pointer-arg-binds-by-value-class-param]] and is NOT this
change's to fix.

**What the boxing refuses to do**, so it can only ever help: it bails - leaving the argument
untouched so the ordinary LOCATED diagnostic stands - when the value is not a ledgered join, when
an arm's class will not resolve, when no candidate offers an interface all arms implement, when
two candidates offer DIFFERENT interfaces here, or when ANY candidate takes a POINTER OF ANY KIND
at this position.

That last bail shipped NARROWED to pointers-to-a-CLASS (`param->Pointer &&
IsDataStructure(param->TypeName)`) and review found it silently STEALING the call: with
`f(void* p)` / `f(IShape s)` the branch ran the interface overload (10) where master ran the
pointer one (500), and likewise for `char*` (400) and `int*` (401). All three were working
programs, and neither binary emitted any diagnostic - boxing happens BEFORE the scorer, so the
pre-empted candidate never gets to win. **The whole-corpus differential `--check` could not see
it** (493 files, only the two intended test files differed), and neither could the leg written to
guard the bail: it used a `CiMove*` competitor, the one pointer shape the narrowed predicate DID
cover, so it was aimed at the wrong half of its own predicate. The bail is now plain
`param->Pointer`, and there are four legs - class / `void*` / `char*` / `int*`.

**Why this does NOT interact with the funcptr signature binding landed alongside it** (both edit
the same two call-site argument loops). The propagation of the three `FuncPtr*` fields runs BEFORE
the boxing block in both loops, and the boxing gate reads `TypeName.empty()`, which the
propagation never writes. Note the tempting wrong reason: "boxing bails on any pointer parameter,
and a funcptr parameter is a pointer" is FALSE - a plain `function<T>` parameter has
`Pointer == false` (only `function<T>*` sets it), and boxing does fire at a position shared with a
`function<>` overload. The actual protection is that a funcptr parameter is never `IsInterface`,
so it can never be the boxing TARGET, and a `??` join of function pointers can never satisfy
`ResolvePointerElementTypeName` + `IsDataStructure`. Do not lean on the pointer premise.

The reviewer's suggested INVERSION ("bail unless every candidate parameter here is an interface")
was evaluated and REJECTED: `f(Circle c)` / `f(IShape s)` has a non-interface candidate, so the
inversion refuses to box and leaves both candidates unmatchable - reinstating the exact false
rejection this helper exists to remove. The asymmetry is real, not a compromise: a by-value class
parameter cannot accept a pointer at all, so there is nothing there to steal; a pointer parameter
can, so there is.

The arity filter matters too - without it the stdlib's unrelated 2-parameter
`take(list<string>*, int)` overload contributed a pointer parameter to a 1-argument call's
candidate set and suppressed the fix entirely.

Applied in BOTH MainListener argument loops. The second (virtual dispatch through
`CallInterfaceMethod`) was not in the issue file at all: `ib.run(z ?? a)` died as
`GetOrCreateVTable: '' does not implement 'IShape::area'` on the pre-fix binary AND on master.
Same root; found by enumerating the spelling axis rather than from the repro. There the interface
method's own declared parameters ARE the candidate set, so the resolution is exact.

Because boxing is per arm and each arm carries its own vtable, MIXED-class arms work in argument
position too - the residue the stamp version had to file as a separate issue does not exist.

**Not closed**: a CHAINED `a ?? b ?? c`, in ANY position. The outer join's arm is the inner
join's LOAD, which names no class, so both consumers bail - inertly and correctly, but nothing
flattens the chain. Filed as [[chained-nullcoalesce-not-boxed-into-interface]].

**Verification** (macOS arm64, Release): `./test.sh Release` 554 / 0 / 8; `example_mac.sh` 35 / 0;
`Test/test_move.cb` 516 / 516 (was 497). Every new leg asserts a VALUE, not a compile - the ledger
keys on load IDENTITY, so an intermediate spill makes the lookup miss and the fix silently not
engage, and the pre-fix failure and the did-not-engage failure are BOTH exit 1. `test_move.cb`
does not compile on the pre-fix binary (`(2066,4): ... returned expression is not owned`), which
is the non-vacuity proof for the block as a whole.

### `2bcc5a0` - the return dangle, on the fourth attempt

**The move that made it work: it never asks reachability.** Attempts 1-3 all tried to answer
"which store REACHES this return", which is unanswerable soundly at emission time. Attempt 4
defers to the end-of-body hook beside `RunNullDerefDataflow` (`MainListener.h:7574`), where the
CFG is COMPLETE, and asks a purely EXISTENTIAL question over the returned local's complete
use-list: reject iff at least one store is a ledger-confirmed `FrameStorage` box AND there is
zero accept evidence. Loads and `llvm.dbg`/`llvm.lifetime` are neutral; **every other user
whatsoever - an unrecorded store, a `Heap`/`Parameter`/`Global`/`Unknown` record, a call
argument, an address escape, a memcpy, anything unrecognised - ACCEPTS and stops the walk.**
Every class of missing information therefore lands on ACCEPT, which is what killed 1-3.

- **The null-store knob is `true` (null store is ACCEPT evidence), not the `false` the design
  shipped with.** Review found four confirmed false rejections under `false`, and the reason is
  the durable part: a slot that is frame-boxed and then nulled before the return cannot dangle,
  so treating the null store as merely NEUTRAL re-asks "does a frame box MAY-reach the return"
   - the exact question that killed attempt 2 - through the back door. The flip is provably
  monotone (the flag is read in one place and only ever sets `accepted = true`).
- **Rejected alternative, do not retry**: a SOURCE-level "tainted binding" property. It requires
  observing every assignment site to interface locals, so a missed site is a FALSE REJECTION -
  the wrong polarity, and this family's documented disease is that assignment sites drift.
  Ground the rule in the finished IR's use-list, where completeness is a property of LLVM's
  def-use graph rather than of the compiler having remembered to log something.
- `interfaceBoxRecords_` holds raw `llvm::Value*` and is never retired mid-function. All 9
  erasure sites were traced and the invariant holds today; it is stated at the declaration
  because an unbracketed mid-function erasure added later would let a freed `Value*` be recycled
  into a spurious taint - a FALSE REJECTION mechanism.
- Residue: [[return-dangle-missed-when-slot-has-extra-user]]. Any extra user of the slot
  (notably a method dispatch through it) is accept evidence, so `r.area()` misses the dangle
  where `measure(r)` catches it. **Widening the whitelist to fix it is the direction that
  produced the earlier false rejections.**

### `09f1d56` - generic-interface registration

The surviving design is **record-then-resolve**: `RecordInterfaceMaterialization(name, role)`
appends `{name, file, line, col, role}` at eight value-materialisation sites (global, local,
struct field, by-value parameter slot, rebox source, rebox target, argument coercion, `is`/`as`
source); `ResolveMaterializedInterfaceUses()` runs once where `interfaceTable` is COMPLETE. It
**cannot reject**, so a missed site degrades to "no diagnostic" - never to a false rejection.

**Four earlier shapes failed. Do not retry them:**

1. *Reject at end-of-compile over every syntactic occurrence.* False-rejected mainstream code
   (`int countOf<T>(IEnumerable<T> e)`, any `if const (__WINDOWS__)`-guarded helper with a
   generic-interface parameter): the set includes uninstantiated template bodies whose recorded
   name is the placeholder `IEnumerable__T`, which can never gain an `interfaceTable` entry.
2. *Reject at each materialisation site.* Site enumeration failed twice running - rebox, then
   local, then field, then global, then by-value parameter - each miss a SIGSEGV.
3. *Delete the check entirely.* Re-opened a vtable-laundering miscompile.
4. **The killer argument against any at-site check**: "in `genericInterfaceInstances`, not in
   `interfaceTable`" is a **legitimately transient** state (`LLVMBackend.h:16301`) - a generic
   interface lowers to a fat pointer before its table entry exists. Deferring did not merely fix
   the message; it turned three legitimate shapes from REJECTED into WORKING.

**The struct-wins tiebreak must allow COEXISTENCE, not pick a winner.** `Test/test_generics.cb`
declares `struct Container<T>` (line 21) AND `interface Container<T>` (line 204) and is green:
the two roles live in different maps and `GetType` prefers `interfaceTable`. An exclusive
decision at pre-declare time is the WRONG SHAPE - which is why the suggested backstop `LogError`
("a name in both `dataStructures` and `interfaceTable`") was **deliberately not shipped**. See
[[duplicate-generic-template-name-silently-accepted]].

Two implementation facts worth keeping: `certain` had TWO causes and they were conflated
(`expect_error` blocks also set `certain=false`, so they wrongly got the `if const` hint) - it is
now split, with a separate `ifConstUnfoldable` the ONLY thing that may populate
`ifConstUncertainInterfaceNames`. And "reports every offender" was false, because `LogError`
never returns; the loop, its dedupe set and its RAII restore were dead code, replaced by ONE
aggregated diagnostic.

Six review rounds, six confirmed defect sets, every one while the suite was green: (1) a
cross-file struct/interface name collision false-rejected a legal generic struct reachable from
`core/interfaces.cb`; (2) a generic interface in a dead `if const` branch compiled and SIGSEGV'd;
(3) the round-2 `if const` decider drift turned the merely-parenthesized `if const ((__MACOS__))`
- idiomatic in `core/cruntime.cb:63` - into a raw verifier failure; (4) **vtable laundering** -
an unrouted name is not called but ASSIGNED THROUGH, so `IA ia = a; GiU<int> u = ia; IB ib = u;
ib.M7()` dispatched `IB::M7` through a 1-slot `IA` vtable; (5) global and by-value-parameter
materialisations still SIGSEGV'd while `--check` reported the program CLEAN, and the `is`/`as`
backstops were dead code because `ClassifyCastSource` returned `InterfaceValue` without
populating `shape.TypeName`; (6) the accuracy items above.

### `15809e0` - namespace key space, layer 1 (the template BASE)

**Root cause: three sites disagreed about the key.** Generic STRUCT (`MainListener.h:24091`) and
CLASS (`26488`) registration used the qualified `ns.Base`; generic INTERFACE registration used
the **bare** `Base` (`4111-4113`, where `name` was deliberately shadowed back to `baseName`); the
use site mangled the **spelled** base verbatim (`MangledGenericName("NS.Box", {"int"})` ->
`"NS.Box__int"`), with `ResolveQualifiedName` never applied; and the scanner claim/veto sets
(`2459`, `2478`) used the bare `getText()`. So a qualified use produced a name nothing creates
(`unknown type`), a bare use from inside the namespace missed the qualified key and landed on the
forward scanner's opaque shell (`incomplete layout`), and two namespaces declaring the same
generic interface collapsed onto one key with no diagnostic. **It was never interface-specific** -
generic STRUCT and CLASS were equally unusable; three predecessor files all framed it as an
interface bug because that is what was being looked at.

Shipped as steps 1, 2, 3 and 5: qualify interface registration, resolve the use-site base through
the enclosing-namespace chain (innermost first) before mangling, keep the dot in the mangled form,
and key the scanner sets the same way.

**The mangled-form question was a non-decision.** A NON-generic namespaced struct already
registers and lowers under its dotted name (`%NS.Plain = type { i32 }`,
`define internal %NS.Plain @_NS.Plain_NS.Plain__()`), so a dot is already legal in both LLVM type
names and function symbols. `NS.Box__int` was never an illegal or unusable name - nothing ever
created it.

**Step 4 (key the struct-wins tie-break on the declaring module) was implemented, REVERTED, and
re-filed as [[generic-interface-name-vetoed-by-core-template]].** Two findings, in order:
"different module -> interface wins" is directly contradicted by ratified assertions in
`Test/test_interface.cb` (legs 16/17/19 pin a user `interface GiCollideRev<T>` LOSING to an
imported `struct GiCollideRev<T>`), giving `529 passed, 1 failed`; and narrowing it to
core-vs-user keeps the suite green but TRADES ONE FALSE REJECTION FOR ANOTHER, breaking a program
that declares `interface list<T>` and then uses core's `list<int>`. The root obstruction: both
shapes spell a bare `list<int>` at GLOBAL scope, so they are mutually exclusive and `global::`
cannot distinguish two roles that both live at root scope. It needs a new disambiguating spelling
or an outright collision diagnostic - not a tie-break.

**RATIFIED BEHAVIOUR CHANGES (T1-T5). Do not revert without reopening the decision.** Only these
six shapes behave differently; everything else that compiled compiles the same.

| # | Change | Ratified because | Pinned by |
|---|---|---|---|
| T1 | **TIGHTENING.** A generic interface declared in a namespace is no longer reachable by a BARE spelling from OUTSIDE it (single namespace, no collision): pre-fix `t1=7`, now `Unknown identifier 'Width'.` | The NON-generic analog rejects on BOTH binaries (`unknown type 'P'`), so bare reachability was an artifact of the bare-key bug, not a feature | `Test/errors/err_namespaced_generic_iface_bare_single_ns.cb` |
| T2 | **SILENT MEANING CHANGE.** Inside a namespace, a bare generic name binds to the namespace-local template instead of a same-named GLOBAL one. Nested `A`, `A.B`, `A.C` over a global template: pre-fix `inB=1 inC=1 inA=1`, now `inB=3 inC=2 inA=2` | Inner scope must win; the walk is innermost-first and falls outward correctly | `testGnNsInnerScopeWins` |
| T2b | **SILENT MEANING CHANGE.** A bare generic name inside `namespace Outer` now finds a template nested in a same-named `struct Outer`: pre-fix `1`, now `5` | The non-generic control prints `5` on BOTH binaries, so 5 is the compiler's own answer and pre-fix's 1 was the anomaly | - |
| T3 | **LOOSENING.** A generic struct/class in a NAMESPACE no longer vetoes a same-named GLOBAL generic interface: pre-fix `Unknown identifier 'Width'.`, now `t3=11` | `scannedGenericStructNames` was over-inclusive; step 5 keys it qualified | - |
| T4 | **LOOSENING (bonus).** A generic template nested inside a struct now works: pre-fix `unknown type 'Outer.Inner__int'` | Fell out of the same repair. Round 2 shipped it as a WRONG VALUE (returned 5, the namespace's `Helper`); it now returns 9, matching its non-generic control | `testGnNsNestedInStructNotNamespace` |
| T5 | **TIGHTENING.** A bare generic name used BEFORE a same-named namespace-local template is declared now fails | T2 meeting a PRE-EXISTING gap: use-before-generic-declaration fails identically at global scope on both binaries | - |

T5's diagnostic blames C interop on a file with no C interop - it is the generic opaque-shell
message reused for an incomplete layout of any cause. Filed as
[[incomplete-layout-message-blames-c-interop]].

**Two silent wrong values shipped in round 2 and were caught by review. Both are the same
mistake:** struct nesting and namespace nesting share ONE dotted key space (a template in
`namespace Outer` and one nested in `struct Outer` are both keyed `Outer.Box`), so recovering the
declaring scope with `rfind('.')` on the key resolved a struct-nested template's body against a
same-named namespace. Fixed with a parallel map, `GenericTemplateState::genericTemplateNamespace`,
written at registration from `GetCurrentNamespace()`. The mirror of it lived in the ALIAS path: a
`using` generic-base alias's already-qualified TARGET was piped through the namespace walk at the
USE site, so a global `using GBox = Box;` silently named `NS.Box` inside `namespace NS`. An alias
hit now short-circuits the walk. Pinned by `testGnNsNestedInStructNotNamespace` and
`testGnNsGenericBaseAliasKeepsDeclSiteMeaning`.

**`currentNamespace_` must not survive a reset.** `LogError` THROWS on the batch (`--check`) and
LSP paths, unwinding past any hand-rolled save/restore. Since this fix that value steers the key
space, so a file erroring inside a namespace caused FALSE REJECTIONS in every later file of a
batched `--check` (`Checked 2 file(s), 2 failed` where the second passes alone). Both halves are
required: `ResetForReanalysis` clears it, and every save/restore site is RAII via
`LLVMBackend::NamespaceScope`. **`test.sh` cannot express this regression** - it runs one file per
process; `test.bat` is the batching consumer, and the same reset path backs LSP re-analysis.

**Untested cross-platform risk (Windows only).** `ScanGenericTypeUses`,
`QueueInstantiateGenericType` and `ScanAndQueueGenericTypeUses` now also see the dotted
`qualifiedGenericIdentifier` spelling, so a WinRT base such as
`Windows.Foundation.IReference<int>` can reach `pendingInstantiations` from two sites it
previously could not. It falls through to the idempotent `InstantiateWinrtGenericInterface`, and
the new qualified branch is gated on `IsGenericTemplateKey`, which a winmd base never satisfies.

### `e2a23d5` - namespace key space, layers 2, 3 and 4

The one root, stated once: a generic template's identity, its type arguments, and the names
inside its body were all carried as **spellings re-resolved later**, against whatever scope
happened to be current at the time. The rule every layer converged on:

> **A name must be RESOLVED once, where the scope that gives it meaning is still current, and the
> resolved result RECORDED. Never re-derive a declaring scope from a key string, and never
> re-resolve a spelling downstream.**

It was learned four separate times: (1) layer 1's key conventions; (2) layer 1 round 3's declaring
scope, derived with `rfind('.')`; (3) layer 1 round 3's alias target, re-resolved at the use site;
(4) layer 3's body consumers - `activeTypeSubstitutions` stores the CALLER-resolved name
correctly, but for a global type that name is a bare string, and three downstream sites ran it
back through the enclosing-namespace walk.

**Layer 2 - type ARGUMENTS.** `Box<Item>` in `namespace A` and at global scope both mangled to
`Box__Item`, so the two uses collapsed onto ONE instantiation whose contents were decided by
whichever caller drained it first. Correct is `inA=7 global=9`; pre-fix `09f1d56` gave
`inA=7 global=7` and layer 1 flipped it to `inA=9 global=9` - same wrongness, now landing on the
namespace-local caller, the case this work exists to enable. Fixed with
`LLVMBackend::ResolveTypeArgBaseName` plus `IsTypeArgTypeKey`, whose accept set is
`dataStructures + interfaceTable + gts.scannedTypeNames` - **types only**, so a namespace sibling
function, global or namespace cannot hijack an argument spelling. Both passes resolve through that
one function, and tuple ELEMENTS go through it too, so `(Item, int)` sugar and an explicit
`tuple<Item, int>` cannot mangle differently.

**Layer 3 - the template BODY.** Layer 2 gave the two instantiations distinct mangled names, and
the body then re-resolved the substituted spelling `Item` while the template's DECLARING namespace
was installed, filling both with `N.Item`. Fixed by resolving a substituted name from the root
(`forceRoot`) instead of relative to the declaring namespace - three sites, plus **six
`CreateOverloadedFunctionCall(fieldTypeName, {})` field-initializer sites found in review**, which
the first cut missed: `T t = default;` routes through `GenerateDefaultValue`, so a field with no
initializer took a different path and produced
`Invalid InsertValueInst operands! ... insertvalue %N.Box__Item zeroinitializer, %N.Item %1, 0`.

**Layer 4 - generic FUNCTION templates.** The whole key space was untouched for free functions:
`IsGenericTemplateKey` never consulted `genericFunctionTemplates`, so a BARE call from inside the
declaring namespace either fell back to a same-named GLOBAL template (silent wrong answer) or hard
-errored if none existed. **It was not primarily a collision problem** - a completely unique
namespaced generic function was unreachable bare from its own namespace
(`no overload of 'gf7IdentNsOnly__int' matches the given arguments.`). Qualified calls already
worked, because the call-site text spells the registered key. Headline repro after the fix:
`ns=11 global=10 unique=15`.

**Verbatim pre-fix witnesses**, rescued from the three corpora before they were deleted. Each row
is a shape that a green suite could not see:

| Layer | Shape | Pre-fix result |
|---|---|---|
| 2 | global `Item` vs `A.Item` as a `Box<T>` argument | `inA=9 global=9` (want `inA=7`) |
| 2 | two namespaces + global, one template | `inA=1 inB=1 global=1` (want 2, 3, 1) |
| 2 | `Box<Item*>` with DIFFERING field layouts | `inA=0 global=9` |
| 2 | differing layouts, field only on the losing side | `Unknown identifier 'x'.` |
| 3 | `T` as a FIELD, global instantiation | `9 != 3` (both instantiations got the namespace's `Item`) |
| 3 | `T` as a method PARAMETER | did not compile: `arg=Bs3Item param=BsN3.Bs3Item` - the clearest single statement of the layer |
| 3 | `T` as a method RETURN type | verifier: `Function return type does not match operand type of return inst!` |
| 3 | field with NO `= default`, and a mismatching initializer | silent `expected 9 got 3` (found in review, after the first fix) |
| 4 | bare call from inside the declaring namespace | `ns=10 global=10` (want 11, 10) |
| 4 | UNIQUE namespaced name, bare call from its own namespace | hard error - not a collision at all |
| 4 | declaration order reversed (namespaced first) | hard error |
| 4 | varargs / nested namespaces / inferred type args / member-vs-free | wrong VALUE (all got the global 10) |

**The review lesson this work paid for twice, stated precisely.** Both misses were missing
INPUTS, not weak assertions - every leg here asserts a value and asserts the namespaced and
global answers together, so a collapse cannot hide. Layer 3's legs covered one SPELLING of the
shape; layer 4's first cut covered only the COLLIDING spelling, where a same-named global
template absorbs the call, so a value-correct leg still passes when the bare spelling never
reaches its own namespace's key. Legs C, F, I and L in `Test/test_generics.cb` now carry
UNIQUE-NAME twins whose names exist nowhere globally, and leg J carries the opposite direction of
its collision. **With those axes covered, the current 536/0/8 is much stronger evidence than the
522/526/530 runs were** - those were green over a suite with no namespaced-generic legs at all.
The generalized form is in [`internal/fix-issue-lessons.md`](../fix-issue-lessons.md) under
"On tests".

**Shapes deliberately NOT covered, because they do not exist or fail identically on both
binaries:** parameter pack / `sizeof...` on a generic free function (the grammar rejects
`f<T...>`; `genericFunctionPackIndex` is read but never written); an interface-typed receiver with
a `where`-constrained plain parameter (`InferAndInstantiateGenericFunction` structurally cannot
infer `T` from that shape - the `IVal<T>`-shaped-first-parameter form DOES work and is covered);
member template with inferred type args; and the partially-qualified sibling spelling
(`In.p9id<int>(...)` -> `Undefined variable In.`, non-generic control identical).

**What is still not true about "namespaced generics work".** All four layers are closed, but each
of these remains, filed separately: [[generic-interface-name-vetoed-by-core-template]];
[[namespaced-using-alias-leaks-globally]], [[namespaced-struct-static-method-not-dispatched]],
[[namespaced-interface-shadowed-by-global-is-broken]],
[[tuple-sugar-in-namespace-does-not-compile]]; a generic-interface parameter with a namespaced
type argument fails module verification; a template body cannot name a sibling TYPE of its
declaring namespace; a type nested in `struct Outer` cannot be a generic argument from inside
`Outer`; and a bare call from a STRUCT METHOD body inside a namespace answers the global template
(pinned next to its non-generic control, which does the same). That last one is what a reader is
most likely to hit next when writing namespaced generic code.

**UNFILED, never root-caused:** a `using` generic-BASE alias used in the same namespace as a BARE
use of its target fails on BOTH binaries (pre-fix `incomplete layout`, post-fix `cannot cast an
aggregate value...`). Only the message moved, so not a regression, but the shape had to be avoided
when writing `testGnNsGenericBaseAliasKeepsDeclSiteMeaning`. Suspected: `ScanGenericTypeUses`'s
`tryPreDeclare` pre-declares the shell under the ALIAS spelling while the main pass mangles the
alias TARGET - a guess, not a diagnosis.

### The 2026-07-29 session

- **A primitive-element array boxed into an interface was accepted and miscompiled.** The real
  mechanism is UNREACHABILITY, not a guard that failed to fire: `RejectPointerShapedInterfaceUpcast`
  sits behind a `StructImplementsInterface()` early-out at every boxing site, and `"int"` never
  satisfies that. The GLOBAL vs LOCAL divergence is purely **Constant vs Instruction**: a global
  array operand is an `llvm::Constant`, so IRBuilder folds the bitcast into a ConstantExpr, which
  the verifier does not subject to the instruction-level check - it verified clean and detonated in
  SelectionDAG; a local array decays to a GEP, so a real bitcast INSTRUCTION is emitted and the
  verifier rejects it. Same bug, two completely different-looking outcomes. Four boxing sites
  needed the guard and the fourth (`CoerceInitValueToInterface`, shared by brace-init and the
  `<Tag attr=...>` element path) was missed on the first pass. Parens do NOT defeat the guard; an
  `auto` intermediate did, and that was the DEDUCTION rather than a widening of the guard - see
  the fixed-array shape record below, which closed it exactly that way.
- **Returning a `?:` join of concrete pointers as an interface** aborted with a raw verifier dump.
  The filed account was right as far as it went but missed two things: under a `move` return type
  the plain spelling was a FALSE REJECTION ("returned expression is not owned") because a phi is
  not a `LoadInst`; and boxing alone was not enough, since the helper built its fat value directly
  and never ledgered an `InterfaceBoxRecord`, so the non-`move` heap arm silently leaked and
  nothing nulled the arms' owning locals. Fix: box from the return path BEFORE the ownership and
  dangle checks, ledger each arm, and null each OWNING arm's source inside its own block (verified
  200 constructions / 200 destructions over 100 alternating calls).
- **Duplicate constructor signature crashed the compiler with no diagnostic.** The filed guess
  (runaway recursion) was WRONG: `CreateFunctionDefinition` early-returns an already-bodied
  function before `createFunctionBlock`, the only thing that pushes a function scope, and
  `ParseConstructorDefinition` lacked the guard `ParseFunctionDefinition` has, so
  `RegisterThisPointer` indexed an empty deque. The "corrupted map" in the crash dump was that
  empty-container read - which is why duplicate METHODS never crashed. The message's noun is
  picked by "declares a typeSpecifier", NOT by `declarationSpecifiers() == nullptr`, because a
  real ctor may carry `inline`/`static`/`const`/`extern`/`stdcall` (`CFlat.g4:783`). **Do not
  "simplify" that back.**
- **A function-pointer parameter on an interface method was never lowered, in EITHER direction.**
  Both conversions are now the shared `LowerClosureFatToThinFnPtr` /
  `WidenBareOrThinToClosureFat`. The widen must not key off `isPointerTy()`: under opaque pointers
  every data pointer looks like a code pointer. It REJECTS ONLY WHAT IT CAN PROVE IS DATA. The
  issue was originally fixed for the fat-to-thin half only, because the regression test used
  lambda literals - the one shape that half handles. Cover all four source shapes (literal, named
  function, thin variable, fat variable) against BOTH slot flavours. See
  [[closure-param-accepts-data-pointer]].
- **`as` boxing skipped every ownership guard the plain spelling applies.** One
  `BoxConcreteIntoInterface` (`MainListener.h:9969`) now carries all six guards for the
  declaration-init and `as` paths. The prerequisite that unblocked it - plumbing the source
  `NamedVariable` into `ParseTypeCheckExpression` via `SoleCastOperandOf` - was built and verified
  BEHAVIOUR-NEUTRAL before any guard was added; do it in that order. **The change BREAKS source
  that was only memory-correct because the transfer was missing**: boxing an owning local with
  `as` and then still using it is now `use of moved`. `Test/test_interface.cb` contained such a
  program and was adapted.

### The 2026-07-28 session

- **`as` / `is` fell through to the interface-source path on any unrecognised operand.**
  `GenerateSafeCast` / `GenerateIsCheck` inferred "this is a fat pointer" from the ABSENCE of a
  concrete struct name, so a pointer `?:` phi and a decayed `T[N]` both read unrelated storage as
  `{vtable, data}`. Replaced with `ClassifyCastSource`, a positive routing decision. **The two
  shapes needed OPPOSITE answers** - the ternary had to be made to BOX (the plain spelling already
  worked, so rejecting it would have regressed expressiveness) while the array had to be REJECTED
  with the plain spelling's exact wording. That is why checking the plain spelling first is the
  rule, not a nicety. The filed severity was also wrong: it was recorded as a compiler crash with
  zero output, but `--run` JITs in-process, so the PROGRAM's SIGSEGV looked like the compiler's.
- **`as` cast of a stack value to an interface crashed the compiler** - `elemType` propagation:
  `ParseMultiplicativeExpression` populates `TypedValue::elemType` only for pointer sources, so a
  stack class value reached `GenerateSafeCast` with a null `elemType` and `CreateExtractValue`
  ran on a class aggregate.
- **Named arguments were ignored on the interface call path.** Fixing it made call-site index and
  declared-parameter index diverge on that path for the first time, exposing three downstream
  sites that had silently relied on them being equal. Auditing the fields the interface arm failed
  to copy surfaced two more: a FALSE REJECTION of legal `alignas` code on a `move` parameter, and
  a SILENT MISCOMPILE where `u8 200` through an interface widened to `-56`.


## Consolidation record (2026-07-30)

Three root-level merges, 64 open issues -> 58. Each merged only where the files themselves
named a shared root and a shared fix vehicle - never on a shared symptom.

| Merged into | From | Root |
|---|---|---|
| Generic namespace key space (fixed, `e2a23d5`; record below) | `generic-template-namespace-key-space`, `generic-type-arguments-not-key-space-resolved`, `generic-template-body-rebinds-substituted-type-arg`, `generic-function-templates-are-bare-keyed` | Names carried as SPELLINGS re-resolved later, instead of resolved once and recorded. Four layers: base, arguments, body, function templates. |
| [[interface-boxing-keyed-on-source-binding]] | `interface-boxing-guards-are-binding-dependent`, `null-coalesce-join-into-interface-not-boxed`, `interface-boxing-sites-not-fully-consolidated` | Boxing keys off the source `NamedVariable`, so parens / `?:` / `??` fall through. Correct for parens and `??` (both fixed 2026-07-31); WRONG for `?:`, whose join is a deliberate borrow - see the narrowed file. |
| [[overload-replay-blames-wrong-candidate]] | `named-arg-replay-reports-losing-candidate`, `iface-slot-replay-blames-wrong-slot` | A non-probed replay with no notion of which candidate the user meant. Both files already named the same fix. |

Also retired: `generic-interface-registered-as-opaque-struct` (fixed and committed as
`09f1d56`; its design record is in the archive, and the gaps it spawned are all filed
separately). Newly filed: [[incomplete-layout-message-blames-c-interop]], split out of the
key-space work because two ratification records lean on a message that names the wrong cause.

**Deliberately NOT merged**, so their findings are not buried:

- The four namespace gaps ([[namespaced-using-alias-leaks-globally]],
  [[namespaced-struct-static-method-not-dispatched]],
  [[namespaced-interface-shadowed-by-global-is-broken]],
  [[tuple-sugar-in-namespace-does-not-compile]]) share the word "namespace" and nothing else:
  registration scope, call dispatch, lookup order, and a parser gap.
- The fixed-array trio was grouped on a plausible-but-unprobed shared root (the array SHAPE is
  dropped to a bare `T`). Probing it settled the grouping: the `auto` binding and the array-to-array
  copy DID share the decl-init path and were fixed together; [[fixed-array-parameter-not-callable]]
  did not and remains open. See the fixed-array shape record below.

### fix/global-positional - brace-list globals rejected, both scopes and both brace spellings (RATIFIED)

Closes [[global-struct-positional-init-silently-zeroes]].

**What is now rejected.** A struct/union/class-typed variable with a non-empty brace-list
initializer, in EITHER spelling (`S x = {...};` or the bare `S x {...};`, which carry the list on
different grammar nodes - `initializer->initializerList()` vs. `initDecl`'s own
`LeftBrace()`/`initializerList()` - and previously only the `=` spelling was even considered):

- At GLOBAL scope: both the positional form (`S gs = {1,2};`) and the named form
  (`S gs = {a=1,b=2};`) are rejected. Positional gets the same diagnostic the LOCAL declarator
  already gave ("positional initializers are not supported for struct type 'S'; use 'field =
  value'"); named gets a new one, since the local declarator DOES support named field-init but
  there is no compile-time-constant construction path for it at global scope.
- Generic containers (`list<T>`, `array<T>`, `dictionary<K,V>`) hit the identical fallthrough at
  global scope and get their OWN message - their positional form is exactly what the LOCAL
  declarator supports (desugars to `add()`/`set()` calls), so "use 'field = value'" would be a
  false remedy for them.
- At LOCAL scope, the bare-brace spelling is now routed through the same handling the `=`
  spelling already had: bare positional (`S ls {1,2};`) is rejected exactly like `S ls = {1,2};`
  always was; bare named (`S ls {a=1,b=2};`) now WORKS exactly like `S ls = {a=1,b=2};` always
  did (assigns the fields for real) - this is not a new reject, it closes a silent-zero gap the
  local declarator had for one specific spelling.

**Why reject rather than implement, at global scope.** A global's initializer must be an LLVM
`Constant` built at compile time; the local declarator's named-field-init path stores through a
real stack `alloca` (`EmitFieldInitializer`) and its container path emits real `add()`/`set()`
CALL instructions (`TryEmitContainerInitializer`) - neither has any global counterpart, and
building one (recursively constant-folding arbitrary field-initializer expressions, or inventing
a global-constructor mechanism for containers) is a materially larger feature than closing a
silent-discard bug. Rejecting turns "compiles clean, wrong value, exit 0" into a diagnostic with
a working remedy (`= default` then assign fields in a function, verified to actually compile and
produce the right values) - strictly safer, and swept against `Test/`, `example/`, and
`cflat/core/` first: no `.cb` anywhere in the tree declared a global struct/union/class/container
with a brace-list initializer, so nothing that worked before this change stopped working.

**The container carve-out from round 1 of this fix was wrong and got reverted in round 2.**
The first pass exempted `list__`/`array__`/`dictionary__` types from the new guard on the theory
that implementing their construction was out of scope - but leaving them unrejected preserved
the EXACT bug the issue was filed for for those three types (silent discard, no diagnostic,
`list<int> g = {1,2,3};` at global scope compiled clean and read back empty). Rejecting a
construct needs nothing the struct case did not already have; only IMPLEMENTING it needs the
missing global-constructor mechanism. Read as a general lesson: when a "cannot implement, so
carve out" decision is made, check whether REJECTING the carved-out shape was actually blocked
by the same constraint - usually it is not.

**The mangled-name-prefix container test is verdict-invariant by construction.** `isContainerType`
(matching `list__`/`array__`/`dictionary__`, the same prefixes `TryEmitContainerInitializer`
already keys on) selects WORDING ONLY - a miss degrades to the less-precise but still-true
struct/union/class message, never to acceptance. Kept as a prefix test rather than a proper type
property because none exists on `StructData` to distinguish a generic container from a plain
aggregate; a future refactor that adds one should switch this over.

**Three more user-visible behaviour changes ride along with the bare-brace-spelling work, all
found by review and all worth recording explicitly rather than leaving implicit in the diff.**

1. A PRIMITIVE-typed local declaration with a non-empty brace list (`int x {5};` / the
   pre-existing `int x = {5};`) now REJECTS with an accurate message ("brace initializer with
   values is not supported on 'x' - 'int' is not a struct/union/class or a recognized
   container..."). PRE, the bare spelling silently read an UNINITIALIZED garbage value (`x`
   held whatever was already on the stack, e.g. `x=1876644176`, compiled clean, exit 0) - a
   second, independent silent-garbage bug this fix's bare-brace routing exposed by finally
   giving `right` a value to fall through to `EmitFieldInitializer` with. The `=` spelling
   already rejected before this fix, but with the SAME misdescriptive message
   ("'int' is not a known struct type") this change also corrects for both spellings. Primitive
   brace-VALUE-init (making `int x {5};` actually assign `5`) is NOT implemented - that is a
   language feature, out of scope for this bug fix, and not filed as its own issue since nothing
   currently depends on it and the reject is the safe default. Note this does NOT apply at
   GLOBAL scope: `int x = {5};` / `int x {5};` at file scope are a different, PRE-EXISTING
   silent-zero gap (confirmed identical on `58d5d27`, unaffected by this fix in either
   direction) - primitive globals are not struct-shaped, so the guard in this fix's global path
   (gated on `GetDataStructure(...).StructType != nullptr`) never sees them, exactly like the
   interface-typed-global gap recorded below.
2. Empty bare-brace (`T x {};`) now zero-initializes on types that PRE left as uninitialized
   garbage: a primitive (`int x {};` -> PRE garbage, POST `0`) and a pointer, including `unique`
   (`S* p {};` / `unique S* p {};` -> PRE a non-null garbage POINTER VALUE, POST `nullptr`). This
   is because `GenerateDefaultValue` is now called unconditionally for ANY `barebraceInit`
   (empty or not) rather than only when the brace list was non-empty - the empty case simply
   never took the old "apply field overrides" step, so it inherits the safe default. Silent and
   strictly safer, which is exactly the kind of change that needs a record rather than the kind
   that does not: a caller relying on `S* p {};` NOT being null (there should be none - reading
   an uninitialized garbage pointer is undefined behaviour in the first place) would observe a
   different, DEFINED value now.
3. A NON-empty bare-brace field-init on a POINTER declaration (`S* p {a=1};`) changed VALUE, not
   just verdict: PRE, `undef` (stable at one garbage bit pattern on a given build, e.g.
   `0x1f7808100`, per `--out-lli` literally `ptr undef`, since the bare spelling's brace list was
   discarded entirely - the exact bug this fix closes for struct/container/primitive); POST,
   the deterministic bogus address `0x1` (the SAME pre-existing pointer-corruption bug the `=`
   spelling already had, now reached by the bare spelling too because both now share one code
   path). Both values are wrong and neither is safe to dereference; this is the routing fix
   working as intended (making the two spellings agree) colliding with a DIFFERENT, unfixed
   pre-existing bug (`pointer-decl-field-init-brace-corrupts-pointer-storage` (FIXED 2026-08-02, file deleted), filed, not
   fixed here) rather than a new defect class of its own. Recorded here because "from undef to
   a different wrong value" is still a measured behaviour change, and a first draft of that
   issue file wrongly called the bare spelling's value identical across both binaries -
   corrected in the file itself; this entry exists so the design record does not repeat that
   error.

**Left open, filed separately, NOT closed by this change** (all found during Phase A enumeration
or the review rounds of this fix, all confirmed to reproduce identically on `master`):
[[class-no-ctor-default-construct-returns-undef]] (a `class` with no user constructor
default-constructs to `undef`, unrelated code path), [[struct-field-default-brace-list-discarded]]
(a struct FIELD's own brace-list default is dropped, not a variable declarator), and
[[interface-typed-global-brace-init-discarded]] (an interface-typed global falls through this
fix's guard entirely, since `GetDataStructure` has no entry for an interface name) - plus the
pre-existing [[global-struct-no-initializer-ignores-field-defaults]] (a global with NO
initializer at all zeroes instead of honoring field defaults; different code path again, the
`right == nullptr` branch rather than the brace-list branch) and
`pointer-decl-field-init-brace-corrupts-pointer-storage` (FIXED 2026-08-02, file deleted) (`S* p = {a=1};` - a POINTER
declaration reaches the pre-existing `EmitFieldInitializer` call unguarded, since its `TypeName`
is the pointee `S`, a known struct; `EmitFieldInitializer` then GEPs into the pointer's own
8-byte slot as if it addressed an `S`, leaving `p` holding a nonsense address built from the
field values. The `=` spelling is identical on `58d5d27` and `af68158`; the BARE spelling
`S* p {a=1};` is NOT - see behaviour change 3 above and the file itself. This fix's new
primitive-typed guard does not and cannot see either spelling, since it is gated on the pointee
name resolving to a non-struct).

### fix/ptr-fieldinit - a brace initializer on a POINTER target rejected at four of five call sites (RATIFIED)

Closes `pointer-decl-field-init-brace-corrupts-pointer-storage`. The filed repro was verified
verbatim on a `dd6f836` Release build before any edit: `S* p = {a=1};` compiles rc 0, runs rc 0 and
prints `p=0x1`. The `--no-opt` IR confirms the filed root cause exactly - the optimized IR folds it
to a constant `inttoptr`, so this reading had to be taken unoptimized:

```
%p = alloca ptr, align 8                                  ; the POINTER's own 8 bytes
store ptr null, ptr %p, align 8
%a_init = getelementptr inbounds %S, ptr %p, i32 0, i32 0  ; GEP'd as if %p addressed an S
store i8 1, ptr %a_init, align 1                           ; field "a" lands in the pointer
%0 = load ptr, ptr %p, align 8                             ; read back as the pointer VALUE
```

**Five call sites; FOUR were broken.** The issue file named the local scalar declarator and said the
`new` and named-argument callers were "not checked". All four of the others were checked by probe,
and three of them reproduced. A first cut of this fix rejected at all five and was WRONG about the
fifth - see the last row.

| `EmitFieldInitializer` caller | Reachable with a pointer target? | Pre-fix behaviour |
|---|---|---|
| local scalar declarator (`~10102`) | YES - `S* p = {a=1};` and every neighbouring spelling | BROKEN: `p == 0x1`; the container sibling `list<int>* lp = {1,2};` compiled and SIGSEGV'd at runtime |
| fixed-array seed (`~9052`) | YES - `S*[2] arr = {a=1};` | BROKEN: `arr[0]` is the PACKED FIELD BYTES - `0x1` for `struct S { int a=0; int b=0; }`, `0x900000001` for `{ int a=7; int b=9; }` with `{a=1}` (b keeps its 9); and `S*[2] arr = {};` memcpy'd the pointee's field DEFAULTS over each slot (`0x7` for a leading `int a = 7`) |
| default-parameter wrapper (`~7837`) | YES - `int f(S* p = {a=1})` | BROKEN: the caller received the field bytes as `p` (`1`) |
| `new T{...}` (`~17680`) | YES, but ONLY via generic substitution | BROKEN: `new S*{a=1}` and `new PS{a=1}` (alias) are both rejected before codegen, so the syntax axis alone said "unreachable". `struct Mk<T> { ... new T{a=1} ... }` instantiated as `Mk<S*>` reaches it and yields `0x1`. The probe that found this was written only because the site was on the audit list |
| named-argument brace at a call (`~22859`) | YES - `g({a=1})` and `g(p: {a=1})` where `g` takes `S*` | **CORRECT - not rejected, nothing changed here.** See below |

**The named-argument site was audited and found CORRECT.** It is structurally incapable of the bug:
it builds its OWN struct alloca (`paramType.TypeName = structType` with `Pointer` left false, then
`CreateAlloca` on the struct's default value) and hands THAT to `EmitFieldInitializer`. The pointer
variable's 8 bytes are never the destination, because there is no pointer variable - the argument is
a materialized temp whose address is passed. Measured on master `7f41a15` with
`struct Ptt { int a = default; int b = default; }` and `int zzt(Ptt* p) { return p->a*10 + p->b; }`:

```
zzt({a=1,b=2})        -> 12    (correct field values, through a real temp)
zzt(p: {a=1,b=2})     -> 12
h.use({a=3,b=4})      -> 304   (method receiver spelling)
```

The first draft's audit misread a truncated stack ADDRESS of a valid temp as "corrupted field
bytes". The tell that separates this site from the other four: the three genuinely-broken sites
return the packed field bytes themselves (`0x1`, `0x900000001`), while this one returns an address.
Rejecting here removed a working feature, so the reject, the `outAllPointer` out-parameter of
`ResolveInitializerArgType`, and the `sawPointer && !sawValue` overload-set logic that existed only
to serve it were all removed; the site is byte-identical to master. Three positive legs in
`Test/test_initializer_list.cb` (positional, `name:`, method receiver) now pin the values so it
cannot be broken by a future round.

**The rejection, and its polarity.** All four rejecting sites go through one helper,
`LogPointerBraceInitReject`, whose message names the ROLE (`declaration 'p'`, `array element of
'arr'`, `parameter default for 'p'`, `element of 'new S*'`) so a test can prove WHICH site fired.
What is rejected is provable, not inferred: the target is a pointer AND a non-empty brace list is
present.

- The local-site guard sits in the `else if` chain AFTER the existing non-struct reject, so `int x
  {5};`, `void* p = {a=1};`, `function<int(int)> fp = {a=1};` and an interface-typed local keep
  their own (better) messages instead of being re-blamed as pointer errors.
- Fixed arrays and `T[]` views never reach that arm - both branches above it `continue`. This
  matters: `int[] v = {1,2,3}` is legal and IS `IsArrayView + Pointer`, so a bare `Pointer` test
  placed one branch earlier would have false-rejected it. Measured, not assumed.
- The declaration message carries the `unique` qualifier into its remedy suggestion
  (`unique Uq* p = new Uq();`), so the suggested spelling is a legal declaration.
- ~~Empty `{}` is never rejected BY THE POINTER GUARD.~~ **SUPERSEDED** by the empty-brace record
  below: empty `{}` on a pointer target is now rejected in every spelling, on an AMBIGUITY
  argument this record did not consider.

**Behaviour change beyond the rejection (~~ratified~~ SUPERSEDED).** `S*[N] a = {};` used to build
one default-constructed pointee and memcpy its first 8 bytes over every pointer slot, so a struct
with a non-zero leading field default produced non-null element addresses (`0x7`). This change made
it zero-init. **That decision was reversed the same day** - see "empty `{}` split by target type"
below, which rejects the spelling instead. The zero-init VALUE legs described here are gone from
`Test/test_initializer_list.cb`; the spelling's coverage is now an `expect_error` leg. What survives
from this record: the three named-argument must-still-work legs, and
`Test/errors/err_ptr_brace_init.cb`'s 9 non-empty-list legs, each mutation-tested individually.

**A compiler SIGSEGV turned into a diagnostic, in the same function this fix edits.**
`int f(int x = {})` - an EMPTY brace list as a parameter default - left `defaultVal` null in the
default-parameter wrapper and segfaulted the compiler (rc 139, zero output) for `int`, a struct and
a pointer alike, on master and on the first cut of this branch. Per CLAUDE.md's debugging workflow
(a diagnosed crash gets a proper error message), a null-`defaultVal` bail was added here; all three
spellings now give rc 1 with a located diagnostic. This was containment only; the SEEDING root cause was
closed the same day by `fix/emptybrace` (record at the end of this file), which also removed the
bail for a NON-pointer default and replaced it for a POINTER one with the ambiguity diagnostic.

**Left open, filed separately, NOT closed here** (both found by this fix's Phase A enumeration):
the `T x = {};` spelling never seeds anything (`int x = {};` reads undef) while the bare-brace
spelling of the same construct seeds correctly, so the two gates ask different questions - only its
crash face was contained here. (That one is now CLOSED by `fix/emptybrace`; its issue file is
deleted.) And
[[string-literal-containing-braces-retyped-as-string]] - a literal whose CONTENT contains `{}` is
typed `string` rather than `char*`; found only because a test label contained `= {}`. That one was
filed as a false rejection and is worse than filed: `printf("a = {} b\n");` compiles rc 0, runs rc 0
and prints binary garbage on BOTH binaries, and the dedicated `cannot pass 'string' to the variadic
'...'` guard does not fire for it. Its file now records the miscompile face too.

Also confirmed NOT reachable, with the measurement rather than an argument: `S* p; p = {a=1};`
(assignment form) and `S** q = new S*{a=1};` are parse errors; a global pointer declaration in every
brace spelling was already rejected by `fix/global-positional`; a struct FIELD default
(`struct W { S* q = {a=1}; };`) silently discards the list and is the already-filed
[[struct-field-default-brace-list-discarded]], a different path that never calls
`EmitFieldInitializer`.

Bar (measured on the round-2 commit, rebased onto master `7f41a15`): `./test.sh Release` 574 passed /
0 failed / 8 skipped against master's own 572/0/8 - +2 for the one new errors file, which the suite
runs cold and warm. The suite counts FILES, so dropping the two named-argument legs from that file
does not move the number. `bash example_mac.sh Release` 35 passed / 0 failed, same as master.

### fix/mdview - every unsized multi-dimensional bracket form REJECTED (RATIFIED)

Closes `multidim-array-view-binding-loses-shape`. All three filed repros were verified verbatim
on a Release build of the parent `5a6580c` before any edit, and all three reproduced exactly as
filed (`v=1`, garbage, `p=1`).

**Root cause, and why it is bigger than the file said.** The grammar is
`arrayDimSpec : ('[' assignmentExpression? ']')+`, so every bracket pair folds into ONE context
and the EMPTY pairs contribute nothing to `assignmentExpression()`. Every consumer read only that
vector, so the bracket COUNT was invisible: `int[][]` parsed as `int[]`, `int[][3]` as `int[3]`,
and `int[2][]` as `int[2]`. The filed root cause ("the row stride is gone") is the symptom; the
mechanism is a dropped bracket. Measured with `a[i][j] = i*10+j` on an `int[2][3]`, a `T[][]`
view read flat index 3 (`a[1][0]`, value 10) for `v[1][2]` - stride-less pointer arithmetic, in
the declaration, parameter, return, field and global positions alike.

**The filed fix direction cannot work and was NOT taken.** "Carry `ConstInnerDimensions` on the
view so the subscript emits the row stride" is expressible only where the extent is known at the
binding site, i.e. a local with an initializer. A `T[][]` PARAMETER has a different extent per
call, and a `T[][]` return type, field and global have none at all - a thin `ptr` view carries no
row stride by construction, and making views fat is a whole-language representation change. That
direction fixes 2 of the 11 miscompiling cells and leaves 9 needing a rejection anyway; rejecting
all 11 consistently is the smaller and provable change.

**What is now rejected.** One predicate, `LeftBracket().size() > 1 && assignmentExpression().size()
< LeftBracket().size()` - i.e. an empty `[]` anywhere in a multi-bracket list - at six sites:
both copies of `ParseDeclarationSpecifiers` (guarded once at the top of each, before any branch
consumes the brackets, so the funcptr, funcptr-alias and general branches are all covered), the
`using` alias path, the `(T[][])` cast/abstract-declarator path, and `IsArrayViewArg` /
`IsBadArrayArg` for the generic- and tuple-type-argument positions. Separately, `auto x = <T[N][M]>`
is rejected in `ParseDeclaration` as a sibling of the existing pointer-element reject: the decayed
element of a multi-dimensional array is a ROW, so `T[]` is the wrong deduction and `T[][]` no
longer exists. The `ConstInnerDimensions.empty()` guard on the deduction therefore STAYS.

Pre/post per cell, measured on `5a6580c` Release and on this commit (`10` = the stride-less flat
index 3 of an `int[2][3]` filled with `i*10+j`; the correct answer is `12`):

| Cell | Pre | Post |
|---|---|---|
| `int[][] v = a;` (local decl) | rc 0, prints 10 | rejected |
| `int f(int[][] v)` (parameter) | rc 0, prints 10 | rejected |
| `int[][] f()` (return) | rc 0, prints 10 | rejected |
| `struct H { int[][] v; }` (field) | rc 0, prints 10 | rejected |
| `int[][] gv = ga;` (global) | rc 0, prints 10 | rejected |
| `int[][] v = nullptr; v = a;` (assign) | rc 0, prints 10 | rejected |
| `struct S { int m(int[][] v) }` (method param) | rc 0, prints 10 | rejected |
| `int[][] v` passed on to an `int[] ` param | rc 0, prints 10 | rejected |
| **`int[][] v = new int[10]; v[3]=3;`** | **rc 0, prints 3 - CORRECT** | **rejected (see "Two working shapes removed")** |
| **`int f(int[][] v)` called with a 1-D `int[4]`** | **rc 0, prints 7 - CORRECT** | **rejected (see "Two working shapes removed")** |
| `int[][3] v = a;` | rc 1, "cannot initialize fixed array 'int[3]'" (a type never written) | rejected, named correctly |
| `int[2][] v = a;` | rc 1, "cannot initialize fixed array 'int[2]'" (ditto) | rejected, named correctly |
| `int[][][] v = a;` (3-D) | rc 0, wrong value | rejected |
| `(int[][])p` (cast) | rc 0, wrong value | rejected |
| `using M = int[][3];` | rc 1, folds to alias "int[3]" | rejected as an alias |
| `Box<int[][]>` (generic type arg) | rc 0, prints 10 | rejected |
| `(int, int[][])` (tuple element) | rc 1, unrelated cast-of-aggregate error | rejected on the bracket form |
| `auto s = a;` over `int[2][3]` | rc 0, garbage (`-142573312`) | rejected |
| `auto s = a;` over `int[2][3][4]` | rc 0, garbage | rejected |
| `auto s = a;` over `Cell[2][3]` | rc 1, "Undefined variable x" (shapeless binding) | rejected, named correctly |

**Two working shapes ARE removed by this change - accepted deliberately.** The rest of the table
is "was silently wrong -> now rejected", but two rows are not, and a reader must not scan past
them. Measured on `5a6580c` Release:

```
int[][] v = new int[10]; v[3]=3;  ->  r=3   correct
int f(int[][] v){ return v[1]; }  ->  r=7   correct   (called with an int[4])
```

Both now hard-error. They work only BECAUSE the extra `[]` is silently dropped - they are
misspellings of `int[]`, and the one-character rewrite (`int[] v = new int[10];`,
`int f(int[] v)`) gives the identical answer on both binaries, verified. Keeping them accepted
is not a free win either: the same dropped bracket is exactly what made `int[][] v = a` over a
2-D array flat-address, so there is no rule that accepts these two and rejects that. The
diagnostic names the rewrite ("Write a single `T[]` for a flat view of the whole allocation"),
which is why that clause is in the message rather than only in this record.

**One remedy the diagnostic names has a known gap, filed not fixed.** "size every dimension
(`T[N][M]`)" is sound for the TYPE - `char[2][8] b = default;` compiles and runs - but a
multi-dimensional fixed array has no working brace initializer on EITHER binary (nested braces
are a parse error, a flat list counts against the outer dimension only, and string-literal
elements hit the fixed-array pointer-store reject). Pre-existing and untouched here; filed as
[[multidim-fixed-array-has-no-brace-initializer]]. The shape that surfaced it,
`char[][8] names = {"ab","cd","ef"};`, was rc 0 + SIGSEGV before this change (the outer
dimension was dropped, so `sizeof` was 8) and is a genuine new catch.

**Accept set, frozen BEFORE the guard was written and re-run after - byte-identical on both
binaries.** 1-D view decl (`7`), `auto` over a 1-D fixed array (`7 5`, borrow both ways), 1-D
view parameter (`7`), 1-D view return (`7`), view reassignment (`9`), fixed 2-D copy (`7`), row
view read+write-through (`3 77`), struct-element view (`7`), view as a struct field (`7`), direct
2-D subscript (`7 3`), `sizeof` of a 2-D array (`24`), `char[]` view (`h`), a row passed to a
`T[]` parameter (`7`), a global 1-D view (`7`), a fully-sized 2-D alias copy (`7`), `Box<int[]>`
(`7`), `(int[])p` cast (`7`), 1-D view as a method parameter (`7`), and `int[] v = <2-D array>`,
which is a FLAT view of the whole allocation (`v[4]` is `a[1][1]`) - identical pre and post. That
last one is why `int[][] v = a` had to go: it was producing exactly this flat addressing under a
2-D spelling. Unchanged rejections in the same neighbourhood, also measured on both: `T*` bound to
a `T[]`, `int*[] v = <int*[3]>`, `using V = int[];`, a tuple element initialized from a fixed
array, and a non-constant array dimension.

**Not in scope, with the reason.** A FIXED-extent parameter (`int f(int[3] r)`, `int f(char[8] b)`,
and equally `int f(int[2][3] m)`) still false-rejects at the CALL with `[0] ptr <unnamed>` and a
candidate mangled `(int r)` - identical on both binaries. That is a dropped SIZED extent, a
different defect from a dropped EMPTY bracket, in a path this change does not touch; it is already
filed as [[fixed-array-parameter-not-callable]], whose file now records the multi-dimensional
spelling too. The parameter axis is therefore still inconsistent across the two spellings, but no
longer between "silently wrong" and "false reject" - both are now hard errors.

**Blast radius.** A 677-file `--check` differential over every `.cb` in the tree (both binaries,
each with a freshly rebuilt local cache) found 3 differences: the new errors file, and the two
tuple diagnostics whose wording gained "an unsized multi-dimensional 'T[][]'" (same exit code,
still PASS). No `.cb` anywhere in the repo - `Test/`, `example/`, `core/` - contains a `[][]`,
`[][N]` or `[N][]` spelling, so no file in the repo changed behaviour. That is NOT the same as
"nothing that worked before stopped working" - see the section directly below. Caveat on that
number: a first sweep reported 14 extra "regressions" in `core/*.cb` ("redeclaration of global");
they were an artifact of one side having a STALE local cache, and vanish when both sides are
re-`--init-local`ed - the pre binary reproduces them exactly with a fresh cache.

**Coverage.** `Test/errors/err_multidim_array_view.cb` carries 15 scoped-block legs, one per
rejecting site/axis; each was extracted into a single-leg file and run against BOTH binaries -
all 15 fail on `5a6580c` and pass here, so none is vacuous and none is riding a pre-existing
guard. `Test/test_basic.cb` gains three VALUE legs next to `fixed_copy_multidim`: a fully-sized
2-D declaration, the flat whole-array view, and the row view's write-through. To be precise
about what protects the guard's polarity: the tripwire is the `int[2][3]` DECLARATIONS that
`fixed_copy_multidim` already had - a bracket-count-only rule would hard-error on those, and
they predate this commit. The added `fixed_multidim_sized_decl` line is a value assertion over
them, not the tripwire itself.

No new `TypeAndValue` / `StructData` / `AnnotationValue` field, so no `--init` round-trip work:
`ConstInnerDimensions` already serializes in BOTH systems (`internal/simd-type.md` documents the
pair) - llvm::json in `LLVMBackend.cpp` under key `"aid"` (write `:4221`, read `:4275`, nested
inside the `ConstArraySize > 0` guard) and nlohmann in `LLVMBackend.h` under key `"idims"`
(`:20666` / `:20696`). This change only READS the field, in one new place.

Bar: `./test.sh Release` **576 passed / 0 failed / 8 skipped** against master's 574/0/8 - +2 for
the one new errors file, which the suite runs cold and warm. `bash example_mac.sh Release`
**35 passed / 0 failed**, same as master.
### fix/emptybrace - empty `{}` split by TARGET TYPE: seeds a non-pointer, rejected on a pointer (RATIFIED)

Closes [[empty-brace-initializer-never-seeds-and-crashes-on-defaults]]. `{}` produces a NULL
`initializerList()` (the list rule requires >= 1 element), so every gate written as
`initializerList() != nullptr` missed it while the sibling gates written on the `LeftBrace()` TOKEN
did not. That single asymmetry is the whole root cause, confirmed from `--no-opt` IR rather than taken from
the issue file. Two halves, verified apart.

**Half 1 - a NON-pointer `{}` seeds, in both spellings.** The local declarator arm
(`MainListener.h`) and the default-parameter wrapper are now gated on the brace token. Measured
`--no-opt` IR, not probe values: PRE `int x = {}` emitted `%x = alloca i32` followed directly by a
`load` - no store anywhere - so the value read was whatever the frame held; POST emits
`store i32 0`. This matters because the PRE probe for `double d = {}` / `bool b = {}` PRINTED
`0.000000` / `0` while emitting no store at all - an incidental stack read that a value-only check
would have scored as correct. `S s = {}` was already correct pre-fix (a null `right` fell through to
the struct default-construction fallback), and its `@main` IR is byte-identical PRE and POST; only
primitives, and `function<>`, were unseeded.

**Half 2 - a POINTER `{}` is REJECTED, in every spelling.** The reason is AMBIGUITY, not the
memory-unsafety that motivated the non-empty pointer reject in `fix/ptr-fieldinit`: on a pointer
target `{}` reads either as the null pointer or as a pointer to an empty object. Both are things a
reader could reasonably expect; zero-init happens to produce null, so silently choosing the first
reading would LOOK correct while quietly teaching the second one wrong. `nullptr` is the unambiguous
spelling and is what the diagnostic names. This is also the scope test for any spelling the rule
does not obviously cover: ask whether the target admits two readings. A non-pointer `{}` admits one
(`S s = {}` is the default-constructed `S`), which is why half 1 seeds rather than rejects.

The guard is ONE site in the declarator, placed before the fixed-array and array-view branches (both
of which `continue`), keyed on `Pointer && !IsArrayView`, plus the parameter-default arm.
It role-names through `LogEmptyBraceOnPointerReject`, the empty-`{}` companion to
`LogPointerBraceInitReject`, so a test can prove WHICH site fired and cannot be satisfied by the
non-empty message. Covered by one guard, measured individually: `S* p = {}`, `S* p {}`, global
`S* gp = {}` and `gp {}`, `S*[2] a = {}` and `a {}` (local and global), `int*`/`char*`/`void*`/`S**`,
`unique S*`, nullable `S?`, `simd<float,4>*`, `list<int>*`, namespace-qualified `N.S*`, a generic
`Box<int>*`, and a `using PS = S*` alias.

**This REVERSES `fix/ptr-fieldinit`'s ratified `S*[N] a = {}` row** (see the strikethrough above).
That record had ratified the spelling as ZERO-INIT, replacing a garbage memcpy of the pointee
defaults (`0x7` -> `0x0`); it is now a rejection. Three of the four pointer spellings compiled and
produced null CORRECTLY before this change - bare, global, array - so this is a real behaviour
change on working programs, not a no-op. The only in-repo fallout was
`Test/test_initializer_list.cb`'s `PtrSeed*[2] pa = {}` legs, replaced in the same commit.

**Accept-set (built BEFORE the guard, all measured PRE and POST, all unchanged).** `S s = {}` /
`S s {}`; `S s = {a=1,b=2}` and its bare twin; `S[3] = {a=5}`, `S[3] = {}`, `S[3] {}`; `int[3] = {}`
and `{}` bare; `int[] v = {1,2,3}` - an array VIEW, which IS Pointer-flagged and is exactly what a
naive `Pointer` test false-rejects, hence the `!IsArrayView` term; `int[] v = {}` and `S*[] v = {}`
keep their own "cannot infer the length" message; `list`/`array`/`dictionary` brace init and their
empty `= {}` / `{}` forms; `string`, `char* c = "hi"`, `void* p = nullptr`, `function<>` by name, a
NON-pointer `simd<float,4>` local, an interface local, `static`/namespaced/union/class/generic/aliased struct targets;
and all five non-empty pointer rejects from `fix/ptr-fieldinit`, whose messages are byte-identical.

**Intended collateral: a generic body's validity can now depend on its type argument.** Round-1
review found this and the maintainer ruled on it:

```cflat
struct Holder<T> { T v = default; void seed() { T x = {}; v = x; } };
```

`Holder<int>` compiles and prints 0 on both binaries; `Holder<S*>` printed 0 on master and now
hard-errors. `ParseDeclarationSpecifiers` sets `Pointer` for a T substituted to a pointer exactly as
it does for a written `S*`, and the guard cannot tell them apart. **The rejection STAYS** - the
ruling was "try `= default` first, exempt only if that does not work", and `T x = default;` was
measured rc 0 and identical PRE/POST across `int`, `double`, `bool`, `S*`, `char*`, `S` (prints 79,
so field defaults genuinely ran), `string`, `Box<int>` and `Box<int>*`. So the remedy works and no
substituted-generic exemption was added. This is a real behaviour change on working code that the
original ruling did not enumerate - the accept-set had `Box<int>*`, a pointer TO a generic, not a T
substituted TO a pointer - so it is recorded here rather than left to be discovered.

It did change the WORDING. The first cut named only `nullptr`, which is bad guidance inside a
generic body: for `T=int` it is nonsense, and it only compiles by coercion. `= default` is now named
FIRST and unconditionally, since it is the one remedy valid at every site and every instantiation.
The written-`*` case is NOT distinguished from the substituted one: it would be cheap at the
declarator (`declSpec->pointer()`) and is not available at the parameter-default arm, and having the
two positions word the same construct differently is exactly the divergence that caused the finding
below. Naming both remedies everywhere was preferred over a new type-flag. `Test/test_initializer_list.cb`
carries `T x = default;` instantiated at BOTH a pointer and a non-pointer T, so the guidance is
proven, not asserted; `Test/errors/err_ptr_brace_init.cb` carries the rejection.

**The two gates diverged again - in the commit that exists to unify them.** The declarator guard
carried `!IsArrayView`; the parameter-default arm tested bare `Pointer`. So
`int f(int[] v = {})` said "AMBIGUOUS on the POINTER parameter default for 'v' ... write 'nullptr'
... 'new int()'" - `int[]` is not a pointer, `nullptr` is not a legal view spelling, and the type
printed was `int` rather than `int[]`. The parameter arm now carries the same exemption and gives
the declarator's own "cannot infer the length of 'int[]'" message. The NON-empty view default
(`int f(int[] v = {1,2})`) had inherited the same false pointer wording from `fix/ptr-fieldinit`
(measured identical PRE/POST, so pre-existing); it now gets an honest message naming backing
storage. Both positions are pinned by legs.

**The remedy text is built from the DECLARED type, not the pointee name.** `LogEmptyBraceOnPointerReject`
and its sibling `LogPointerBraceInitReject` both formatted from `TypeName` alone, so `void* p = {}`
suggested `new void()` and `S** pp = {}` suggested `new S()`. Both helpers now take a rendered type
text (`DescribePointerDeclType`: `S*`, `S**`, `void*`, `S*[2]`, `simd<float,4>*`) and a
`CanSuggestAllocation` flag that withholds the `new T()` hint unless it is meaningful - a single
star over a known struct. The non-empty helper says "assign an address to it instead" when it is
not. The inherited defect is fixed in both, which is why five `fix/ptr-fieldinit` legs saw their
message TAIL change; all five pin the role phrase, which is unchanged, and all nine of that
commit's legs were re-mutation-tested here. **Not fixed: a type ALIAS loses its identity** -
`using PS = S*; PS p = {}` names `S*`, because `ParseDeclarationSpecifiers` resolves the alias
before the guard ever runs and recovering the written spelling means carrying raw declSpec text.
`= default` is spelling-agnostic and is named first, so the guidance is still correct for an alias.

**A behaviour change worth naming: `function<int(int)> f = {}`.** Function-pointer locals are not
`Pointer`-flagged on this path, so they take half 1 rather than half 2: PRE the `=` spelling read an
undef non-null value while the bare `f {}` gave null, and POST both give null. The two spellings now
agree, which is the whole point of half 1.

**Out of scope, with the reason.** A struct FIELD default (`struct H { S* p = {}; };`) discards the
brace list entirely for POINTER and NON-POINTER fields alike - `S s = {a=1}` in a field yields the
field defaults, not `1` - which is the already-filed
[[struct-field-default-brace-list-discarded]], a different path. `new T{}` and `f({})` are PARSE
errors (both grammar rules require a non-empty `initializerList`), so the `new` and named-argument
`EmitFieldInitializer` callers cannot see an empty brace at all; confirmed from `CFlat.g4`, not
inferred. A non-pointer `{}` on a GLOBAL struct zero-fills rather than running field defaults
(`S gs = {}` gives `0 0`, not `7 9`) identically PRE and POST - a pre-existing global-constant
limitation, untouched here.

No new `TypeAndValue` / `StructData` / `AnnotationValue` field: the guard reads `Pointer`,
`IsArrayView` and `ConstArraySize`, all three already in the `--init` round-trip
(`LLVMBackend.cpp`), so the errors file fires cold and warm. No new transient state, so nothing to
clear in `ResetForReanalysis`. Neither edit is in `ForwardRefScanner` (which only stores the
initializer context, never interprets it) and no type parsing changed, so the both-copies rule does
not apply.

**A hole this fix opened and closed in the same commit, worth the record.** The guard's first cut
exempted `IsSimd` alongside `IsArrayView`. `simd<T,N>` sets `Pointer` from its own `*` like any
other type, so that did not merely miss a rejection: `simd<float,4>* sp = {};` then fell into half
1's seeding arm, which stored a whole VECTOR into an 8-byte pointer slot and ABORTED the compiler
(rc 138, zero output) - a shape that ran clean, if garbage, on master. `IsArrayView` is the ONLY
exemption, and it is needed only because `int[] v = {1,2,3}` is Pointer-flagged; views never reach
the seeding arm anyway (their branch `continue`s above it). `Test/errors/err_ptr_brace_init.cb`
carries the simd leg as the tripwire.

Chasing that down surfaced a PRE-EXISTING abort, filed as
[[simd-pointer-declaration-aborts-the-compiler]]: `simd<float,4>* sp = nullptr;` AND
`... = default;` are both rc 138 on `5a6580c` and on this commit alike - so `simd<T,N>*` has no
initialized declaration spelling that compiles. (The `= default` face was found by mutation-testing
the simd leg, after the issue file had already been written claiming only `nullptr`; the file
records the correction.) It matters here because those are exactly the two remedies this fix's
diagnostic names, so for that one type neither suggestion compiles. Not fixed here - different root
cause, in the type's slot computation rather than in brace handling. (That issue is now CLOSED by
`fix/simdptr` below, and its file is deleted; the reading above was right about where the root
cause lived and wrong about which site - see that record.)

Bar (measured after rebasing onto `fix/mdview`, so the compiler carries both changes):
`./test.sh Release` 576 passed / 0 failed / 8 skipped, unchanged from master's own 576/0/8 -
the suite counts FILES and this commit adds none, it extends `Test/test_initializer_list.cb` and
`Test/errors/err_ptr_brace_init.cb`. `bash example_mac.sh Release` 35 passed / 0 failed. All
`expect_error` legs in `Test/errors/err_ptr_brace_init.cb` - the 13 added here AND the 9 inherited
from `fix/ptr-fieldinit`, whose message tails this commit changed - mutation-tested individually,
22 for 22: replacing each brace form with a legal spelling flips the file to rc 1 naming that leg.

### fix/ftell-long - `ftell`/`fseek` re-bound to C's `long` (RATIFIED)

Landed 2026-08-02 from a **Windows** host, which is the whole reason this one sat parked: the
defect does not exist on LP64, so a macOS or Linux session could neither reproduce nor verify it.

`core/cruntime.cb`, `core/os.windows.cb` and `core/os.posix.cb` all declared C's `ftell` as
returning `win_size` (pointer-sized) and `fseek` as taking a `win_size` offset. C spells both
`long`. On Windows/LLP64 that is a 32-bit type, so the CRT's `ftell` writes only `eax` and the
caller read whatever was left in the upper half of `rax`. All three decl sites now say `long`,
and `filesystem.cb:35` casts to `(long)` instead of `(win_size)`.

Three things worth carrying forward:

- **The fix is invisible in observed VALUES.** The garbage upper half is ABI-permitted but does
  not materialize on this UCRT - a pre-fix build returns exactly `4` for a 4-byte file, same as
  post-fix. The first version of the regression test asserted `ftell(f) == 4` and was therefore
  VACUOUS; it passed identically against a stale pre-fix core deployment. What discriminates is
  the DECLARED TYPE, so the test asserts `typeof(rawEnd) == "long"` instead
  (`Test/test_filesystem.cb`, `testFileInstanceMethods`). Verified non-vacuous by compiling
  against the not-yet-redeployed `x64/Debug/core`, where it fails with
  `expected 'long' got 'i64'`. General lesson: when a fix corrects a TYPE whose wrong value is
  merely permitted rather than guaranteed, assert the type.
- **The three-site decl duplication is unguarded.** Declaring the same extern twice with
  DIFFERENT types compiles clean with no diagnostic - the compiler keeps the first and silently
  drops the rest, so the winner is import order. Only cruntime's un-namespaced copy has callers
  today (`filesystem.cb`); nothing spells `os.ftell`. A future divergence in the namespaced
  copies would sit undetected until someone calls them.
- **`File` is still capped at 2 GB** on every platform - `_fs_ftell` narrows through `int` and
  the public `tell()`/`size()`/`seek()` are `int`. Deliberately NOT fixed here (it is a public
  API change needing `_ftelli64`/`_fseeki64` and `ftello`/`fseeko`); filed as
  [[file-offsets-capped-at-2gb]] at P2, which also carries the `extern i32 strlen` note.

Bar: `test.bat Release` all passed, `example.bat Release` 90 passed / 0 failed / 27 skipped.

### fix/alias-mangling - a pure-rename `using` alias folded at MONOMORPHIZATION (RATIFIED)

Closes `p2/generic-type-alias-arg-not-resolved` (deleted). `using MyInt = int;` then `list<MyInt>`
now binds a `list<int>*` parameter, and `Lambda<int(MyInt)>` encodes `__fatfn_1_3_i32_3_i32` - one
instantiation, as C++ gives a `typedef`. The alias was already resolved in every ordinary type
position; only the paths that mangle from raw source text saw it as an opaque token.

`MangleTypeArg` (`cflat/MainListener.h`) took a REQUIRED leading `const LLVMBackend*` - no default
value, so the C++ compiler flagged all 15 call sites and each was handled deliberately. All 15
reach a compiler; none passes `nullptr`. `BuildEncodedClosureName` threads the same pointer through
to reach the closure encoder (3 call sites). The fold runs BEFORE `CanonicalPrimitiveSpelling`, so
`MyInt` -> `int` -> `i32`, and chases an alias chain with an 8-hop guard.

Three things worth carrying forward:

- **The alias set is pre-registered, not accumulated.** This is the both-pass hazard, and it is
  real: `ScanGenericTypeUses` mangles every generic use in a file BEFORE `ScanExternalDeclaration`
  reaches the first `using`, while codegen sees the alias already registered - so a walk-populated
  map builds a shell under `list__MyInt` and looks it up as `list__i32`.
  `ForwardRefScanner::PreRegisterRenameAliases` sweeps file-scope `using` declarations at all three
  scanner drivers before either pass starts, into a DEDICATED map (`manglingAliases_`) that nothing
  else writes. `typeAliases` is deliberately NOT consulted by the mangler: it fills in
  progressively and at different points in the two passes, and it also holds generic aliases
  (`using IL = list<int>;` -> `list__i32`) whose targets look like bare names and would reintroduce
  the same split.
- **`if const` arms and function bodies are deliberately not swept.** `core/os.posix.cb` binds
  `win_size` to `i64` in one arm and `i32` in the other; a naive whole-tree sweep takes the last
  one and would fold a generic argument to the WRONG width. Those aliases stay opaque to the
  mangler, which is what both passes did before.
- **The new map is in the `--init` round-trip** (`mangling_aliases`, next to `type_aliases`). A
  warm cache never re-scans the core `.cb` files, so without it `list<win_int>` would mangle
  `list__i32` cold and `list__win_int` warm. Verified both ways.

Constraint kept: PURE RENAMES only. A target is folded only when it is a bare (possibly dotted)
identifier - `using Handle = void*;` and `using Vec3 = float[3];` store their suffix in the alias
string and are unfolded at `GetType`/`ParseDeclarationSpecifiers`, so folding them here would
double the mangler's own suffix walk. The measured `list<Handle>` baseline ("unknown function
'_data'" at `list.cb(212,54)`) is unchanged.

RATIFIED tightening: a program declaring both `f(list<MyInt>)` and `f(list<int>)` now collides as a
redefinition. One of them was silently unreachable. Measured on a repro; no test in the corpus hit
it.

Two ordering limits confirmed PRE-EXISTING and left alone (both reproduce without generics):
`NS.MyInt` never resolves - a `using` inside a namespace registers unqualified - and a chain
written out of order (`using A2 = A1;` above `using A1 = int;`) leaves `A2` unregistered in
`typeAliases`, because the scan declines an alias whose target is not yet a known type. The mangler
handled both correctly; the failure is downstream in `typeAliases`.

Regression legs added to `testGnCanonicalPrimitiveTypeArg` in `Test/test_generics.cb`: alias binds
`list<int>`, alias binds a function written BEFORE the `using`, alias-of-alias chain (list and
`CanonBox`), struct alias, and the closure-encoder form.

Bar: macOS arm64 Release `./test.sh` 576 passed / 0 failed / 8 skipped, `example_mac.sh` 35 passed
/ 0 failed, `test_lsp.sh` 152 passed / 0 failed. No test expectation was changed - the anticipated
`expect_error`-on-old-mangled-name fallout did not materialize.

### fix/funcptr-close - the last two funcptr-signature items, and the residue split out (RATIFIED)

Closes `p1/funcptr-overload-binding-ignores-signature` (deleted after four narrowings). The two
items it had left both diagnose now instead of running:

- Item 3, `runGlobal(NS.touchNs)` through a bare-`Pt` slot: was `neigh=2333` exit 0, a 12-byte
  write into a 4-byte element clobbering two neighbours. Now "no overload of 'runGlobal' matches
  the given arguments"; the value-spelled form adds "parameter takes 'void(Pt*)' but the argument
  is 'void(NS.Pt*)'".
- Item 2, a candidate refuted on its signature rebinding onto a `void*` sibling: was `rb=999`
  exit 0. Now "no overload of 'lam' matches", with the per-candidate mechanism line.

**Item 3 is NOT a mangling change, and the deleted file's fix direction was wrong to say it was.**
That file called for qualified keys in `FuncPtrParam.TypeName` and scheduled it as Stage 2. But
that string feeds `BuildEncodedClosureName`, and the two passes must produce byte-identical
encoded names or a struct shell is built under one name and looked up under another. The resolved
key went into SEPARATE fields instead - `FuncPtrParam::ResolvedTypeKey` and
`TypeAndValue::FuncPtrReturnResolvedKey` - and no mangled name moved. `""` means NOT RECORDED, the
same convention `PointerDepth == 0` already uses one line above.

Three things worth carrying forward:

- **Narrowing is MEMBERSHIP-ONLY.** `FuncPtrComponentOf` collapses the candidate set to the
  recorded key only when the ABI-canon form of that key is ALREADY in the set. So a stale or wrong
  key can only collapse an ambiguity the compiler itself resolved; it can never invent a rejection.
  The ABI-canon hop is kept inside the narrowing, or `Box<int>` vs `Box<i32>` starts false-rejecting
  again - the regression that parked the previous branch for three review rounds.
- **Inside a namespace, a bare spelling the walk could NOT qualify records nothing.**
  `SigComponentResolvedKey` records only when `ResolveTypeArgBaseName` qualified the name, when the
  spelling was already dotted, or at global scope where a bare name is unambiguous. Recording the
  bare form inside a namespace would be a false rejection whenever the namespaced type is not
  registered yet; recording nothing just falls back to today's broad set.
- **The SCANNER copy is the load-bearing one, and the plan that said "codegen sites only" was
  wrong.** Filling only the three `MainListener` sites was a no-op for the repro: a call site reads
  the parameter signature the **ForwardRefScanner** registered into the function table, not the
  main pass's. Instrumentation caught it - the key arrived as `""`. Both copies are filled, which
  is what CLAUDE.md's both-pass rule required anyway. New fields are in the `--init` round-trip
  (`fprk` / `rk`), verified warm.

For item 2 the gate is on the ARGUMENT being code, not on the parameter being `void*`:
`ArgumentIsFunctionPointerish` is one predicate shared by the funcptr overload arm and the `void*`
leg so the two cannot drift, and it fires only at `FunctionPointerShapeOf(...) == 0`. **That shape
check is load-bearing.** A first cut keyed on "the argument carries function evidence" and rejected
`function<T>*` and `function<T>[N]` into a `void*` parameter - the ADDRESS of a slot and an array
of slots are plain DATA pointers, and both are programs master compiles and runs. Three value legs
pin them.

The accepting site was not where the deleted file implied, either. A `function<>` VALUE reaches
`ComputeOverloadFunction` with an EMPTY `TypeName` - the call-argument loop copies the signature
fields but leaves `TypeName`/`IsFunctionPointer` alone - so it never touched the
"any pointer converts to void*" leg at all; it fell to `CompareUpconvert`, which under opaque
pointers sees two identical `ptr`s. The gate is in that branch. `ArgumentIsFunctionPointerish` has
to accept a recorded SIGNATURE as evidence for the same reason.

DELIBERATELY NOT DONE, both recorded rather than left implicit:

- **Variadic is untouched.** A variadic candidate is selected without inspecting the arguments at
  all, C passes function pointers through `...` too, and there is no measured repro. Blocking it is
  a separate and much larger tightening. RETIRED 2026-08-03 by `fix/funcptr-rebind`: review round 1
  produced the measured repro (`lam(Rec*, ...)`, exit 138) and it is memory-unsafe. The tightening
  was neither separate nor large - it judges the DECLARED parameters only, so the `...` tail is
  untouched and `printf("%p", fn)` still binds.
- **A non-`void*` pointer sibling still absorbs a refuted candidate**, and that one is
  memory-unsafe: with an `int*` sibling it silently returns 888, with a `Rec*` sibling that writes
  through the parameter it is `exit 138`. Pre-existing and unchanged by this fix (the gate keys on
  `TypeName == "void"`). Split out as `funcptr-refuted-candidate-rebinds-onto-pointer-sibling` at
  P1 rather than folded into this change, because widening the gate to any pointee needs C interop
  and header-import paths measured first. CLOSED later the same day by `fix/funcptr-rebind`, which
  measured both - see its landed record at the bottom.

RATIFIED tightening: passing a `function<>`/`Lambda<>` VALUE to a `void*` parameter is now an
error; write a cast. No test or core library in the corpus depended on it.

`Test/test_function_ptr.cb` lost the leg that PINNED item 2 (`rebindProbe(wrongSig) == 999`) and
its "RECORDED GAP" comment block - the only assertion whose answer changed - and gained the three
must-keep-binding shapes. `Test/errors/err_data_pointer_to_closure_param.cb` gained the two reject
legs, each built as two same-arity overloads so the overload SCORER is the deciding path, and both
verified non-vacuous against a binary built from `a32e55e`.

Bar: macOS arm64 Release `./test.sh` 576 passed / 0 failed / 8 skipped cold and warm,
`example_mac.sh` 35 passed / 0 failed, `test_lsp.sh` 152 passed / 0 failed.

### fix/simdptr - a `simd<T,N>` slot that is NOT a vector no longer takes the splat (RATIFIED)

Closes `p1/simd-pointer-declaration-aborts-the-compiler` (deleted). `simd<float,4>* sp = nullptr;`
and `... = default;` aborted the compiler with rc 138 and zero output.

**The DESIGN question the issue file asked first - is `simd<T,N>*` a supported type - is answered
YES, on measured evidence, and answering it NO would have been a false rejection of working code.**
`internal/simd-type.md`'s "no pointer" line and a repo-wide grep finding zero uses both point the
other way, and both are the trap the lessons file names: a grep proves the SPELLING is unused, not
that the CAPABILITY is absent. On `904f026` a `simd<T,N>*` PARAMETER compiles, is callable with
`&v`, and `simd<float,4> t = *p;` inside the callee reads the right lanes; a GLOBAL, a STRUCT FIELD
and a UNION FIELD of that type all compile and run. `GetType` has carried an explicit
`simd<float,8>* lowers to <8 x float>*` arm since the type landed. Only the LOCAL declaration was
broken, so the fix is the issue file's second branch: pointer-ness wins over `IsSimd` where the
slot type is computed.

**Root cause CONFIRMED by stack, and the issue file's un-measured citation was half right.** It
guessed `ParseDeclarationSpecifiers` setting `Pointer` on the simd branch and "something downstream
asking `GetType` for the vector rather than the pointer". The real site asks `GetType` for the
POINTER (`allowPointer` defaults true) and then blindly `cast<FixedVectorType>`s it:
`MainListener.h` decl-init did `if (typeAndValue.IsSimd && !right->getType()->isVectorTy())
right = SplatToSimd(...)`, and `SplatToSimd` (`LLVMBackend.h`) opened with
`cast<FixedVectorType>(GetType(tv))`. A Debug breakpoint puts all five crashing spellings -
`= nullptr`, `= default`, `= &v`, `simd<T,N>**`, `static` - on that one frame.

Three changes, all narrow:

- **Decl-init splat gated on the SLOT** - `llvm::isa_and_nonnull<FixedVectorType>(GetType(tv))`.
  This is a ROUTING predicate, not a rejection: a slot that is not a vector simply stores the value
  as any other type would, so a shape nobody enumerated degrades to normal behaviour rather than to
  a false rejection.
- **`SplatToSimd` `dyn_cast` + `LogError` backstop.** Honestly a backstop - the only caller now
  proves the slot is a vector, so it is unreachable today. It is here because CLAUDE.md requires an
  LLVM assert to become a message, and it is deliberately NOT sold as the fix or given a test.
- **`RecordSimdPointerAndDims`, shared by BOTH `ParseDeclarationSpecifiers` copies.** The simd
  branch `break`s out of the specifier loop before the common tail that records pointer depth and
  array dims for every other type, so `simd<T,N>**` lost its second level and `simd<T,N>[N]`
  silently lost its DIMENSION - allocating one vector and turning `a[i]` into a LANE index.

**TWO traps recording that dimension sprang, both closed in the same commit. Both have the same
shape and it is the durable lesson here: when a change makes a field newly non-null, every guard
that READS that field has to be re-audited under the NEW conditions, not the old ones.**

- `CreateAssignment` turned `simd<float,4>[2] a; a[0] = 3.0f;` from a clean rejection (master reads
  it as a lane write) into a SILENT MISCOMPILE, casting the scalar to the vector slot as
  `bitcast (<1 x float> ... to <4 x float>)`, which writes the value into every EVEN lane and
  leaves the odd ones zero. A dimension fix that upgrades a hard error into a wrong value is worse
  than no fix. `CreateAssignment` now splats a scalar into vector storage the same way the
  declaration initializer does - which ALSO fixes a pre-existing silent miscompile the matrix
  turned up on the PLAIN type. **Measured on a `f463e7f` (pre-fix) Release build**, not inferred:
  `simd<float,4> r = default; r = 5.0f;` gives `5 0 5 0`; `simd<float,8>` gives `5 0 5 0 5 0 5 0`;
  `simd<int,4> i = default; i = 7;` gives `7 0 7 0`. The earlier record of this said "lane 0 only",
  which was wrong in three places and made every single-lane assertion look like a discriminator
  when it only discriminated by parity.
- The by-value fixed-array-RETURN rejection (`MainListener.h`) carried a `!returnType.IsSimd`
  carve-out that was harmless only because the simd branch never set `ArraySize`. With the
  dimension recorded it suppressed the rejection for exactly the shape that now needs it, and
  `simd<float,4>[2] f()` emitted an unlocated `ret ptr` verifier dump where `float[2] g()` gets a
  clean located message. The carve-out is REMOVED - it is now redundant for a bare `simd<T,N>`
  return, whose `ArraySize` is null anyway, and wrong for an array of them. The message renders the
  vector spelling, since the type's `TypeName` is only the lane type. Found by review, not by the
  fix's own site audit, which had classified that site from master's conditions.

Ratified behaviour, do not "fix" back:

- `simd<T,N>` array/pointer storage is REAL storage: `simd<float,4>[2]` is `[2 x <4 x float>]` and
  `a[i]` is an ELEMENT, not a lane. Lane indexing still applies to a bare `simd<T,N>` value, and
  `Test/errors/err_simd_lane_write.cb` still pins the lane-write rejection.
- A scalar assigned into vector storage SPLATS, at a declaration and at an assignment alike. Every
  lane gets the value; there is no partial (even-lane-only) store.
- A `simd<T,N>[N]` RETURN is a by-value fixed-array return and is rejected like any other. A bare
  `simd<T,N>` return stays legal and is how every simd-producing helper is written.
- **`simd<T,N>[N]` now occupies its full size, so STRUCT LAYOUT changes for anyone out of tree.**
  Measured: a struct with one `simd<float,4>[2]` field, and a global of that type, both go from
  `sizeof` 16 to 32 - master allocated a single vector. There are zero in-tree uses, so nothing in
  this repo moved, but a persisted or FFI-shared layout built against an older compiler will not
  match. This is the correct size; the old one was the dropped dimension.
- An empty `[]` on a simd type is deliberately left alone (a simd array view is unimplemented, and
  deducing one would change a shape that currently compiles). That, with three name-keyed positions
  that cannot spell the type at all and the pointer-to-array spellings that never reach the guard
  every other element type reaches, is filed as
  [[simd-type-spelling-unusable-outside-declarations]] at P2.
- Three diagnostic helpers now spell a simd element as `simd<float,4>` instead of the bare lane
  type: `DescribeAggregateStorageShape` (`LLVMBackend.h`), `DescribePointerShapedInterfaceSource`
  (`LLVMBackend.h`, its `**` / `[]` / `[N]` arms - the bare-simd arm is untouched) and
  `DescribeArrayShape` (`MainListener.h`). Wording only: measured pre/post, every plain-array and
  plain-pointer spelling renders byte-identically and every verdict (rc) is unchanged.
- TWO `!IsSimd` guard exclusions (`MainListener.h`, whole-array assignment and global fixed-array
  init) became REACHABLE when the dimension started being recorded, and are DELIBERATELY left
  alone. A second guard catches the shape either way, so both spellings are still rejected - only
  the wording differs. Widening a rejection predicate for a wording benefit is not worth an
  accept-set exercise here; filed as
  [[simd-array-error-wording-differs-from-plain-arrays]] at P3, with both measured repros.

No new `TypeAndValue` field: the change sets `ElemPointer` and `ConstArraySize`, both already in
BOTH `--init` serializers (`ep`/`arr`). No new transient state, nothing to clear in
`ResetForReanalysis`.

One more edit worth naming, since it is a behaviour change and not a comment: the splat threads
`srcIsUnsigned` into the element conversion at **all THREE splat sites**, which the `else` arm
`CreateAssignment` bypasses was already doing. Only the WIDENING step takes the flag; the narrowing
/ int-float leg keeps its long-standing default so threading it cannot change an unrelated cast.

- `CreateAssignment`'s splat arm (`LLVMBackend.h`) - assignment.
- `SplatToSimd(scalar, tv, srcIsUnsigned)` (`LLVMBackend.h`), passed from the decl-init caller in
  `MainListener.h` - declaration initializer.
- `CreateVectorOperation`'s `splat` lambda (`LLVMBackend.h`) - a scalar operand of a vector
  operator. Its single `isUnsigned` parameter became a PER-OPERAND pair
  (`leftIsUnsigned`, `rightIsUnsigned`) so a splatted scalar widens with its OWN signedness;
  comparison signedness still uses `leftIsUnsigned || rightIsUnsigned`, exactly as before.

The first cut of this commit threaded the flag at the assignment site ONLY, which made three
shipped statements false. Measured on that binary and on this one, `simd<i64,2>` lane 1:

| source            | site      | before | after      |
|-------------------|-----------|--------|------------|
| `u32 4000000000`  | assign    | 4000000000 | 4000000000 |
| `u32 4000000000`  | decl-init | -294967296 | 4000000000 |
| `u32 4000000000`  | vector-op | -294967296 | 4000000000 |
| `u16 60000`       | decl-init | -5536      | 60000      |
| `u16 60000`       | vector-op | -5536      | 60000      |
| `u8 200`          | decl-init | -56        | 200        |
| `u8 200`          | vector-op | -56        | 200        |
| `i32 -5`          | all three | -5         | -5         |

The signed row is the must-not-break half: only genuinely unsigned sources zero-extend.
`Test/test_hpc.cb` section 11 freezes all ten cells as value legs; the six unsigned decl-init /
vector-op legs fail on the assignment-only binary and pass here.

Bar: macOS arm64 Release `./test.sh Release` 576 passed / 0 failed / 8 skipped, `bash
example_mac.sh Release` 35 passed / 0 failed, `bash test_lsp.sh Release` 152 passed / 0 failed. The
suite counts FILES and this commit adds none: it extends `Test/test_hpc.cb` with
`testSimdPointerStorage` (33 value legs, 259 -> 292 asserts in that file),
`Test/errors/err_simd_lane_write.cb` with the whole-array-assign leg, and
`Test/errors/err_fixed_array_byval_return.cb` with the simd-array-return leg. Differential `--check`
sweep of a `904f026` build against this one over all 523 `.cb` files under `Test/`, `example/` and
`cflat/core/`: the only real differences are the three touched test files; every other apparent diff
is the compiler's own worktree path echoed inside an "imported file not found: windows.h" message.
That sweep predates the amend that threaded `srcIsUnsigned` at the remaining two splat sites and
taught the two sibling describe helpers the simd spelling. Those were verified by targeted pre/post
pairs against a build of the pre-amend commit (every plain-array and plain-pointer spelling
byte-identical, every rc identical) plus a full green bar, NOT by a second whole-corpus sweep.

**How to read the 33 `test_hpc.cb` legs, stated honestly because a review round had to correct the
first version of this claim.** On a pre-fix binary that FILE does not compile at all, so the
pointer legs share ONE failure and none of them is an independent pre-fix discriminator - they are
forward tripwires, not twenty separate proofs. Two specific corrections the review forced, both now
in the file's comments: the `simd ptr passed to callee` leg was described as covering the parameter
path, which was never broken (its pre-fix failure comes from the local declaration above it); and
the first cut of the scalar-reassign pair asserted lane 0, which the pre-fix miscompile wrote
CORRECTLY (it writes every EVEN lane) - that leg passed with the `CreateAssignment` splat reverted.
The ALL-LANE SUM is now the discriminator, precisely because it does not depend on which lanes the
bad store happened to hit; the lane-3 leg is kept as a supplementary per-lane check. Every `== 0`
pointer leg is now followed by a round-trip through a real address, so a splat-into-the-slot could
not satisfy it. The legs that ARE independently discriminating against `904f026`, extracted and
mutation-tested standalone: the four array legs (hard-error "simd<T,N> lane write 'v[i] = ...' is
not supported" - the dropped dimension made `a[0]` a lane) and the reassign pair (measured
`5 0 5 0`, so the sum leg reads 10 against the expected 20 and the lane-3 leg reads 0 against 5).
The six section-11 unsigned-splat legs discriminate against the assignment-only first cut of this
same commit rather than against `904f026`. Both `err_simd_lane_write.cb` legs and the new `mk4` return leg were mutation-tested
individually and each flips its file to rc 1.

---

## Landed: `fix/funcptr-rebind` (2026-08-03) - a code VALUE no longer converts to ANY data parameter

Closes `funcptr-refuted-candidate-rebinds-onto-pointer-sibling` (P1), the last residue of the
funcptr family's item 2, and DELETES the file. The `void*` gate landed by `fix/funcptr-close` was
keyed on the pointee NAME (`candidateParamItr->TypeName == "void"`), so a candidate refuted on its
signature simply rebound onto the next pointer sibling instead. The gate is now on the ARGUMENT
being code and covers every data parameter the branch can reach.

Two predicates, shared by every reader so they cannot drift (`cflat/LLVMBackend.h`):

    bool ArgumentIsCodeValue(const NamedVariable& arg) const;   // shape 0 + function-pointerish
    bool ParameterStoresData(const TypeAndValue& param) const;  // pointer or `string`, not code
    bool ParameterAcceptsCodeValue(const TypeAndValue& p) const; // the argument side's code shapes

Read at three sites - two in `ComputeOverloadFunction`'s empty-TypeName branch, one in the variadic
short-circuit that precedes all per-argument scoring:

- the `CompareUpconvert` acceptance now rejects for ANY `candidateParamItr->Pointer`, not just
  `void`. The pointee is never itself a function-pointer type here: the funcptr arm above claims
  every such parameter whenever the argument is code, which the "funcptr* sibling still binds"
  probe (`scratch/fpr_p_funcptrptrsib.cb`, 913 on both binaries) confirms empirically.
- the implicit `char*` -> `string` coercion, which is NOT a pointer parameter and so was reached by
  a different acceptance entirely. Verified broken from `--no-opt` IR, not from a probe value:
  `call %string @"_operator string_string_charPtr_"(ptr @_ro_double_double_)` - the callee's
  machine code read as a NUL-terminated buffer.

MEASURED, since the issue file required both before any widening:

- **C interop is clean.** The C binder maps a recognized callback parameter to `__c_fn_ptr` (its
  own arm) and an unrecognized one to `void*`; it never produces a non-`void` pointee for a
  callback. Surveyed on macOS SDK headers: `qsort`, `bsearch`, `signal`, `pthread_create`,
  `pthread_once`, `pthread_key_create`, `pthread_atfork` are all `__c_fn_ptr`. `qsort(..., cmpi)`
  and `signal(2, onSig)` compile and run identically on both binaries.
- **The mirror arm is untouched.** The funcptr parameter arm still accepts
  `arg.BaseType->isPointerTy()`; `data-pointer-into-thin-function-param-segfaults` and
  [[shape-mismatched-funcptr-arg-binds-silently]] are unaffected. A string LITERAL into a
  `function<int(int)>` overload set is still `exit 138` on both binaries - that is the mirror leg,
  not this one.

THE ORACLE HAD TWO HOLES, and the first attempt to describe one of them was wrong in a way worth
recording.

**Hole 1 - VARIADIC, and the serious one.** `ComputeOverloadFunction` takes a variadic candidate as
a fallback with NO per-argument scoring at all (`if (candidate.Variadic) { possibleResult = pair;
continue; }`), so neither the `void*` gate nor its widening ever ran for one. `lam(Rec*, ...)`
absorbed the code address exactly as the non-variadic sibling used to - exit 138, no diagnostic, on
master AND on the first cut of this fix, so the widening alone did NOT close the P1 it was written
for. `lam(void* p, ...)` printed 901 on both, so this was the oracle's own hole and the widening
copied it. The gate is now repeated in the variadic short-circuit, over the DECLARED parameters
only: an argument in the `...` tail has no parameter to disagree with, and C passes function
pointers through `...` routinely. `printf("%p", fn)` is a value leg. This also retires the
"Variadic is untouched - there is no measured repro" deferral recorded under `fix/funcptr-close`:
there is a measured repro now, and it is memory-unsafe.

The variadic arm asks a WIDER question than the two non-variadic sites, corrected by review round 2
after its first cut asked the same one. `ParameterStoresData` answers false for a non-pointer
scalar, so a code value into a DECLARED scalar of a variadic candidate - `lam(int n, ...)` - was
unjudged and reached the LLVM verifier as a fatal `Call parameter type does not match function
signature!` / `call i32 (i32, ...) @_lamS_i32_i32_(ptr %0)`, identical on `904f026` and on the
first cut here. That arm now drops when `ArgumentIsCodeValue(arg) && !ParameterAcceptsCodeValue(p)`,
which subsumes the pointer/`string` question and additionally judges scalars, so the repo rule that
a diagnosed LLVM-level failure becomes a proper compiler error is satisfied. The NON-variadic sites
are deliberately left on `ParameterStoresData`: the non-variadic twin `lam(int n)` already rejects
cleanly with the standard no-overload error, so there is nothing there to close.

`ParameterAcceptsCodeValue` must MIRROR the argument side's code-shape spellings, not be spelled
from `IsEncodedClosureType` alone - review round 3's one confirmed finding. That helper is a map
lookup of ENCODED closure names and does not contain the literal `__closure_fat_ptr`, which is
exactly what a MONOMORPHIZED generic parameter carries (`ArgumentIsFunctionPointerish` lists the
spelling explicitly; so does the reader at `LLVMBackend.h:12889`). Omitting it FALSE-REJECTED
`int useT<T>(T v, ...)` called with a `Lambda<int(int)>`, with a self-refuting message - the dump
printed `__closure_fat_ptr` for both the argument and the parameter. Measured: `65d9283` and the
round-1 branch binary both exit 0 with `a=42 b=42`; round 2's first cut exited 1. The round-1 gate
was harmless here only because `ParameterStoresData` also answered false for that TypeName, so
widening the question is what exposed the gap. The complement of the predicate is therefore
"pointer, string, or scalar", NOT "anything IsEncodedClosureType does not name".

**Hole 2 - a false rejection, in the C binder.** `regConst(cb)` for a header-declared
`int regConst(int (* const cb)(int));` is REJECTED on master and stays rejected here:
`MapCTypeToTypeAndValueImpl` detects a function pointer by the literal `"(*)"` substring, clang
spells that parameter `int (* const)(int)`, and the qualifier between `*` and `)` defeats the
probe, so it binds as `void*`. Filed as `p2/c-binder-misses-decorated-function-pointer-parameter`;
NOT fixed here, and the widening neither creates nor worsens it (an unparsed callback always lands
on `void*`, which the pre-existing gate already refused).

That P2 was first filed against `atexit(bye)` with `_Nonnull` named as the trigger, and **both were
wrong**: `_Nonnull` keeps `(*)` intact and binds correctly, and the `atexit(void* func)` candidate
that rejects that call is `cflat/core/cruntime.cb:584`, a hand-written prototype, not a binder
mis-parse. Two supporting measurements in that first filing were also false. The corrected file
records the withdrawals; the lesson is the one already in `internal/fix-issue-lessons.md` about
measuring per spelling instead of inferring, applied here to WHICH DECLARATION a candidate came
from - `--symbol` names the defining file and settles it in one command.

Diagnostics: the no-match dump printed two indistinguishable `ptr`s for this family, so a second
per-candidate line now names the absorbing sibling - "parameter 0 is a data type ('Rec*') and the
argument is a function-pointer or closure VALUE - code does not convert to a data pointer." The
new reject legs pin that line rather than the generic header, so they cannot pass on the
signature refutation that precedes it.

Two corrections to that loop from review round 2. Its condition is now PER-ARM, because the gate's
question is: the two non-variadic sites judge only an argument in their own empty-TypeName shape,
the variadic gate calls `ArgumentIsCodeValue` unconditionally, and applying the non-variadic shape
requirement to every candidate meant a fat `Lambda<T>` value into `lam(Rec*, ...)` - correctly
REJECTED by the variadic gate - got no explanation line at all. And the wording is per-SHAPE: the
"is a data type" sentence is false at the scalar cell the widened variadic arm also judges, so a
non-pointer parameter gets "parameter 0 has type 'int' and the argument is a function-pointer or
closure VALUE - code does not convert to a non-pointer type." A rejection's diagnostic has to be
true of the site it fires at.

DELIBERATELY NOT DONE, recorded rather than left implicit: the same code-value-into-data-pointer
conversion at DECLARATION-INIT (`Rec* r = w;`), at `return`, and at a field store is unfixed and
memory-unsafe (exit 138, no diagnostic, identical on both binaries). Filed as
`p1/code-value-into-data-pointer-outside-overload-resolution`. The scorer is one funnel with a
measured accept set; the store paths are several sites, and a rejection there is exactly the shape
that has repeatedly false-rejected working code here - it needs its own accept set built first.

`Test/errors/err_data_pointer_to_closure_param.cb` gained nine reject legs: struct-pointer
sibling, `int*` sibling, `string` sibling, a LONE data-pointer candidate with no funcptr overload
in the set at all (the proof the gate is not "the funcptr candidate lost"), the three variadic
shapes (sibling, lone, and the `void*` spelling that the previous gate never covered), plus the two
from round 2 - a code value into a variadic candidate's DECLARED SCALAR (`lam(int n, ...)`, an LLVM
verifier fatal on `904f026` and on round 1), and a fat `Lambda<T>` value into `lam(Rec*, ...)`,
which round 1 rejected with no explanation line. Each was mutation-tested in isolation. The round-2
pair discriminates against the ROUND-1 branch binary, not only against `904f026`: reverting the
variadic gate flips the scalar leg, reverting the diagnostic loop's per-arm condition flips the fat
leg, and neither reversion flips the other.
`Test/test_function_ptr.cb` gained fourteen must-still-bind value legs - the three `void*` shapes
repeated against a struct-pointer sibling, the matching-signature half for both new siblings, the
two `string` coercions, and five variadic ones (data pointer into the declared param, a matching
signature into a variadic funcptr param, and all three `...` tail argument shapes: a NAMED
function, a `function<>` VALUE, and a FAT `Lambda<T>` value, since the tail is what a gate applied
to the whole argument list would break) - all with identical values on both binaries, and each
mutation-checked to confirm it can go red. The round-2 tail pair was measured on the round-1 branch
binary and on the main checkout's `65d9283` Release binary: both compile and return 923 on both,
so they are ACCEPT-side freezes, not discriminators. The round-3 pair - a generic VARIADIC and its
non-variadic twin, each taking a fat `Lambda<>` and forwarding it to a `Lambda<>` callee - returns
42 on `65d9283` and on the round-1 binary and is a real DISCRIMINATOR against round 2's first cut,
which failed to compile the whole file.

An A/B `--check` sweep over 546 `.cb` files under `Test/`, `example/`, `cflat/core/`,
`performance/` and `vscode-extension/` differs on exactly one file, the new reject test. Seven
core files first read as differences and were CACHE artifacts (`--init-local` state, not the
change): with `CFLAT_CACHE_DIR` equalized all seven agree.

Bar: macOS arm64 Release `./test.sh` 576 passed / 0 failed / 8 skipped, `example_mac.sh`
35 passed / 0 failed.

## Landed: `fix/iface-ifconst` (2026-08-04) - name the guarding `if const` in the zero-implementor rebox error

Closes `iface-ifconst-base-clause-implementor` (P2) and retires the shelved-attempt record
`iface-ifconst-blame-attempt-shelved` (P3), whose essentials are preserved here. The 2026-07-27
attempt (8 review rounds, 9 defects) was revived, its one outstanding defect fixed, and merged.

**Semantics decision (ratified by landing):** a conversion to an interface whose only
implementors sit inside non-taken `if const` arms stays a HARD ERROR - the class genuinely does
not exist in this build - but the message now names the class and the guarding arm chain:
`the only class implementing it, 'X', is declared inside an 'if const (COND)' branch that is not
taken in this build`. Making the conversion legal (uncertainty) was considered and rejected: it
turns a compile error into a null-vtable segfault.

**Design (survived a hostile independent audit - do not re-derive):** at scan time every class
inside an `if const` subtree is recorded with the CHAIN of levels it sits under, outermost first.
As MainListener decides each arm it RETRACTS (arm taken - class live), PEELS one level (a nested
`if const` gets its own decision), or FORGETS outright (subtree walk abandoned by a fired
`expect_error` - a peel would leave a front level the walk never reached). Invariant: the front
of a surviving chain is an arm nobody was shown to take, so naming it is truthful. The registry
(`ifConstGuardedImpls_`) is diagnostic-only, never gates acceptance, and is never serialized
(warm-cache diagnostics are byte-identical, so the `--init` serializer rule does not bite).

**Decisions that must NOT be retried:**
- Do NOT propagate uncertainty up the interface inheritance chain - tried and reverted; one
  unrelated generic disables the impossible-conversion guard for a whole ancestor chain.
- Blame is SUPPRESSED (never uncertainty) for classes under a generic TEMPLATE body - members
  reconcile zero or N times and the peel is not idempotent.
- The last-component fallback of a qualified base spelling feeds SUPPRESSION only, never blame.
  This was the shelving defect (round 9): with the qualified spelling unregistered (its namespace
  declared only inside the untaken arm), blame resolved through the fallback and fabricated an
  implements-claim against an unrelated same-named file-scope interface. Regression:
  `err_iface_rebox_ifconst_unregistered_ns_iface.cb` (unregistered cell) alongside
  `err_iface_rebox_ifconst_unrelated_iface.cb` (registered cell). Rule of thumb, twice-proven:
  over-broad candidate sets are SAFE for suppression and FABRICATE CLAIMS for blame.
- The reachability argument to check any future change against: blame escapes suppression via a
  PARENT of a surplus candidate, because `FindIfConstGuardedImplementor` matches through
  `InterfaceInheritsFrom` while uncertainty deliberately does not propagate up.

**Product decision (q5 shape):** when an unqualified base spelling inside an arm resolves to a
GLOBAL interface, blame follows the resolver even though, were the arm live, the class would fail
`does not implement` checks. The resolver and the diagnostic agree, which is the most truth
available at scan time. Accepted as-is.

**Known minor residue (accepted):** the candidate list a blame resolves through is
alias-resolved twice (`ResolveInterfaceName` at record, `ResolveTypeAlias` at resolve) - benign
in plain CFlat, untested against `import package "*.h"` / WinMD where a second alias hop is
plausible; condition text truncation (120 bytes) can land mid-identifier. Class-site candidates
are built from the accumulated NAMESPACE-only scope (namespaces opened inside the arm included,
class names excluded): the resolver never walks class-name prefixes, so a class-rung candidate is
a name the class could never have registered against, and feeding it to blame fabricates a claim
(struct and namespace nesting share one dotted key space, so such a name CAN be a registered
interface). Regression: `err_iface_rebox_ifconst_class_scope_iface.cb`.

Verified: batch `--check` (multi-file, both orders) shows no stale-registry leakage across
`ResetForReanalysis`. Bar: macOS arm64 Release `./test.sh` 596 passed / 0 failed / 8 skipped,
`example_mac.sh` 35 passed / 0 failed. Review: two opus rounds; round 1 found the class-rung
scope defect (fixed as above), round 2 clean.
