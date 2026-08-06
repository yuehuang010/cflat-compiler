# Issue queue

The index for `internal/issue/`. Started as an interface-only tracker and is now the index for
everything here - several entries below say "filed here because it has no other queue", which
is why the family headings replaced the old interface-first framing.

Not an issue itself, and **the only non-issue file in this directory**. Each row points at the
file that owns the detail. When an issue is fixed its file is deleted (the repo convention);
delete its row here in the same change. `internal/issue/` holds ACTIVE items only.

Layout: every issue file lives in a subfolder - **[`p1/`](p1/)**, **[`p2/`](p2/)**,
**[`p3/`](p3/)** by fix priority (P1 highest), plus **[`ui/`](ui/)** for the separate UI / Win32 /
WinRT track, and `p1/` alone is split one level deeper into **[`p1/codegen/`](p1/codegen/)** (a
wrong program is accepted with no error) and **[`p1/crash/`](p1/crash/)** (no usable diagnostic
is produced at all), which gates no compiler work and is not ranked against them. This file is the only
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
REVERTED once (a receiver comparison by variable NAME) before being FIXED on the second attempt -
see its landed design record below, which keeps the three discriminators that cannot work.
## Resume point

**2026-08-04: P1-to-zero release campaign in progress.** Goal: zero open P1s ahead of a release.
The open P1s are being driven through the `fix-issue` workflow (worktree, opus fix agent with
coverage matrix, opus review loop, linear merge), roughly narrowest-first. **The count is 5 as of
this edit, and the campaign is honest about why it took four fixes to fall**: the first THREE
campaign fixes each had a review that split out a residue P1, so the table went 6 -> 6 -> 6 -> 6
rather than 6 -> 5 -> 4 -> 3. `temp-unique-field-into-borrow-slot-use-after-free` is FIXED and
deleted, and its round-1 review split out `temp-unique-field-escapes-through-unguarded-spellings`;
`delete-of-untracked-pointer-copy-not-diagnosed` is FIXED and deleted, and its round-1 review split
out `pointer-copy-propagates-no-ownership-fact` (the three sibling double frees its accept set
measured, plus a `?:` join spelling the reviewer found);
`code-value-into-data-pointer-outside-overload-resolution` is FIXED and deleted, and its round-1
review split out [[join-erases-code-value-evidence-at-every-gate]] (a `?:`/`??` join erases the
source evidence every code-value predicate reads, which defeats the already-landed ARGUMENT gate as
well as the store sites). **`unique-field-global-struct-self-assign-false-positive` is the FOURTH
campaign fix and the first whose review split out nothing, so the count now falls: 6 -> 5 as of
that edit.** `interface-field-self-assign-false-positive` is the FIFTH, also splitting out
nothing at P1 (its round-1 review filed ONE new item, at P3:
[[unique-field-to-field-interface-receiver-residues]]): **5 -> 4**. All five design records are
below. The count will move again when the parallel branches land, so re-derive it from
`ls internal/issue/p1/` rather than trusting this sentence. The runtime-index residue of
`unique-field-to-field-array-element-receiver` was then RE-RANKED P1 -> P3 2026-08-05 as a
deliberate deferral (**4 -> 3**; see its P3 row for the rationale and the explicit invitation to
override). [[join-erases-code-value-evidence-at-every-gate]] is the SIXTH campaign landing
(`fix/joinledger`, 2026-08-05) and is count-neutral again: its own neighbour audit filed the MIRROR
defect, [[join-defeats-the-closure-widen-gate]], so **3 -> 3**.
`pointer-copy-propagates-no-ownership-fact` is the SEVENTH (`fix/ptrcopy`, 2026-08-05, filed its
residues at P2/P3): **3 -> 2**. `temp-unique-field-escapes-through-unguarded-spellings` is the
EIGHTH (`fix/tempuniq`, 2026-08-05, plain-`T*` remainder filed at P2): **2 -> 1**.
`join-defeats-the-closure-widen-gate` - the mirror itself - is the NINTH (`fix/widengate`,
2026-08-05), the first to file NOTHING new: **1 -> 0. No open P1s remain.** Every P1 fixed or
filed by this campaign is closed; the six originally-open P1s and the three review-filed ones
are all resolved. Progress is tracked by row deletion here, as always.

**2026-08-05, after `f45c9ad` re-triaged the crash / codegen buckets to P1, the working set is the
`p1/crash/` and `p1/codegen/` files rather than that campaign's list.**
`fix/genfp-return` closes one of them ([[generic-funcptr-return-poisons-enclosing-return]], the
`%thinret` verifier dump) and files two P2s -
[[generic-function-cannot-be-forward-referenced]] and, from its own boundary audit,
[[nested-emission-clears-enclosing-alias-scope-registry]]. Re-derive the count from
`ls internal/issue/p1/*/*.md`; do not trust the paragraph above for it.

**Previous head: the P1 campaign.** macOS arm64 Release **576 / 0 / 8** plus `example_mac.sh`
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
`data-pointer-returned-as-closure-not-gated` (itself fixed and deleted 2026-08-06 by
`fix/return-gate` - landed record below), `shape-mismatched-funcptr-arg-binds-silently` (P2).
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
| **P1** | [`p1/`](p1/) | The compiler produces a WRONG PROGRAM, or dies with no usable diagnostic. Silent wrong values, miscompiles, SIGSEGV/abort, verifier failures, missed lifetime errors. Split into [`p1/codegen/`](p1/codegen/) (16) and `p1/crash/` (0 - the folder is EMPTY and therefore absent from disk, since git cannot track an empty directory; recreate it when the next row is filed). | 16 |
| **P2** | [`p2/`](p2/) | Legal code is REJECTED, a feature is unavailable, or an ownership guard has a hole that does not (yet) produce a wrong value. The program does not run, but nothing lies to you. | 39 |
| **P3** | [`p3/`](p3/) | Diagnostic quality, latent/no-repro, deliberate deferrals, and shelved attempts. Real, filed, and not blocking anyone. | 36 |
| **UI** | [`ui/`](ui/) | Separate track - UI / Win32 / WinRT parity. Gates no compiler work; not priority-ranked against the compiler buckets. | 7 |

Counts re-verified from disk on 2026-08-05 (`ls internal/issue/p1/*/*.md internal/issue/p{2,3}/*.md
internal/issue/ui/*.md | wc -l` per bucket) on the `fix/chain-coalesce` tree, which rebased onto
`fix/lamptr-generic`:
**17 P1 (15 codegen / 2 crash) / 38 P2 / 34 P3 / 7 UI = 96 total**. `fix/chain-coalesce` moved one
P1/crash out (fixed and deleted) and filed four new P2s. On the `fix/lamptr-generic` tree
immediately before that the same count read 18 P1 (15 / 3) / 34 P2 / 34 P3 / 7 UI = 93. The table
above had drifted
badly - it still read 21 P1 / 29 P2 / 32 P3, and the P1-crash figure of 6 was already wrong on
`4c06cce`, where disk held 4. Recount with `ls` before quoting these; the prose figures below are
a dated snapshot of an earlier round and are deliberately left as written.

**Recount 2026-08-05, after `fix/genfn-lowering` rebased onto `fix/chain-coalesce`** (`ls` per
bucket on that tree, counted not computed):
**17 P1 (16 codegen / 1 crash) / 37 P2 / 35 P3 / 7 UI = 96 total.** Three files deleted
(`generic-wrapper-over-function-type-llvm-fatal` and
`list-of-function-element-into-closure-param-fails-verifier` from `p1/crash/`,
`closure-by-value-into-generic-struct-field` from `p2/`) and three filed
([[data-pointer-assigned-to-thin-function-value]] in `p1/codegen/`,
[[sizeof-generic-over-closure-type-segfaults-compiler]] in `p1/crash/`,
[[inline-deref-of-container-call-result-has-no-storage]] in `p3/`).

**Recount 2026-08-06, after `fix/sizeof-closure`** (`ls` per bucket on that tree, counted not
computed): **16 P1 (16 codegen / 0 crash) / 39 P2 / 36 P3 / 7 UI = 98 total.** One file deleted
(`sizeof-generic-over-closure-type-segfaults-compiler` from `p1/crash/`, which EMPTIES that
bucket) and three filed: two in `p2/`
([[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]], the deliberately-left
feature gap behind that crash, and
[[zero-parameter-generic-function-emits-double-mangled-symbol]], an unrelated link failure found
on the same probe run), and one in `p3/`
([[sizeof-steals-discarded-tuple-comparison-spelling]], the measured cost of the fix's character
test, found by review round 1 - the fix agent's matrix had no comma-comparison cell).

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
new issues filed in its place - three P2 (`class-no-ctor-default-construct-returns-undef`,
[[struct-field-default-brace-list-discarded]] (since FIXED and deleted - see the `fix/field-brace`
landed record below), `interface-typed-global-brace-init-discarded` (since FIXED and deleted - see
the `fix/iface-global` landed record below))
and one P3 ([[global-struct-no-initializer-ignores-field-defaults]]). All four were found by
review of that P1's fix, not by the original investigation - the same pattern this file has
recorded before (see the 2026-08-01 paragraph above).

A further review round of the same fix filed a FIFTH: `pointer-decl-field-init-brace-corrupts-pointer-storage`
(P1 - `S* p {a=1};` writes a nonsense address into `p` itself). **FIXED and deleted 2026-08-02** by
`fix/ptr-fieldinit`; see the landed design record below. That fix in turn filed two more from its own
Phase A enumeration - `empty-brace-initializer-never-seeds-and-crashes-on-defaults` (P1, since
**FIXED and deleted** by `fix/emptybrace` / `b844137`; see the landed design record below) and
[[string-literal-containing-braces-retyped-as-string]] (filed P2, re-ranked P1 for its
miscompile face) - which is the same pattern again. **FIXED and deleted 2026-08-06** by
`fix/brace-literal`; see the landed design record below.
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

Counts re-verified from disk on 2026-08-04 AT THE START of the P1-to-zero release campaign,
BEFORE any campaign fix landed - history, superseded by the bucket table above:
**6 P1 / 43 P2 / 29 P3 / 7 UI = 85 total** (after that round's re-bucket). Both integrity checks
were re-run scriptably (extract `[[...]]` links per table section, diff against `ls` per bucket)
and FOUR failures were found and fixed in this edit - the worst tally since the checks were
introduced:

- `p1/delete-of-untracked-pointer-copy-not-diagnosed` had a file and no row (filed 2026-08-02).
- `p2/null-interface-access-remaining-storage-kinds`, `p2/c-binder-misses-decorated-function-pointer-parameter`
  and `p2/coalesce-assign-skips-store-bookkeeping` all had files and no rows.
- One P1 table row (the null-interface-access residue) described a FIXED item and resolved to no
  file; removed - its record lives in the plan file and the new P2 row.
- `interface-boxing-keyed-on-source-binding` re-bucketed P1 -> P3 per its own text (only the
  preventive `RegisterInterfaceBox` dedupe remainder is left).

Net movement: no fixes this edit - bookkeeping only, so the true open-P1 count entering the
campaign was **6**: `code-value-into-data-pointer-outside-overload-resolution`,
`delete-of-untracked-pointer-copy-not-diagnosed`, `interface-field-self-assign-false-positive`,
`temp-unique-field-into-borrow-slot-use-after-free`,
`unique-field-global-struct-self-assign-false-positive`, and the runtime-index residue of
`unique-field-to-field-array-element-receiver`. The first campaign fix has since landed
(`temp-unique-field-into-borrow-slot-use-after-free`), and its review split out one new P1
(`temp-unique-field-escapes-through-unguarded-spellings`) plus one new P2
(`lambda-body-owning-temp-never-destructed`), so the bucket table above read **6 P1 / 44 P2**.

**Recount 2026-08-04, after `delete-of-untracked-pointer-copy-not-diagnosed` landed:** its file is
deleted and its P1 row removed, and its round-1 review filed one new P1 in the same commit -
`pointer-copy-propagates-no-ownership-fact`, holding the four sibling double frees the fix's accept
set measured and deliberately left alone (a copy stored into a `unique` FIELD, a one-hop copy of a
CONTAINER-ELEMENT borrow, `T* d = move b;` off a copy, and a `?:` join into a pointer declaration -
all four identical on `312d202` and on the merged fix). ONE OUT, ONE IN, so the bucket table still
reads **6 P1 / 44 P2 / 29 P3 / 7 UI = 86 total** - the same net pattern as the codeval branch. They
were filed rather than parked in the design record below because that section's own heading says
"Nothing here is open", and three live silent double frees do not belong under it.

**Amended later on 2026-08-04 by `fix/codeval-store`.** That change fixed and DELETED
`code-value-into-data-pointer-outside-overload-resolution` and filed
[[join-erases-code-value-evidence-at-every-gate]] in its place, so the P1 count is unchanged at
**6** - one file out, one file in, the same count-neutral shape `fix/funcptr-rebind` had, and the
same reason it is spelled out rather than left to a bare recount. The new file is NOT a residue of
the old one: the store gate it replaces was a missing DESTINATION reader, and the new one is
erased SOURCE evidence that defeats the already-landed ARGUMENT gate equally.

So BOTH campaign fixes that ran in parallel worktrees have now landed, and both were P1
count-neutral for the same reason - each had a residue split out of it by its round-1 review. The
bucket table above therefore stays at **6 P1 / 44 P2** (the 43 -> 44 move is `fix/temp-uniq-borrow`
adding `lambda-body-owning-temp-never-destructed`; this change adds no P2). Two P1s fixed, two P1s
filed, net zero - which is the pattern this file has now recorded four times, and the reason the
campaign is tracked by row deletion rather than by the count.

**Recount 2026-08-04, after `unique-field-global-struct-self-assign-false-positive` landed:** its
file is deleted and its P1 row removed, and its round-1 review filed NOTHING new - the FOURTH
campaign fix, and the first that actually moves the count: **5 P1 / 44 P2 / 29 P3 / 7 UI = 85
total**, so the net-zero pattern above is broken rather than repeated a fifth time. Both integrity
checks were re-run scriptably against this tree (every row resolves to a file, every file has a
row, per bucket) and are clean. See the landed design record below for why the file's preferred
step 1 (give a global receiver a `NamedVariable` identity) was NOT taken - its premise was
measurably wrong - and for the one other behaviour change the change carries.

**Recount 2026-08-05, after `interface-field-self-assign-false-positive` landed:** its file is
deleted and its P1 row removed - the FIFTH campaign fix, and the second in a row to move the P1
count: **4 P1 / 44 P2 / 30 P3 / 7 UI = 85 total**. The round-1 review filed ONE new item, at P3
not P1: [[unique-field-to-field-interface-receiver-residues]], consolidating the fix's own
deliberate-deferral residues on disk instead of leaving them only in a review report. Both
integrity checks were re-run against this tree (every row resolves to a file, every file has a
row, per bucket) and are clean. The landed design record below states why the receiver identity is taken
from the BOXED OBJECT and why the verdict is deferred to end of body.

**Recount 2026-08-05, after the array-element runtime-index RE-RANK:**
`unique-field-to-field-array-element-receiver` moved P1 -> P3 as a deliberate deferral (file moved
to `p3/`, name unchanged so links resolve; rationale in its P3 row and at the top of the file):
**3 P1 / 44 P2 / 31 P3 / 7 UI = 85 total**. This is a bucketing change, not a fix - no compiler
edit, no test change. Both integrity checks re-run against this tree, clean. The three P1s open AT
THAT MOMENT were all campaign-review discoveries: [[pointer-copy-propagates-no-ownership-fact]],
`join-erases-code-value-evidence-at-every-gate` and
`temp-unique-field-escapes-through-unguarded-spellings`. **Superseded by the recounts below** -
every P1 named here has since been fixed and its file deleted; read the FINAL recount in this
section for the live list.

**Recount 2026-08-05, after `fix/joinledger` landed:**
[[join-erases-code-value-evidence-at-every-gate]] is FIXED and its file DELETED, and the fix's own
neighbour audit filed [[join-defeats-the-closure-widen-gate]] in its place - P1 unchanged. Review
round 2 additionally promoted the fix's same-statement launder residual from a landed-record
footnote to its own P2 file, [[same-statement-cast-launders-join-code-evidence]]:
**3 P1 / 45 P2 / 31 P3 / 7 UI = 86 total**. Both integrity checks re-run against
this tree (every row resolves to a file, every file has a row, per bucket), clean.

The new file is the MIRROR of the one it replaces, not a residue of it: the fixed issue is a join
erasing the evidence that a value IS code, and the new one is a join erasing the evidence that a
value is NOT code, on the closure-widen gate. They need opposite ledgers and opposite quantifiers
("any arm is code" versus "every arm is data"), which is why it was filed rather than folded in -
the second guard would have been written past an already-frozen accept set. The landed design
record is at the bottom of this file.

**Recount 2026-08-05, after `pointer-copy-propagates-no-ownership-fact` landed:** its file is
deleted and its P1 row removed - the SIXTH campaign fix. All FOUR of its filed repros now reject
with a diagnostic. The change is NOT count-neutral in the usual direction: it files THREE new items,
none at P1 - two P2 ([[move-of-borrowed-pointer-adopts-into-plain-destination]],
[[coalesce-join-null-local-arm-erases-owner-proof]]) and one P3
([[implied-move-store-boxed-spelling-false-rejects]]). Counted from disk per bucket, on the tree
rebased over the merged `fix/joinledger`:
**2 P1 / 47 P2 / 32 P3 / 7 UI = 88 total.** Both integrity checks were re-run against this tree
(every row resolves to a file, every file has a row, per bucket) and are clean.

The P3 is the notable one, and it was found by the fix's own ACCEPT SET rather than by a review: an
intermediate cut of this change read `InheritedKeepsOwner` in the raw-`delete` guard, which
false-rejected `Ci* p = nullptr; p = c; delete p;` - a program master runs at one free, because that
store is an IMPLIED MOVE. The accept leg caught it before the guard shipped; the pre-existing BOXED
half of the same mis-blame is what got filed. That is the "build the accept set FIRST" rule paying
for itself inside a single round, and it is why the join proof is a separate field.

**Recount 2026-08-05, after `fix/tempuniq` landed:**
`temp-unique-field-escapes-through-unguarded-spellings` is FIXED and its file DELETED - the
SEVENTH campaign fix, and it takes the P1 count to ONE. It files TWO new items, neither at P1:
its own declared remainder ([[temp-unique-field-escapes-through-a-plain-pointer-parameter]]) and
the LEAK its accept set uncovered ([[owning-temp-in-coalesce-fallback-arm-never-destructed]]).
Counted from disk per bucket:
**1 P1 / 49 P2 / 32 P3 / 7 UI = 89 total.** Both integrity checks were re-run against this tree
(every row resolves to a file, every file has a row, per bucket) and are clean.

The second filing is the one worth reading. The fix's own arm-position probe measured that a
`?:` arm really does dangle (`dtors=1`, both the true arm and a taken false arm) while the `??`
FALLBACK arm is never destructed at all (`dtors=0`, the read returns the LIVE value) - so an
"any arm" walk would have false-rejected `p ?? makeBox().t`, a program master runs correctly.
The arm walk excludes that one position and the leak behind it is filed with an explicit
instruction that fixing it must delete the exclusion in the same change.

The split is not a scope cut dressed up as a filing. The parent issue enumerated FIVE spellings
and named the plain-`T*` parameter as the undecidable one in its own text; four of the five are
closed, and the remainder is filed with the accept cell that blocks the obvious wrong fix
(`rd(makeBox().t)`, a read-only plain parameter, is CORRECT code) already frozen as a value leg.
Separately, `fix/tempuniq` reduced [[coalesce-assign-skips-store-bookkeeping]] from a file of
predictions to one with a MEASURED memory-unsafe repro - it did not file a new issue for the
`??=` spelling, because the root is that issue's, not this one's.

The one remaining P1 at that point was a campaign-review discovery:
[[join-defeats-the-closure-widen-gate]]. **Superseded by the recount below.**

**Recount 2026-08-05, after `fix/widengate` landed:**
[[join-defeats-the-closure-widen-gate]] is FIXED and its file DELETED - the NINTH campaign
landing, and the first to file NOTHING new. The two shapes it measured outside its own domain were
both already-filed issues and were annotated in place rather than re-filed: the `list<Lambda<>>`
container axis (the `add` direction of
[[list-of-function-element-into-closure-param-fails-verifier]], identical on both binaries and not
join-related) and the RETURN path
([[data-pointer-returned-as-closure-not-gated]], whose bare spelling is equally unguarded, so it is
not a join residue). Counted from disk per bucket:
**0 P1 / 49 P2 / 32 P3 / 7 UI = 88 total. The P1 bucket is EMPTY.** Both integrity checks re-run
against this tree (every row resolves to a file, every file has a row, per bucket), clean.

**Recount 2026-08-05, after the cache-codegen investigation:** the standing claim that the `--init`
bitcode cache CHANGES CODEGEN - measured repeatedly by review agents as `leaks --atExit` drifting
by 2 leaks / 48 bytes on `Test/test_move.cb` between a cold and a warm cache - was investigated and
**REFUTED**. The bitcode cache is semantically pure: with the cache state as the only variable, a
10-test leak sweep is identical, and the only IR differences are LLVM value-name uniquing
(`%.unpack4` vs `%.unpack8`) plus global emission order. The real variable is the harvested `macsdk`
libSystem stub that `--init` writes into the same directory, which flips the `LC_BUILD_VERSION` sdk
stamp of every emitted binary; patching only those 4 bytes in an otherwise byte-identical
executable reproduces the entire leak delta. Filed as ONE new P2,
[[macos-sdk-stamp-differs-by-cache-state]] (bucket note in the file invites a re-rank to P3). No
compiler change, no fix, no test change - investigation only. Counted from disk per bucket:
**0 P1 / 50 P2 / 32 P3 / 7 UI = 89 total.** Both integrity checks re-run against this tree (every
row resolves to a file, every file has a row, per bucket), clean. **Carry this forward:** leak
counts are NOT comparable across cache states, so any future ownership measurement must fix the
cache state across the binaries it compares, or it will re-derive this same false alarm.

### P1 - wrong programs and crashes (`p1/`)

Re-populated 2026-08-05 by a P2 -> P1 retriage on the maintainer's rubric: **any compiler crash is
P1, and any bad codegen accepted without an error is P1**. That rubric overrides the
residue-not-regression precedent, which is what had kept the silent-double-free and
memory-unsafe-accept rows in `p2/` - several of those files say in as many words "re-rank to P1 if
the memory-unsafe-accept rubric wins". It does. `p1/` is split into two subfolders because the two
halves of the rubric want different fixes: `codegen/` lies to you and runs, `crash/` refuses to
produce a program.

#### P1 / codegen - accepted with no error, wrong program (`p1/codegen/`)

| Issue | Family | Severity |
|---|---|---|
| [[string-literal-containing-braces-retyped-as-string]] | miscompile + false rejection | A string literal whose CONTENT contains a brace pair (`"a = {} b"`) is typed `string` instead of `char*`. At a call it stops every `char*` overload matching and the diagnostic blames the call; at a VARIADIC it is a SILENT MISCOMPILE - `printf("a = {} b\n");` compiles and runs rc 0 printing binary garbage, and the dedicated `cannot pass 'string' to the variadic '...'` guard does not fire. Identical on both binaries. Filed 2026-08-02 in `p2/` for the rejection face; the miscompile face may warrant P1. |
| [[extern-decl-drops-fixed-array-return-size]] | silent wrong ABI | `extern char[8] extmk();` compiles clean on BOTH `ca5a02a` and `fix/array-storage` - the `[8]` is dropped and the declaration binds to a symbol returning one `char`. The by-value fixed-array-return reject landed on the DEFINITION path only; this is the one remaining spelling of that axis. Not a regression. Filed 2026-08-02. |
| [[temp-unique-field-escapes-through-a-plain-pointer-parameter]] | memory-unsafe accept | Silent use-after-free, compiles clean and exits 0, identical on `14097e1` and on the merged `fix/tempuniq`. `keep(makeBox().t)` where `void keep(Node* n) { g = n; }` reads a freed block (proven by dtor count + reallocation aliasing; the `MallocScribble` fill shows only on ld64.lld-linked builds`. The DECLARED remainder of `temp-unique-field-escapes-through-unguarded-spellings` (closed and
| [[same-statement-cast-launders-join-code-evidence]] | memory-unsafe accept | Silent exit 138: `two((void*)ro, c ? ro : n)` - a data cast of a NAMED function anywhere in a statement launders every other mention of that function in the SAME statement, because the launder is keyed on `llvm::Value*` alone and a named function is one shared constant. Cross-statement and cross-function are closed (`fix/joinledger`); only the same-statement window remains. P2 under the residue-not-regression precedent ([[unique-field-to-field-interface-receiver-residues]]) - the spelling was accepted before the fix too; re-rank to P1 if the memory-unsafe-accept rubric wins. Fix direction: occurrence keying (value + syntactic cast site). Filed 2026-08-05 by review round 2 of `fix/joinledger`. |
| [[move-of-borrowed-pointer-adopts-into-plain-destination]] | ownership | Exit 134, no diagnostic, identical on `d93c359` and on the merged `fix/ptrcopy`. `move` of an `IsBorrowed` source is gated only on a `unique` DESTINATION, so `Ci* d = move p;` off a borrowed pointer PARAMETER (and its one-hop copy, and the `move`-returning-wrapper spelling) adopts ownership the borrow never had. `fix/ptrcopy` added the destination-agnostic move guard next to this and deliberately left `IsBorrowed` out of it: `MainListener.h` carries an explicit ratified policy that forwarding an ordinary borrow as `move` stays legal, so closing this means REOPENING that policy with its own accept set, not adding a clause. Filed 2026-08-05 by `fix/ptrcopy`. Silent double free, so P1 by the bare rubric; filed P2 under the residue-not-regression precedent (`unique-field-to-field-interface-receiver-residues`, `return-dangle-missed-when-slot-has-extra-user`) - accepted by the PRE binary too, so residue rather than regression. Re-rank to P1 if the maintainer rules the silent-double-free rubric wins. |
| [[alias-borrow-local-launder-gaps]] | ownership | An `IsAliasBorrow` owning-struct local launders its borrow through `=` and through `move`. |
| [[coalesce-join-null-local-arm-erases-owner-proof]] | ownership | Exit 134, no diagnostic, identical on `d93c359` and on the merged `fix/ptrcopy`. `Ci* n = nullptr; Ci* b = n ?? c; delete b;` and its BOXED twin both double-free. The BOTH-ARMS join rule skips a null LITERAL arm as neutral but counts a LOCAL that merely HOLDS null as a non-proving arm, so the whole join is dropped. The raw and boxed spellings AGREE here, so this is a pre-existing hole in the arm CLASSIFICATION, not an asymmetry `fix/ptrcopy` introduced - its `?:` twins reject on both binaries. Silent double free, so P1 by the bare rubric; filed P2 under the residue-not-regression precedent (`unique-field-to-field-interface-receiver-residues`, `return-dangle-missed-when-slot-has-extra-user`) - accepted by the PRE binary too, so residue rather than regression. Re-rank to P1 if the maintainer rules the silent-double-free rubric wins. Filed 2026-08-05 by `fix/ptrcopy`. |

#### P1 / crash - dies with no usable diagnostic (`p1/crash/`)

**This bucket is EMPTY as of 2026-08-06.** No rows. The folder itself is gone from disk because
git cannot track an empty directory - recreate `internal/issue/p1/crash/` when the next row is
filed, and restore the table header with it.

Three landings emptied it. `fix/chain-coalesce` fixed and deleted
`chained-nullcoalesce-not-boxed-into-interface`; `fix/genfn-lowering` (2026-08-05) fixed and
deleted BOTH `generic-wrapper-over-function-type-llvm-fatal` and
`list-of-function-element-into-closure-param-fails-verifier`, as well as the P2
`closure-by-value-into-generic-struct-field` that shared their root (see the landed record below -
the filed LLVM ISel fatal had already DRIFTED into a located hard error before that branch
started, and re-measuring found two spellings that were compiler SIGSEGVs, which the files did not
record). `fix/sizeof-closure` (2026-08-06) fixed and deleted the last one,
`sizeof-generic-over-closure-type-segfaults-compiler` - the residual FEATURE gap behind it is now
[[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]] in `p2/`, which is exactly
the "located diagnostic is the floor, the feature gap may stay open" split this bucket's
convention calls for.

The bucket's standing convention is unchanged by the emptying: an LLVM assert, fatal, or verifier
failure reachable from plain source belongs here whether or not it literally aborts, because a raw
verifier dump with no `file(line,col):` prefix is "dies with no usable diagnostic". A located
diagnostic is the floor for any such row, whether or not the underlying feature gap closes in the
same change.

Rows that did NOT move, and why - the rubric is about wrong values and crashes, so a LEAK stays
P2 ([[lambda-body-owning-temp-never-destructed]],
[[owning-temp-in-coalesce-fallback-arm-never-destructed]], [[unique-assign-syntactic-owned-rhs-leaks]]),
and so does a hazard with no witness ([[array-view-params-unconditionally-noalias]] - P1 the moment
one exists), a theoretical stale-ledger double free ([[detection-ledgers-not-discarded-on-aborted-arm]]),
and a `core/` API-width bug that is not codegen at all ([[file-offsets-capped-at-2gb]]).
[[delete-borrow-via-named-local]] and [[deref-of-moved-pointer-guard-inside-callee]] are genuine
memory-unsafe accepts held at P2 on cost, not on severity: the recorded fix attempt for the former
cost ~6 suite failures plus the core UI framework, and the latter needs cross-function analysis.

### P2 - false rejections, unavailable features, ownership holes (`p2/`)

| Issue | Family | Severity |
|---|---|---|
| [[conditional-store-retires-borrow-facts-unconditionally]] | ownership | rc 133 (double free), no diagnostic, identical on `152728c` and on the merged `fix/coalesce-tail`. The store tail's three RETIREMENTS (`ClearVariableBond`, `SetVariableBorrowsOwnedString`, `SetVariableBorrowsOwnedElement`) are walk-order, not control flow, so an `=` inside a branch that is NEVER TAKEN still clears the fact: `R* g = l.get(0); if (g == nullptr) { g = new R(); } delete g;` compiles and double-frees the list's element, while the same `delete g` without the `if` is a hard error. Split out of `coalesce-assign-skips-store-bookkeeping` - `fix/coalesce-tail` answered the `??=` spelling by taking the JOIN (conservative, matches master), which does NOT generalize: joining would false-reject the always-taken `if (c) { g = new R(); } delete g;`. Needs a real MAY/MUST fact, so it is a feature, not a clause. Filed 2026-08-06 by `fix/coalesce-tail`. |
| [[as-is-does-not-recognize-nullcoalesce-join]] | false rejection | `(z ?? a) as IShape` / `is IShape` gives "requires an interface value or a class instance ... this expression is neither". NOT a chaining defect - it reproduces at chain length 1, and the `?:` spelling of the same construct WORKS. `ClassifyCastSource` in `MainListener.h` recognizes a pointer join only as a `PHINode`, and a `??` joins through a slot so its result is a `LoadInst`. Arms are recoverable via `CollectPointerJoinArms`. Note `ResolveTernaryArmClasses` does `llvm::cast<llvm::PHINode>` and must move onto the same collector in the SAME change or it asserts on the newly-admitted load. Measured identical on `4c06cce` and the chain fix. Filed 2026-08-05 by `fix/chain-coalesce`. |
| [[nested-join-arm-unresolved-in-is-as-and-mixed-ternary]] | false rejection | The named residue of `fix/chain-coalesce`: that fix taught the two BOXING sites to recurse into a join arm that is itself a join; two other sites asking the same question were left. `ResolveTernaryArmClasses` - `is`/`as` against a CONCRETE class over a nested `?:` - and `BoxTernaryThinArmToInterface` - the thin arm of a MIXED fat/thin `?:` when that arm is a join (both in `MainListener.h`). Neither is a drop-in recursion: the first folds one i1 answer per arm and a nested arm has a SET of leaf answers; the second is called with the builder already positioned in the caller's block. Contrast that localizes it: the same chain through `as <Interface>` now works. Measured identical on `4c06cce` and the chain fix. Filed 2026-08-05 by `fix/chain-coalesce`. |
| [[join-arm-from-call-result-not-boxed-into-interface]] | false rejection | `take(mk(0) ?? mk(3))` and `IShape j = mk(0) ?? mk(3);` do not compile - a join arm that is a direct CALL RESULT resolves to no class. NOT a chaining defect: reproduces at chain length 1, so `fix/chain-coalesce` neither caused nor closed it. `ResolvePointerElementTypeName` answers an arm from the declared type of the binding a LOAD reads, and a call result is not a load. Fix direction: answer a `CallInst` from the callee's registered return type - a widening of a RESOLUTION helper, so a miss degrades to today's bail; audit `JoinArmsKeepOwner` and the `as`/`is` readers first, where a newly-resolvable arm changes an OWNERSHIP verdict. Filed 2026-08-05 by `fix/chain-coalesce`. |
| [[move-interface-return-of-nullcoalesce-join-not-owned]] | false rejection | `move IShape f() { unique T* a = new T(); T* z = nullptr; return z ?? a; }` is rejected "returned expression is not owned". NOT a chaining defect - reproduces at chain length 1. The whole-expression owned-return check runs BEFORE the per-arm machinery and `IsOwningValue` answers only a load off a NamedVariable, so the coalesce-SLOT load answers false. The existing `transferArmOwnership` path would answer correctly and is never consulted. Contrast: the passing `ownJoinNC` leg returns a join of two `unique` locals, which the whole-expression check does prove. Fix must WIDEN the accept side while keeping a genuinely borrowed arm rejected - build the accept-set first. Filed 2026-08-05 by `fix/chain-coalesce`. |
| [[owning-temp-in-coalesce-fallback-arm-never-destructed]] | ownership hole | LEAK, no diagnostic, identical on `14097e1` and on the merged `fix/tempuniq`. `p ?? makeBox().t` measures `dtors=0` and reads the LIVE value: the `??` right operand is evaluated in `nullcoal_null`, which neither dominates the join nor gets the per-arm `FlushOwnedTempsSince` the `?:` arms get, so the owning temp is never destructed. The `?:` twins measure `dtors=1` and dangle, which is what makes this specific to `??`'s fallback arm rather than to joins generally. **Coupled**: `fix/tempuniq`'s join walk EXCLUDES this arm (rejecting it would refuse a correct program), so fixing the leak turns the shape into a use-after-free and must delete the arm-0-only restriction in `JoinCarriesOwningTempUniqueField` in the same change - both halves together. Same root as [[lambda-body-owning-temp-never-destructed]]; the `??=` half (`coalesce-assign-skips-store-bookkeeping`) is closed by `fix/coalesce-tail`, which makes that spelling a hard error. Filed 2026-08-05 by `fix/tempuniq`. |
deleted 2026-08-05), which closed the four decidable spellings and named this one undecidable at the call site: the store happens in the CALLEE, and the read-only `rd(makeBox().t)` is CORRECT code that must keep working (frozen as `temp_uniq_accept_plain_param_read` with a destructor count). `unique T*` / `move T*` parameters ARE closed - those state the claim at the call site. All four reachable shapes (free function, `list.add`, constructor argument, global-storing callee) are the one plain-`T*` parameter. Fix direction: a CALLEE-side escape fact, not a call-site predicate - `RegisterNonEscapingOwningPtrArgs` answers a close question already. P2 under the residue-not-regression precedent ([[unique-field-to-field-interface-receiver-residues]]); re-rank to P1 if the memory-unsafe-accept rubric wins. Filed 2026-08-05 by `fix/tempuniq`. |
| [[lambda-body-owning-temp-never-destructed]] | ownership hole | LEAK, no diagnostic, identical on `6e9ab46` and the merged fix. `Lambda<Dt*()> f = () => makeDt().t;` prints `dtors=0` - the owning `Box` temp inside a lambda BODY is never registered/flushed, so its destructor never runs at all. The same body as a free function is rejected by the temp-escape gate. Not a missed guard: the lambda body does not run the statement-boundary owned-temp machinery, so the read carries no temp provenance for any guard to see. Fixing it will likely turn the leak into a use-after-free the gate then rejects - land both halves together. Filed 2026-08-04 by the round-1 review of `fix/temp-uniq-borrow`. |
| [[multidim-fixed-array-has-no-brace-initializer]] | feature gap | A multi-dimensional fixed array has no working brace initializer on either binary: nested braces are a PARSE error, a flat list counts against the OUTER dimension only (`int[2][3] a = {1,2,3,4,5,6}` -> "too many initializers for 'int[2]'"), and string-literal elements hit the fixed-array pointer-store reject. `= default` plus element assignment works. Matters because `fix/mdview`'s diagnostic points at `T[N][M]`. Fix the FLAT list first (multiply through `ConstInnerDimensions`); nested braces need a grammar change. Filed 2026-08-02. |
| [[auto-binding-of-fixed-array-loses-shape]] | feature gap | RESTORED and narrowed, re-ranked P1 -> P2. The non-pointer half is fixed; `auto v = <T*[N]>` now REJECTS because `T*[]` collapses to `T[]` in both parser copies. Representation is free - no new field needed. |
| [[char-array-from-string-literal-has-no-spelling]] | feature gap | `char[N] b = "literal";` now has a clear diagnostic and three suggested spellings, but no direct replacement for the C idiom. Master miscompiled it silently. |
| [[array-view-params-unconditionally-noalias]] | latent miscompile | Latent `-O2` miscompile hazard - UB handed to LLVM. P1 the moment a witness exists. |
| [[nested-emission-clears-enclosing-alias-scope-registry]] | latent miscompile | `createFunctionBlock` clears `aliasScopes_` / `viewScopeByOrigin_`; `BuilderState` saves every other field it clears but not these three, so a generic instantiation mid-body re-points the enclosing function's view scope IDs (they are vector indices). IR witness: one view's store and load end up on DIFFERENT `!alias.scope` nodes. Same defect shape and same boundary as the `fix/genfp-return` fix, deliberately not folded into it - it changes emitted alias metadata and needs its own `-O2` pass. Filed 2026-08-05; identical on both binaries of that branch. |
| [[incomplete-layout-message-blames-c-interop]] | diagnostic | **Raised above its severity.** One emission site, three unrelated causes, and the wording names the cause that is usually absent. Two ratification records cite a C-interop cause on files with no C interop. |
| [[last-segment-collision-still-shells-unknown-generic]] | silent accept | The residue of `fix/generic-shell`. `AnyGenericTypeTemplateNamed`'s last-dotted-segment clause still shells a BARE name that is declared nowhere at the use site but matches some namespaced template's last segment, so `namespace N { class Box<T> ... }` plus a top-level `int useIt(Box<int> b)` compiles, links and RUNS clean with no diagnostic - and so does the cross-namespace `Tag<T>` variant, where the template is not even in an enclosing scope. Its non-generic twin gets `unknown type`. NOT a regression: measured identical on `b18ae7f` and the post-fix binary. The clause is load-bearing - built without it, all three `Test/errors/err_namespaced_generic_iface_*.cb` move off their ratified `Unknown identifier` messages - and the obvious scoping narrowing fixes the cross-namespace cell while breaking exactly the top-level cell those tests are built on. Fix direction unknown; a deferred accept resolved by the main pass is the untested candidate. Filed 2026-08-06 by the round-1 review of `fix/generic-shell`. |
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
| [[generic-function-cannot-be-forward-referenced]] | false reject | A generic function called ABOVE its definition does not resolve; the message names a mangled symbol and invents a zero-parameter candidate. Non-generic functions forward-reference fine (that is the pre-pass's job). Not a `function<>` issue - reproduces on `T idg<T>(T v)`. Filed 2026-08-05 from the `fix/genfp-return` matrix; identical on both binaries of that branch. |
| [[sizeof-of-generic-instantiation]] | false reject | `sizeof(B<int>)` -> `unknown type`. The operand skips the generic mangling/queue path. Check `alignof` and cast operands too. |
| [[function-type-as-generic-interface-type-argument]] | false reject | `C<function<int(int)>>` fails on both binaries. Clean failure. |
| [[bare-interface-name-resolves-outward-before-namespace]] | false reject | Outer scope wins for non-generic interface names, opposite to the ratified generic rule. |
| [[macos-header-import-and-framework-link]] | false reject | Two gaps block first-class Apple-API binding: header import hard-codes a Linux triple on Darwin (`objc/runtime.h` registers 1 of ~80 functions), and there is no `-framework` / `-F` link channel. The macOS demos work around both with dlopen + typed `objc_msgSend` casts. |
| [[unique-assign-syntactic-owned-rhs-leaks]] | ownership | Owning value laundered through a BORROW-returning call still leaks. |
| [[delete-borrow-via-named-local]] | ownership | Opt-in spelling closes it; the bare case is still open. |
| [[deref-of-moved-pointer-guard-inside-callee]] | ownership | False positive: guarded only by a conditionally-terminating callee. |
| [[owning-temp-ledgers-should-be-split]] | ownership | `ownedReturnTemps_` fails UNSAFE, `ownedNewTemps_` fails SAFE. |
| [[detection-ledgers-not-discarded-on-aborted-arm]] | ownership | Detection-only ledgers survive an aborted `?:` arm. |
| [[file-offsets-capped-at-2gb]] | silent wrong value | `core/filesystem.cb` narrows every offset through `int`, so `File.size()`/`tell()`/`seek()` truncate past 2 GB on ALL platforms - the public surface is `int` too, so widening the internals alone is not enough. Split out of `ftell-fseek-long-width-on-windows` when that P1 landed 2026-08-02; NOT the `long`-width defect, which is fixed. Had no row in this table until 2026-08-03 - it was filed in narrative only. |
| [[simd-type-spelling-unusable-outside-declarations]] | feature gap | `simd<T,N>` is recognised only in `ParseDeclarationSpecifiers` and as a `primaryExpression`, so a cast target, a lambda parameter and a tuple/`function<>` signature component all say "unknown type 'simd<float,4>'", and `simd<T,N>[]` silently DROPS the empty bracket and compiles as a plain vector local. Measured identical on `904f026` and `fix/simdptr`. Wants one encoded-name mechanism (mirroring `BuildEncodedClosureName`), not four patches. Filed 2026-08-03. |
| [[c-binder-misses-decorated-function-pointer-parameter]] | false rejection | A `const`-qualified C function-pointer parameter binds as `void*`, and the code-value gate then rejects the legal call. Found 2026-08-03 verifying the `void*` oracle for `fix/funcptr-rebind`; the file's first filing named the wrong qualifier and repro - the correction inside is the useful part. Had no row here until 2026-08-04. |
| [[double-pointer-arg-binds-single-pointer-param]] | silent wrong value | `byPtr(pp)` with a `Circle**` and a `Circle*` parameter compiles, links and RUNS, returning the low bytes of the pointee address instead of the field (`2003` expected; the exit code is the pointee ADDRESS's low bytes, so it is ENVIRONMENT-dependent - the same binary gives 176 and 224 from two different output directories - and a change in it is NOT a change in behaviour). Opaque pointers make both sides one LLVM type, so unlike its `T*`-into-`T` sibling there is nothing for the module verifier to catch. Same predicate (`IsTypeMatch`), depth axis instead of the pointer-ness axis; held out of `fix/ptrarg-byval` because an `ElemPointer` rejection would reject programs that compile and run today, whereas that fix rejected a shape no compiling program can contain. Filed 2026-08-05 by `fix/ptrarg-byval`. |
| [[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]] | feature gap | `sizeof(Box<double>)` gives a located `unknown type 'Box<double>'` while `alignof(Box<double>)` returns the correct 8 - and `alignof` is not guessing (`Box<char>` 1, `Box<BigA>` 32 under `alignas(32)`). The `('sizeof')*` prefix loop in `unaryExpression` consumes the token before the `('sizeof'\|'alignof') '(' typeName ')'` alternative can match, so a prefix `sizeof` is serviced by a TEXT-reconstruction handler that asks `GetType` for the source spelling, and `dataStructures` is keyed on the mangled instantiation name. `alignof` has no prefix loop and lands on the real `ParseTypeName`. This is the residual FEATURE gap behind the crash `fix/sizeof-closure` closed; that branch's floor was the located diagnostic. Measured identical on `f24fb18` and on the fix. Filed 2026-08-06 by `fix/sizeof-closure`. |
| [[zero-parameter-generic-function-emits-double-mangled-symbol]] | link failure | `int gid<T>() { T v = default; return 7; }` called as `gid<P>()` fails to link: `undefined symbol: __gid__P_gid__P__`, the instantiation name applied twice. One value parameter is enough to make the same shape work (`gone<P>(5)` runs), and inference works (`gtwo(p)` runs) - it is the ZERO-value-parameter generic function called with an explicit type argument. Not diagnosed. Measured identical on `f24fb18` and on `fix/sizeof-closure`; unrelated to that fix, found on its probe run. Filed 2026-08-06. |
| [[closure-type-argument-to-a-generic-function]] | feature gap | A closure type as a generic FUNCTION's type argument does not resolve, in either spelling: explicit `idf<function<int(int)>>(g)` gives `unknown type 'function<int(int)>'` reported on the TEMPLATE's parameter list, and inferred `idf(g)` mangles the argument from its LLVM repr and instantiates `idf__i8`. The generic STRUCT path funnels through `ResolveTypeArgEntry` and works (`Box<Lambda<...>>`, `list<function<...>>`, and after `fix/lamptr-generic` also `Box<function<T>*>`); this path appears not to, so the fat/thin pointer asymmetry that landed there does not reach it. Non-closure control `idf<int*>(&n)` works on both binaries. Measured identical on `4c06cce` and on the merged `fix/lamptr-generic`. Filed 2026-08-05 by `fix/lamptr-generic`. |
| [[union-closure-member-call-crashes-compiler]] | compiler crash | Calling a closure member of a `union` SIGSEGVs the COMPILER (fat member; thin twin dies in module verification with an invalid whole-union bitcast), even for a legal named-fn source. The union member-default path also runs neither assign-provenance gate (unreachable at runtime today). Pre-existing; found by `fix/fat-default` review. Filed 2026-08-07. |
| [[fixed-array-default-skips-field-initializers]] | silent wrong value / crash | Stack `S[N] a = default;` zero-fills and skips every element field initializer - `0 0` where the field default says `7 7`, SIGSEGV when the skipped field is a closure - while `new S[N]` and single `S s = default;` run them. Pre-existing; found by `fix/fat-widen` review. Filed 2026-08-07. |
| [[defaulted-ctor-param-default-construct-aborts]] | compiler crash | Default-constructing a `class`/`struct` whose only ctor has ALL parameters defaulted crashes the COMPILER at compile time - the `-o` compile itself nondeterministically exits `134`, `139`, or `1` with an internal "declared with no enclosing scope" diagnostic; no executable is ever produced. Also fails on the explicit-argument spelling (`CDef d = CDef(7);`). Identical on `master` (33b3ac4, 134 8/8) and `fix/class-undef` (63107e2); not a regression from that fix. Root cause not traced. Found by the round-1 review of `fix/class-undef`. |
| [[fixed-array-field-brace-default-discarded]] | miscompile | A FIXED-ARRAY field's own `= {1,2,3}` default brace list is silently discarded - `struct Outer { int[3] a = {1,2,3}; };` reads `0 0 0`, while the identical LOCAL declarator `int[3] a = {1,2,3};` prints `1 2 3`. Same family as the fixed [[struct-field-default-brace-list-discarded]] (landed record below) and deliberately left out of it: the list is POSITIONAL and needs an `[N x T]` aggregate builder, not `EmitFieldInitializer`, and the existing `EmitPositionalFixedArrayInit` registers an array LOCAL so it cannot be reused as-is. The neighbouring POINTER-field spelling (`Inner* p = {x=1};`) reads `nullptr` silently and probably wants the declarator's rejection instead. Measured identical on `68c78fc` and `fix/field-brace`. Filed 2026-08-06 by `fix/field-brace`. |
| [[lambda-literal-param-default-invalid-ir]] | compile failure | A lambda literal as a PARAMETER default emits invalid IR (`Module verification failed: Found return instr that returns non-void in Function of void return type`); the same literal as a local decl-init works. Pre-existing on `68c78fc`; lives in `GenerateDefaultParamOverloads`, the emitter the `fix/assign-gate` amend touched. Found by round-3 review of `fix/assign-gate`. Filed 2026-08-06. |

### P3 - diagnostics, latent, deliberate deferrals (`p3/`)

| Issue | Family | Severity |
|---|---|---|
| [[sizeof-steals-discarded-tuple-comparison-spelling]] | deliberate deferral | `fix/sizeof-closure`'s admission of `(` `)` `,` inside generic brackets also classifies the tuple-comparison text `a<b,c>d` as a type. Measured cost is ONE spelling: the value-DISCARDED statement `sizeof(a<b,c>d);` went from rc 0 (a no-op that printed `ok`) to `unknown type 'a<b,c>d'`. Every CONSUMING form was already rejected on PRE with a different message (`cannot cast an aggregate value`; `no overload of 'operator+' ... tuple__bool__bool`, which is what proves the tuple reading), and the tuple WITHOUT `sizeof` (`tuple<bool, bool> t = (a<b, c>d);`) is accepted identically on both binaries - nothing outside the `sizeof` operand was taken. The POST message also calls a tuple expression a "type". NOT fixed by an expression fallback: that is exactly the fallthrough that reached `CreateCast` with a null `Primary` and SIGSEGVed. Take it with [[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]], whose fix makes the parser (not a character test) decide. Filed 2026-08-06 by review round 1 of `fix/sizeof-closure`. |
| [[inline-deref-of-container-call-result-has-no-storage]] | diagnostic | False rejection with a LOCATED but internal-sounding message: `(*ls.get(0))(2)` on a `list<function<int(int)>*>` gives `Unable to dereference an object without a Storage.` while the two-line form (`function<int(int)>* e0 = ls.get(0); (*e0)(2)`) compiles and runs - that two-line spelling is what `Test/test_function_ptr.cb` already covers. A deref wants an addressable `Storage`; a call RESULT has only `Primary`. Element type is probably incidental - check `*someCall()` on a plain `int*` return before scoping it as a container issue. Identical on `8c5a860` and on the merged `fix/genfn-lowering`. Filed 2026-08-05 by `fix/genfn-lowering`. |
| [[implied-move-store-boxed-spelling-false-rejects]] | diagnostic | PRE-EXISTING false rejection, identical on `d93c359` and on the merged `fix/ptrcopy`. A plain `p = c;` store between two pointer locals is an IMPLIED MOVE, so the raw `delete p;` is correct and accepted - but `MarkPointerRebound` runs BEFORE the transfer and records `InheritedKeepsOwner` naming `c`, which is null by then. Nothing reads that in the raw-delete guard; `BindingKeepsOwnershipOfBoxedObject` does, so the BOXED twin is rejected with a message that is false at that site. P3: working remedy, mis-blamed rather than dangerous. Recorded because it is why `fix/ptrcopy` introduced `JoinKeepsOwner` as a SEPARATE field. Filed 2026-08-05 by `fix/ptrcopy`. |
| [[interface-boxing-keyed-on-source-binding]] | deliberate deferral | RE-BUCKETED P1 -> P3 2026-08-04, on the file's own text: the live double free it was filed for was CLOSED 2026-08-02 (borrowed-interface-box delete diagnosed via the positive keeps-owner proof; landed record below), and only the preventive remainder is left - `RegisterInterfaceBox` dedupes on `FatValue` alone, harmless today. The file kept its name and moved to `p3/`, so existing links still resolve. |
| [[owning-temp-parent-misroutes-chained-alias-access]] | diagnostic | RE-RANKED P1 -> P3 2026-08-02, on the file's own "re-rank freely" and on a re-measurement: the VERDICT is right (two `unique` owners really is an error) and only the WORDING is wrong, so no program's accept/reject status changes. A wrong message is P3 by this table's own rubric; it sat at P1 only for visibility to whoever next touched the `unique` field-store routing, and that work has landed. Still live on `ca5a02a`: the call-result message fires on a container-element shape, stating a false mechanism and naming a remedy that aborts 134. |
| [[return-dangle-missed-when-slot-has-extra-user]] | deliberate deferral | RECLASSIFIED P1 -> P3 2026-08-02 by the maintainer, rationale kept intact. Missed dangle, no diagnostic - but the shapes were ALL accepted before `2bcc5a0` too, so this is residue, not a regression, and its own file rules out the obvious remedy (widening the extra-user whitelist re-introduces false rejections). Stays open as a record of what the pass cannot see. |
| [[unique-field-to-field-array-element-receiver]] | deliberate deferral | RE-RANKED P1 -> P3 2026-08-05 by the release campaign (NOT a maintainer ruling - override freely). Twice-narrowed (`fix/uniq-array-elem`, `fix/uniq-global`); the residue is a non-constant index within ONE array (runtime subscript, unenforced `const` integer), still a silent abort with no diagnostic for those shapes. Unprovable without taking away legal same-index self-assigns - the file's own "NOT an oversight" section rules out widening - and every residue shape was accepted before the narrowing fixes too. Same rationale as `return-dangle-missed-when-slot-has-extra-user`: residue, not regression, and a permanent non-fix does not belong in the P1 working set. |
| [[unique-field-to-field-interface-receiver-residues]] | deliberate deferral | The five interface-receiver shapes the `fix/iface-selfassign` proof deliberately cannot reach (pointer/`new`-boxed receivers, parameters and call results, sub-objects of one container, lambda bodies). All missing diagnostics, never false rejections; all accepted by the PRE binary too, so residue, not regression - same rationale as `return-dangle-missed-when-slot-has-extra-user`. P3 by that precedent; re-rank to P1 if the maintainer rules the silent-double-free rubric wins. Filed 2026-08-05 out of the fix's round-1 review. |
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
| [[json-ish-brace-literal-still-typed-string]] | false reject | `char* j = "{\"k\":1}";` is rejected ("cannot initialize pointer ... with a value of type 'char'") because a matched brace pair whose content cannot be an expression still routes through the interpolation path and comes out a `string`. The EMPTY-pair face of the same defect is closed (`fix/brace-literal`); this one was left open because the two paths disagree about `{{`/`}}` INSIDE such a region and `Test/test_reflect.cb`'s `toJson_nested` literal depends on the current verbatim copy. Filed 2026-08-06. |
| [[indirect-call-string-to-charptr-fails-in-verifier]] | diagnostic | A `string` passed to a `char*` parameter through a `function<int(char*)>` value dies with `Module verification failed:` and no source location. The DIRECT spellings are diagnosed (`fix/brace-literal`); the indirect arms (`CheckIndirectCallArgShape`) do not know the case. Identical on `56ebc52` and on `fix/brace-literal`. Filed 2026-08-06. |
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
| [[closure-arg-suffixes-unvalidated-in-signature-position]] | diagnostic | Two missing rejections left deliberately open by `fix/lamptr-generic`, both measured identical on `4c06cce` and on the merged branch. (1) The `[]` suffix on a closure generic type argument is unvalidated - `Box<Lambda<int(int)>[]>` compiles, though the DECLARATOR spelling is rejected by `err_lambda_array_view.cb`'s first leg; it is the structural twin of the pointer bug one suffix over, in the same branch that returned before the suffix was applied. (2) A fat closure POINTER inside a closure SIGNATURE (`Box<Lambda<int(Lambda<int(int)>*)>>`) is accepted by `ResolveSigComponentCodegen` - a THIRD site, distinct from the declarator guard and the type-argument funnel. Neither is reachable to a store or a call today (the lambda literal that would exercise (2) fails with `unknown type`), so neither was rejected: the standing rule is that a site added to a reject must be shown broken from the `--no-opt` IR first. Filed 2026-08-05 by `fix/lamptr-generic`. |
| [[unique-on-closure-arg-message-denies-the-pointer]] | diagnostic | `RBox<unique TA*>` (and the direct `RBox<unique function<int(int)>*>`) is rejected with `unique requires a pointer or interface type` - factually false, since `TA*` IS a pointer. The REJECTION is correct (a closure owns no allocation) and the declarator path already words it correctly ("a function pointer or closure does not own an allocation"); only these two type-argument arms disagree. The DIRECT arm measures identical on `4c06cce` and on the merged `fix/lamptr-generic`; the ALIAS arm's message is newly REACHABLE there (that branch re-gated the arm from `!hasPointer` to `!hasArrayView`) with the wording itself unchanged - exposure, not regression. Fix both arms together and reuse the declarator wording. Filed 2026-08-05 by the round-1 review of `fix/lamptr-generic`. |

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

### fix/uniq-global - two distinct STACK-or-GLOBAL roots proved different (RATIFIED)

Closes [[unique-field-global-struct-self-assign-false-positive]]. `gA.slot = gB.slot` on two
file-scope structs compiled clean and aborted at teardown (exit 134) with no diagnostic; the same
copy between two LOCAL holders rejected. The corrected mechanism in the issue file held exactly:
the guard stack IS entered, and `selfFieldAssign` suppressed it because a global-struct field read
carries an EMPTY `CallerName`, so two different globals compared equal on caller + field name.

**The issue file's PREFERRED step 1 was not taken, because its premise is false.** It said globals
have no `NamedVariable` representation at all, so giving them one would be additive and would let
the existing root proof cover the repro. Measured on `a846e6e`: `GetGlobalVariableNV`
(`LLVMBackend.h`) already builds a `NamedVariable` for a global and already populates `Storage`
with the `GlobalVariable*`, which is what `ProvablyDifferentSlots` consumes - so "once globals
carry Storage the existing proof covers this" was already true of `Storage` and still did not fire.
The only thing missing was `CallerName`, and setting it is NOT additive: the empty string is
load-bearing at several sites that read `!CallerName.empty() && FindVariableStorage(CallerName) ==
nullptr` as "this is a call result, not a named variable" (`LLVMBackend.h:17605`, `:17963`) - a
global satisfies both halves, so those funcptr checks would start firing on globals - and a dozen
diagnostics print `CallerName` in preference to the type name. Step 2 was taken instead.

The proof: `ProvablyDifferentObjects` (`cflat/MainListener.h`) strips the whole GEP chain off each
address and answers true when the two roots are DISTINCT and each is an `AllocaInst` or a
`GlobalVariable` - two such objects are distinct objects in LLVM whatever the indices in between.
It is keyed on the root KIND, never on `Value*` inequality alone; a `LoadInst` root (a pointer
receiver, an array view) is not admitted, which is what `SameLoadedPointer` exists for and why
`gA.slot = pa->slot` with `pa = &gA` is untouched. `ProvablyDifferentSlots` consults it first and
otherwise keeps the same-root constant-offset rule unchanged, so the runtime-index residue of
[[unique-field-to-field-array-element-receiver]] stays accepted by construction.

ONE other behaviour change, sound and recorded rather than left implicit: two DISTINCT GLOBAL
arrays with runtime indices (`gArrA[i].slot = gArrB[j].slot`) flip accept -> reject, because two
globals are two objects whatever the indices. Measured accepted-and-aliasing on `a846e6e`. Two
distinct LOCAL arrays already rejected (their `CallerName`s differ), and the ONE-array runtime
pair is untouched - that file's residue framing is amended to say so.

Both halves of the guard move together, as they must: `selfFieldAssign` (the diagnostic) and
`sameFieldStore` (which EMITS the reassignment-destruct and the auto copy). The store half was
independently broken for globals and is what the owning-VALUE legs pin - `gGB1.f = gGB2.f` on two
global holders of a counting-destructor value measured ZERO destructs and aborted, where the
identical local spelling destructs once. Distinct alloca/global roots satisfy
`AddressRootIsStackOrGlobal` by construction, so the `-Initialized` form gains nothing extra here.

Evidence: a 27-cell receiver-kind matrix in `scratch/ugs_*` (global-to-global, nested, namespaced,
class, global array, through-pointer, global-to-local, local-to-global, `move` spelling, plain
non-owning fields, and owning-VALUE `string` / counting-destructor twins, each with local oracles);
`--check` differential sweep over 446 `.cb` in `Test/`, `example/` and `core/` with 29 diffs, ALL of
them the binary's own core path inside an "imported file not found" message and zero behavioural;
compile+run A/B over the 45 top-level `Test/*.cb` with 8 diffs - 4 nondeterministic (an address, a
concurrent-btree restart count, a cycle counter, a `timeout` message for the Windows-only files) and
one intended (`test_move.cb`). `leaks --atExit` on `test_move.cb` is 13 leaks / 256 bytes on both
binaries with the baseline legs; the new legs add none (PRE with the new legs measures 15 / 288).

Legs: `Test/test_move.cb::testUniqueGlobalReceiverFieldStore` (15 value legs - self-assign on one
global, namespaced, nested, the owning-VALUE destruct count, the owning-VALUE self-assign, and a
`string` field pair) and three scoped `expect_error` legs in
`Test/errors/err_unique_array_element_field_to_field.cb` (global-to-global, nested, namespaced),
each mutation-tested individually to a self-assign and each flipping the file to exit 1.

KNOWN LIMIT, deliberate: the message still names the source as `'slot'` with no caller, because
the caller name genuinely is not available for a global receiver. That is the same wording the
global-into-local form already produced, and the remedy correctly degrades to "prefix the source
expression with 'move'" rather than naming an expression that would not compile.

### fix/untracked-copy - the copy of an owning local aliased at its DECLARATION (RATIFIED)

Closes [[delete-of-untracked-pointer-copy-not-diagnosed]]. `T* b = c;` and `alias T* b = c;` off a
live owning local produced a binding with NO ownership fact at all, so `delete b;` and the boxed
`IS s = b; delete s;` both compiled and double-freed (exit 134). Thirteen spellings did: the plain
copy, the `alias` declaration, `auto`, a parenthesized initializer, a `using` type alias, two hops,
a copy in a nested scope, each of their boxed twins, and a copy whose source is later `move`d out
or deleted. Measured over a 62-cell corpus (13 changed, 49 byte-identical on both binaries).

The aliasing is recorded where BOTH sides are in hand - the declaration - as its own pair,
`NamedVariable::BorrowsOwningLocal` + `OwningLocalOrigin`, plus the source's SLOT in
`OwningLocalStorage`. It is deliberately NOT `IsBorrowed`: that flag is read by roughly thirty
sites (argument passing, returns, stores into `unique` fields, struct copies), and widening it
would have changed all of them. `BorrowsOwnedElement` set the precedent and says so in its own
comment. The decision is made by STORAGE IDENTITY (`FindVariableByStorage`), never by spelling, so
a parenthesized or `auto` source resolves and a shadowing name cannot be mistaken for another
binding. Consumers: the raw-delete guard in `ParseDeleteExpression` and
`BindingKeepsOwnershipOfBoxedObject`, which use the SAME predicate, so the raw and boxed spellings
reject exactly the same set and boxing cannot launder what `delete b;` rejects.

**`alias` is NOT what arms this, and the issue file's fix direction was wrong on that point.** It
said the `alias T* b = c;` spelling should set the flag unconditionally, "since `alias` is the user
saying borrow out loud". Measured, that would have false-rejected three programs master compiles
and frees correctly, each with a LEAKING remedy: `alias T* e = makeAliasT(); delete e;`,
`alias T* e = makeMoveT(); delete e;` and `alias T* b = new T(); delete b;` - in all three the
delete is the ONLY release, the same polarity error `IsAliasBorrow` was excluded from the boxing
proof for. What arms it is the SOURCE being a live owning local; the `alias` spelling then falls
out for free, because `alias T* b = c;` has the same source as `T* b = c;`.

**The proof RETIRES at BOTH ends, and the source end was found by measurement, not reasoning.**
The first cut checked only the copy's own `PointerRebound` and FALSE-REJECTED
`T* c = new T(); T* b = c; c = new T(); delete b;` - a program master runs at one free per object,
where reassigning the SOURCE leaves the copy holding the ONLY reference to the original, so the
rejection's remedy ("delete `c` instead") would have freed the wrong object and leaked. Both ends
are now re-asked at the delete site through `OwningLocalCopyStillAliases`, which resolves the
recorded slot and requires that binding to STILL be a non-rebound owner. An unresolvable or dead
source answers false - the accept direction. `Test/test_move.cb`'s `delete_copy_owner_reassign_*`
legs pin all four retirement shapes with values and free counts; the reject half is the
`copyOfOwner*` legs in `Test/errors/err_delete_borrowed_interface_box.cb`.

Untouched by design, because none of them is an owning local: a LATE-ASSIGNED local
(`T* c = nullptr; c = new T();` - the box is its only owner, the polarity error this family died
on twice), a GLOBAL owning pointer (globals never scope-exit-free), an `alias`- or `move`-returning
call result, a container element, a parameter (already rejected), and a `unique` local (already
rejected). All measured accepted-and-correct on both binaries.

**The neighbouring double frees this accept set measured were FILED, not parked here**, since this
section's heading says nothing in it is open: `pointer-copy-propagates-no-ownership-fact` (P1) held
all four, with repros - a copy stored into a `unique` field, a one-hop copy of a container-element
borrow, `T* d = move b;` off a copy, and a `?:` join into a pointer declaration. All four are the
same missing propagation through a plain copy, each needing a different fact carried and its own
accept set, which is why none was folded in here. **All four are now CLOSED by `fix/ptrcopy`** and
that file is deleted; see its own design record below for the four mechanisms and the three residues
it split out.

**Stale blame, tolerated (P3-grade).** In the two cells where the source stops owning the object
BEFORE the copy's delete - `T* d = move c;` and a preceding `delete c;` - the REJECTION is correct
(the object is `d`'s, or already freed) but the message still names `c` as the live owner, and its
"Delete 'c' instead" remedy frees nothing: measured `D=0`, rc 0 for the moved-from form and a
no-op second delete for the other. The retirement check answers about the SLOT's binding, which is
still owning in both. The sibling record documents the same tolerance for stale blame elsewhere in
this proof; correct blame needs move/delete state threaded into the recheck.

### fix/ptrcopy - the four ownership facts a plain pointer COPY drops (RATIFIED)

Closes `pointer-copy-propagates-no-ownership-fact`, the sibling of `fix/untracked-copy` above. That
change carried ONE fact across a pointer declaration (`BorrowsOwningLocal`, consumed by the raw and
boxed `delete` guards); this one carries the rest and serves the other two consumers. All four filed
repros were silent double frees with no diagnostic on `d93c359`, and all four now reject. Measured
over an 88-cell corpus (`scratch/upc/final_matrix.txt`): **30 cells changed** (crash -> diagnostic),
**53 byte-identical**, and **5 that crash on BOTH binaries** - the filed residues below, whose only
difference is the abort code (133 vs 134, a benign cold-vs-warm cache property of the two builds,
not a behaviour difference). 30 + 53 + 5 = 88.

| Member | Mechanism | Consumers reached |
|---|---|---|
| (a) copy stored into a `unique` FIELD | the store site re-asks `OwningLocalCopyStillAliases` on the SOURCE binding, resolved by storage identity | unique-field store |
| (b) one-hop copy of a container-ELEMENT borrow | `BorrowsOwnedElement` propagated at decl-init off the source BINDING, not just off the accessor RESULT | raw `delete`, boxed `delete`, `move` |
| (c) `move` off a copy | a destination-agnostic guard in `ParseMoveExpression` reading the SAME two proofs the `delete` guard reads | every `move` spelling: decl, `=`, argument, `return` |
| (d) `?:` / `??` JOIN into a pointer declaration | `JoinArmsKeepOwner` walks the arms and records `JoinKeepsOwner` + `JoinKeepsOwnerSource` | raw `delete`, boxed `delete`, unique-field store |

**(a) is a store-side re-ask, NOT a new fact.** The decl-init recording `fix/untracked-copy` landed
already holds everything the store needs; only the consumer was missing. The DIRECT spelling
`h.f = c;` is an IMPLIED MOVE that transfers ownership out of `c` and stays legal, which is why the
diagnostic names it as the remedy - a rejection whose remedy does not exist is the failure mode this
family has hit before.

**(b)'s retirement is COPY-END ONLY, and that asymmetry is deliberate.** For an owning-local copy,
rebinding the SOURCE leaves the copy holding the only reference, so the fact retires at both ends.
For a container element the CONTAINER owns it no matter what the source LOCAL is later pointed at,
so only the copy's own rebinding retires it - `SetVariableBorrowsOwnedElement` on the plain `=`
already does that, and the accept leg `elem_copy_rebound_*` pins it. A source that was rebound or
`??=`'d BEFORE the copy is the stale direction and is dropped at the declaration (accept side).

**(d) uses the BOTH-ARMS rule, taken from the per-arm interface boxing ledger** (`BoxInterfaceJoinArms`):
every non-null arm must resolve to a live binding that keeps ownership, or the fact is dropped. A
null LITERAL arm is neutral - it owns nothing, so it neither proves nor blocks. A MIXED join (one arm
an owning local, one a fresh `new`) is therefore ACCEPTED rather than rejected on a may-alias, which
is what keeps this from reopening the false-rejection direction. Both spellings are covered: `?:`
arms come off the PHI, `??` arms off `FindNullCoalesceJoin`, the same two routes the boxing helper
uses. The ASSIGNMENT form is covered too, recorded by the `=` that rebound the local.

**`JoinKeepsOwner` is a SEPARATE field from `InheritedKeepsOwner`, and reusing the latter was tried
and MUST NOT be retried.** It looked ideal: same shape, same retirement, already consulted by the
boxing proof. But `MarkPointerRebound` also sets `InheritedKeepsOwner` on a plain `p = c;` store
between two pointer locals - and that store is an IMPLIED MOVE, so `c` is nulled and `delete p;` is
CORRECT (measured: one free, exit 0, on both binaries). Reading it in the raw-`delete` guard
false-rejected exactly that program. The accept leg caught it before the guard shipped. The
pre-existing BOXED half of the same stale blame is filed as
[[implied-move-store-boxed-spelling-false-rejects]] (P3) - `BindingKeepsOwnershipOfBoxedObject` does
read `InheritedKeepsOwner`, so the boxed twin of that correct program is rejected on both binaries.

**One RATIFIED behaviour change.** `T* d = nullptr; d = move b;` off a copy - the ASSIGNMENT spelling
with no later `delete` - compiled and ran correctly on `d93c359` (one free: the move transferred
nothing, and the source still freed at scope exit) and is now REJECTED. It works by accident: the
identical program with `delete d;` added aborts 134 on `d93c359`. The guard is at the `move`
expression rather than at each destination, so every spelling of the same mistake agrees, which was
the point of the member. The diagnostic is true at the site and its remedy (`move` the owner itself)
is real.

**Scoped out, with reasons, not silently.** `move` of an ordinary `IsBorrowed` binding (a borrowed
pointer PARAMETER, its one-hop copy, and the `move`-returning-wrapper spelling) is still a silent
double free - filed as [[move-of-borrowed-pointer-adopts-into-plain-destination]] (P2). It was left
out because `MainListener.h` carries an explicit policy directly above the new guard - "Forwarding an
ordinary borrow as 'move' stays legal (the programmer asserts the borrow is dead)" - so closing it
means REOPENING a ratified decision with its own accept set, not adding a clause. A `??` join whose
LHS arm is a null-VALUED LOCAL (rather than the null literal) still drops the proof in BOTH the raw
and the boxed spelling; filed as [[coalesce-join-null-local-arm-erases-owner-proof]] (P2), and the
agreement between the two spellings is why it is a pre-existing classification hole rather than an
asymmetry this change introduced.

**Both are silent double frees, which is literally the P1 rubric, and both are filed at P2 under the
residue-not-regression precedent** - the same rationale as
[[unique-field-to-field-interface-receiver-residues]] and
`return-dangle-missed-when-slot-has-extra-user`: every shape they cover was accepted by the PRE
binary too, so they are residue this change did not reach rather than anything it broke. **Re-rank
either to P1 if the maintainer rules the silent-double-free rubric wins** - that escape hatch is
stated here and in both rows deliberately, exactly as the sibling row states it.

Untouched by design, all measured accepted-and-correct on both binaries: a late-assigned source, a
source rebound after the copy, a copy rebound before its use, a GLOBAL owning source, an
`alias`/`move` return result, reading through any of these copies, and passing one as a borrowed
argument. Every one is a shape where the copy or the field is the SOLE owner and a rejection would
LEAK. The accept half is `Test/test_move.cb`'s `testCopyFactPropagationAccepts` (51 legs, values plus
free counts); the reject half is three legs in `Test/errors/err_unique_alias_into_field.cb` and six
in `Test/errors/err_delete_borrowed_interface_box.cb`, each pinning its own wording and each verified
non-vacuous individually against the `d93c359` binary.

**The join proof records every ARM's SLOT, not just a rendered owner name, and re-asks them all at
each consumer** (`JoinArmsStillKeepOwner`) - the arm-side twin of `OwningLocalCopyStillAliases`, using
the same explicit `!PointerRebound` test for the same reason: a rebound binding keeps its `IsOwning`
flag, which `BindingKeepsOwnershipOfBoxedObject` answers ABOVE its own retirement, so asking that
helper alone would never retire an arm. Round 1 of this change recorded only the NAME, so the proof
retired only when the JOIN end was rebound: nulling or rebinding an ARM left it stale and
FALSE-REJECTED seven programs master runs at one free - and the plain-copy siblings of those exact
spellings were accepted, so the change made copy and join disagree on the very axis it set out to
align. The 38-leg accept set had no cell on the arm axis, which is how it shipped; the
`join_arm_nulled_*` / `join_arm_rebound_*` legs are that missing axis. `T* o = move c;` does NOT
retire an arm (a move sets no `PointerRebound`), so the genuine double free stays rejected - the same
stale-blame tolerance the sibling record documents.

**What the arm re-ask gives up** (round-2 measurement; every shape is accepted by the PRE binary
too, so residue, not regression, and each is the ratified unprovable-then-accept polarity): five
genuine double frees round 1 rejected are accepted again once liveness is unprovable at the
consumer. The shapes: an arm nulled inside an UNTAKEN `if` (the walk is not control-flow-aware,
the same principle the `InterfaceBoxProvenanceUnknown` comment states); an arm that is itself a
COPY later rebound (the copy's `PointerRebound` short-circuits before anyone asks whether the
copy's own source still owns - the sharpest of the five); the same copy shape one level into a
join-of-joins; an arm rebound away and then back to the original object; and a join nest deeper
than the cap. On that cap: `JoinArmsStillKeepOwner` bounds recursion at depth 4 - a 5-deep join
chain still rejects, a 6-deep one degrades to accept, the safe direction.

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
| The four ownership facts a plain pointer COPY drops - `unique`-field store, container-element borrow, `move`, and a `?:` / `??` JOIN; `move` of a provable borrow rejected into ANY destination (RATIFIED) | fix/ptrcopy (branch, not yet merged) |

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
returning `function<>` poisons the ENCLOSING function's return coercion. That is why the
regression test had no generic-producer leg at the time. **FIXED 2026-08-05** - see the landed
design record "Generic instantiation leaked its return TypeAndValue into the caller" below;
`Test/test_function_ptr.cb` now has the generic-producer legs (`gfp_*`).

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
`class-no-ctor-default-construct-returns-undef` (a `class` with no user constructor
default-constructs to `undef`, unrelated code path; FIXED by `fix/class-undef`, record below), [[struct-field-default-brace-list-discarded]]
(a struct FIELD's own brace-list default is dropped, not a variable declarator; since FIXED by
`fix/field-brace`, record below), and
`interface-typed-global-brace-init-discarded` (an interface-typed global falls through this
fix's guard entirely, since `GetDataStructure` has no entry for an interface name; since FIXED by
`fix/iface-global`, record below) - plus the
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
'...'` guard does not fire for it. Its file recorded the miscompile face too. **FIXED and deleted
2026-08-06** by `fix/brace-literal`; see the landed design record below.

Also confirmed NOT reachable, with the measurement rather than an argument: `S* p; p = {a=1};`
(assignment form) and `S** q = new S*{a=1};` are parse errors; a global pointer declaration in every
brace spelling was already rejected by `fix/global-positional`; a struct FIELD default
(`struct W { S* q = {a=1}; };`) silently discards the list and is the already-filed
[[struct-field-default-brace-list-discarded]], a different path that never calls
`EmitFieldInitializer`. **Superseded in part:** `fix/field-brace` (record below) fixed that path
for by-value struct/class/container fields by routing them THROUGH `EmitFieldInitializer`; the
POINTER-field spelling quoted here is the one cell it deliberately left untouched, and it still
silently reads `nullptr` - now tracked by [[fixed-array-field-brace-default-discarded]].

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
[[struct-field-default-brace-list-discarded]], a different path. **Superseded in part:**
`fix/field-brace` (record below) fixed the NON-POINTER half of that sentence - a by-value struct
field default `S s = {a=1}` now yields `1`; only the POINTER half is still discarded. `new T{}`
and `f({})` are PARSE
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

## Landed: `fix/temp-uniq-borrow` (2026-08-04) - a temp's `unique` field may not escape the statement

Closes [[temp-unique-field-into-borrow-slot-use-after-free]] (P1). `q.p = makeBox().t;` where
`Node` has no destructor compiled clean, ran, printed `70` and exited 0 - the `70` was read out
of memory the temp `Box`'s synthesized destructor had already freed.

**Root cause, confirmed as filed.** All three persist-site rejects
(`ParseAssignmentExpression`, `ParseDeclaration`'s decl-init, and the RETURN path) tested
`rightNV.FromOwningTempField && IsOwningValueType(rightNV.TypeAndValue.TypeName)`. That type-name
gate is true only when the POINTEE has a destructor; the pointer being freed belongs to the TEMP,
not to the pointee's type, so a dtor-less pointee fell through every gate. The field-to-field
reject declined correctly - the destination is a plain borrow, not a second owner.

**The rule.** `IsOwningTempUniqueFieldEscape(nv)` = `FromOwningTempField && OwningTempParent &&
!IsMove &&` (owning `unique` POINTER field, or owning `unique` INTERFACE field). It is applied at
FIVE persist sites: the three above, plus `EmitOneFieldInit` (brace-init) and the INTERFACE
decl-init branch (`IsFatInterfaceValue()`), neither of which had a leg of this family at all.
Reads that do not persist never reach a persist site and are untouched.

**The interface decl-init branch was missed on the first pass**, found by review: `IShape s =
makeIBox().t;` takes its own branch of `ParseDeclaration`, so the guard sitting in that branch's
ELSE never saw it and the program dispatched through a freed box. This is the "N-1 sites" failure
mode this same test file already records twice; the destination wording ("an interface local") is
what proves which of the two decl-init sites fired.

**PARENTHESES defeated the whole gate**, also found by review - the issue file's own verbatim
repro plus one token (`q.p = (makeBox().t);`) still use-after-freed. A parenthesized primary is
rebuilt in `ParsePostfixExpression` from a side channel that carried only type and storage, so
every provenance flag was dropped. Fixed by widening that side channel
(`lastParenExprFromOwningTempField` and friends) rather than by touching the gate. The lessons
file's syntax-axis note is exactly this: a construct's SPELLINGS are an axis separate from its
types and scopes, and one token is enough.

**Global scope is excluded on purpose.** `Node* gp = makeBox().t;` at file scope already had a
truer diagnostic ("global variable initializer must be a compile-time constant"); the new guard
fired first and MASKED it with a message naming a "local". Both decl-init call sites are gated on
`!global_scope` so the pre-existing message survives.

**`OwningTempParent` is the load-bearing half of the polarity.** `FromOwningTempField` alone is
also set for a BORROWED element (`l.get(0).t`, an `alias` return), where nothing is freed at the
end of the statement and binding to a local is legal - measured on both binaries. A gate keyed on
`FromOwningTempField` alone would have false-rejected it; `Test/test_move.cb::
temp_uniq_borrowed_elem_value` freezes that accept.

**Ordering matters and was got wrong once.** The new leg sits AFTER the existing type-name leg at
each of the three shared sites. Placed before it, a dtor-BEARING pointee took the new wording and
three legs of `Test/errors/err_unique_borrow_into_field.cb` broke. Only two cells change wording
for a dtor-bearing pointee, and both were ACCEPTED before (brace-init into a borrowing field, and
a `unique IShape` temp field into a plain interface field) - pure tightenings.

**Measurement method worth reusing.** `MallocScribble=1` on macOS turns this whole family from
"prints a plausible value" into a one-bit discriminator: a use-after-free read returns
`1431655765` (the 0x55 fill). Every cell of the coverage matrix was classified with it.

**Deliberately NOT closed**, each pre-existing and measured identical on both binaries: a
same-type C-style cast, a `??` join, a `?:` join, an ARRAY AGGREGATE initializer, and a call
ARGUMENT that stores. All five are filed together as
`temp-unique-field-escapes-through-unguarded-spellings` (P1, CLOSED 2026-08-05 by `fix/tempuniq`,
file deleted) with a measured pre/post pair
each and a fix order. Correction to the round-1 framing: the argument case is **partly**
closable, not indistinguishable - a `unique T*` or `move T*` parameter states the ownership
claim at the call site (both measured still broken); only a plain `T*` parameter is undecidable
there. Separately, [[lambda-body-owning-temp-never-destructed]] (P2) records that an owning temp
in a LAMBDA body is never destructed at all - a leak with a different root, which no guard can
see because the provenance is never set.

Bar: macOS arm64 Release `./test.sh` 598 passed / 0 failed / 8 skipped, `example_mac.sh` 35 passed
/ 0 failed, `leaks --atExit` on `Test/test_move.cb` unchanged at 13 leaks / 256 bytes across
(pre-binary, pre-tests), (post-binary, pre-tests) and (post-binary, post-tests).

## Landed: `fix/codeval-store` (2026-08-04) - a code VALUE no longer converts to a data pointer at a store

Closes `p1/code-value-into-data-pointer-outside-overload-resolution` and DELETES the file. That
issue was recorded by review round 1 of `fix/funcptr-rebind`, which closed the OVERLOAD-BINDING
path of this defect class and deliberately did not reach the store paths. All three filed repros
verified on the merge-base binary first: compile rc 0, run **exit 138**, no diagnostic.

The two predicates the scorer already uses are now read from the DESTINATION side by one shared
helper (`cflat/LLVMBackend.h`), so the argument path and the store paths cannot drift:

    bool CodeValueIntoDataDestination(const NamedVariable& src, const TypeAndValue& dest) const
    { return ArgumentIsCodeValue(src) && ParameterStoresData(dest); }

**NINE gate call sites in `cflat/MainListener.h`, along TWO axes** (thirteen syntactic entry
points - the field-default site is shared by five default-constructor emitters). The first cut had three and claimed
they were "the whole set" on the strength of `b.p = w` being rejected. That claim was false and
review round 1 measured it: the field, element, nested-field and global stores do reach the
assignment site, but only through the `=` OPERATOR. Write the same store as a brace initializer, a
field default or a parameter default and none of the three sites is on the path - four more
spellings, all still exit 138 with no diagnostic after the first cut landed. This is the syntax
axis, recorded in `internal/fix-issue-lessons.md` as the twin of the name-spelling axis, and it
was missed the same way: the type axis (`Rec*`, `Rec**`, `char*`, `void*`, `string`, alias,
element, global, nested) was enumerated exhaustively and the SYNTAX by which a value arrives was
not enumerated at all.

The TYPE-axis sites (the `=` operator, a declarator, a return):

- the declarator initializer, inside the `rightNV` scope beside `RejectRawPointerToArrayView`;
- `ParseAssignment`, which carries the `=` spelling of the field, element, nested-field and global
  stores, and the compound operators;
- the `return` leg, against `currentFunctionReturnTV`, before `LoadNamedVariable(returnNV)`.

The SYNTAX-axis sites, each a separate lowering path reached by none of the above:

- `EmitOneFieldInit` - the named brace field init, which is where `= {f=v}`, bare `{f=v}`,
  `new T {f=v}` and the `<Tag attr=>` sugar all funnel;
- `EmitPositionalFixedArrayInit` - `T*[N] a = {w, w}`. **The element type is derived from the
  array's star count** (`Pointer + ElemPointer`), not from `ElemPointer` alone: the view path's
  derivation is correct for `T[]`, where the array itself is a pointer, and copying it to the FIXED
  path silently disarmed the guard for every `T*[N]`. Caught by the leg, not by reasoning;
- `EmitArrayViewInferredInit` - the length-inferred view. **Its live spelling is a `string` element,
  not a pointer one**, which an earlier draft of this record got wrong: `Rec*[] v = {w, w}` never
  reaches this gate at all (the pre-existing "a fixed array is not assignable from a pointer" fires
  first, identically on both binaries), so citing it as the example described coverage that does not
  exist. The spelling that DOES reach it is `string[] v = {w, w}`, and on the merge base it is a
  SILENT MISCOMPILE - exit 0, `length()` reads 1, `.data()` prints empty - not a crash. Proven from
  `--no-opt` IR rather than from the probe value: the element store is
  `store %string %1, ptr %arrview_elem` where `%1` wraps `@_ro_double_double_`, i.e. the callee's
  machine code read as a NUL-terminated buffer, the same shape as the `char*` -> `string` coercion
  the argument gate already refuses;
- `EmitGlobalFixedArrayInit` - the global twin. Its elements fold to constants inside a throwaway
  function with the builder redirected, so the reject is RECORDED and raised after
  `RestoreBuilderState`. `LogErrorContext` throws, and unwinding from inside that loop would leave
  the insert point in the temp function - the "IR bracket left open on the unwind path" failure
  this repo has already paid for three times;
- the FIVE default-constructor emitters share `ParseFieldDefaultInitializer`:
  `ParseStructDefinition`, `ParseClassDefinition`, `ParseConstructorDefinition`, and BOTH `program`
  emitters (`ParseProgramDefinition` and `ParseImportedProgramDefinition`). **None of them is a
  union-carrying emitter** - an earlier draft of this record and of the code comment said so and
  was simply wrong; the union branch of `ParseStructDefinition` returns a zeroed value and never
  runs a field-init loop at all. Gating only the struct ones left the class spelling reproducing,
  measured. The `program` pair is reached too: `program P { Rec* p = ro; }` now emits this gate's
  message where the merge base emitted an unrelated `run()`-generation error;
- the omitted-argument forwarding wrapper - `f(Rec* p = ro)` filled the parameter slot with a code
  address and the body wrote through it.

**The shape check inside `ArgumentIsCodeValue` is what makes a destination-side rejection safe.**
`function<T>*` is the ADDRESS of a slot and `function<T>[N]` decays to one; a wholesale rejection
of `function<T>*` was landed once in this repo and had to be reverted. Shape 0 is the only
rejecting shape, and `ParameterStoresData` answers false for a function-pointer destination, so
storing a code value into a code slot of every spelling is untouched.

RATIFIED, and the point at which the store rule now agrees with the argument rule: `void* v = w;`
is an error. `fix/funcptr-close` already ratified the same tightening for a `void*` PARAMETER
("write a cast"); until now the two spellings of one rule disagreed. Measured: nothing in `Test/`,
`example/` or the swept corpus depended on it.

Two cells were judged separately rather than swept in with the rest:

- **Compound `+=` gets its OWN wording, because the question is different.** It is not a store: the
  code address is consumed as an INTEGER OFFSET (`ptrtoint` of the callee added to the pointer),
  forging an address that is neither the function nor the pointee. Established from `--no-opt` IR
  (`%2 = ptrtoint ptr %1 to i64  %3 = ptrtoint ptr %0 to i64 ... store ptr %4, ptr %r`), not from a
  probe value - the repo has added a site to a rejection on a strange decimal once, with a message
  that was false where it fired. "code does not convert to a data pointer" would have been that
  message here, so the compound arm says "a code address is not an offset" instead. Likewise the
  cast advice is dropped for the `string` destination, where `(string)` of a raw value is itself
  rejected.
- **A `?:` / `??` JOIN is out of scope and is filed at P1**, not deferred silently, as
  [[join-erases-code-value-evidence-at-every-gate]]. The measurement that settles it is the
  ARGUMENT leg: `lam(c ? w : n)` into a `Rec*` parameter is exit 138 on the merge base AND on this
  branch, while the bare `lam(w)` has been diagnosed since `fix/funcptr-rebind`. A join erases the
  source evidence every code-value predicate reads, so it defeats the already-landed argument gate
  exactly as it defeats these store sites - a source-side recording problem, not a missing
  destination reader, and it must serve all three gates at once or the halves drift.

Interface destinations need no work: `IThing t = w;` and `IThing* t = w;` are already rejected by
pre-existing guards, identically on both binaries. A `list<Rec*>.add(w)` is refused by the scorer.

PRE-EXISTING RESIDUE noticed while doing this, neither caused nor worsened here, recorded so the
next visitor does not re-derive it:

- **The deferred-raise hazard is only half fixed in `EmitGlobalFixedArrayInit`.** The code-value
  reject added here is recorded and raised after `RestoreBuilderState`, but the pre-existing
  `LogErrorContext(fi, "global array initializer elements must be compile-time constants")` one
  line below it still throws from INSIDE the redirected-builder region - the exact hazard this
  change documents avoiding. Its `ok = false; break;` is therefore dead code, and the unwind skips
  `tmpFn->eraseFromParent()` and the state restore. Pre-existing, untouched, and it wants the same
  record-then-raise treatment.
- **A `Lambda<>` field default segfaults, and correctly does NOT fire this gate.**
  `struct S { Lambda<double(double)> lf = ro; };` is exit 139 on `6e9ab46` AND here. The
  destination is a closure slot, so `ParameterStoresData` answers false and the gate is right to
  stay out of the way - this is a neighbouring defect in the fat-closure field-default lowering,
  not a hole in the code-value rule. Not filed separately; noted here because it is the first
  thing a reader will hit when probing the field-default site.

WHAT EACH CELL ACTUALLY WAS, since the first cut's report lumped them together as "~19 reject
cells, all exit 138 on the merge base" and that was overstated. Three categories, and they are
ranked differently:

- **Memory-unsafe closed (exit 138 on the merge base, diagnosed now)** - 19 cells: declarator,
  assignment, return, struct field, class field, global store, array element, nested field,
  `using` alias destination, global declarator, `char*`, and the source spellings (named function,
  `move`d value, `function<>` field read, parameter, call result, `Lambda<>` local), plus the four
  syntax-axis spellings review round 1 found (brace field init in three spellings, array
  aggregate local and global, struct and class field default, parameter default).
- **Ratified TIGHTENINGS (the merge base compiles and RUNS them, exit 0)** - 3 cells: `void* v = w`
  (now agreeing with the `void*` PARAMETER rule `fix/funcptr-close` already ratified), `Rec** pp = w`,
  and `r += w`. These are not closed crashes; they are programs that stop compiling. Nothing in the
  swept corpus depended on any of them.
- **Message REPLACEMENTS (already rejected on the merge base, different wording now)** - the
  lambda-literal and `Lambda<>` destinations, which said "cannot initialize pointer 'r' with a
  value of type 'Rec'" - factually wrong, the RHS is not a `Rec`. The `string`, interface and
  container cells did not compile on the merge base either and are unchanged or untouched.

EVIDENCE. The accept set was built and frozen as value legs BEFORE the guard, and every cell was
run on the merge-base binary first. `Test/test_function_ptr.cb` gains
`testCodeValueStoreAccepts()` and `testCodeValueAggregateStoreAccepts()` (44 legs); the file runs
67/67 and its output is BYTE-IDENTICAL on both binaries - these legs discriminate against an over-broad guard, not against master, which is
why that pairing is the right non-vacuity check for them. The most dangerous accepted shape is a
function pointer whose RETURN type is a data pointer (`function<StoreRec*(int)>`): its call result
is genuine data and reaches all four gated destinations. `Test/errors/err_data_pointer_to_closure_param.cb`
gains 21 reject legs; each was mutation-tested individually (mutate one expectation, the file must
flip to exit 1) and each was extracted to a standalone file and shown to FAIL on the merge base and
PASS here. The file runs 58 PASS legs in total.

DIAGNOSTIC ACCURACY, all three found by review round 1 and all three cases of a message describing
something the source did not write:

- The destination is spelled at FULL pointer depth. A `Rec**` was being reported as `Rec*`, with a
  `'(Rec*)'` remedy - naming a type the user never wrote. `CodeValueDestSpelling` renders `Rec**`,
  and an array VIEW as `int[]` rather than the `int*` the shared pointer-speller gives.
- The cast escape is advised only where one COMPILES, verified per spelling. There is none for a
  `T[]` view: `(int*)w` then fails the view's own "cannot bind a raw pointer 'T*' to an array-view"
  check and `(int[])w` is refused outright, so the advice was sending the user into a second error.
  Dropped there and for `string`, kept for `Rec*` / `Rec**` / `void*`, each measured.
- A COMPOUND operator on a non-pointer destination gets its own wording. `string +=` is
  concatenation, not pointer arithmetic, so "a code address is not an offset" - true for `Rec* +=`,
  and proven there from IR - was false on `string`. On the merge base that spelling reached the
  LLVM verifier as "GEP indexes must be integers" with no source location. Differential A/B over all 446 `.cb` in `Test/` and `example/`: zero real differences
under `--check` and zero under a real `-o` compile (run separately, since `--check` cannot see a
codegen crash); after the new legs landed, exactly one - the intended reject file. macOS arm64
Release **598 / 0 / 8** and `example_mac.sh` **35 / 0**, re-run on the final base after this
branch was rebased twice (`6e9ab46` -> `312d202` -> `a846e6e`) while it was in review; the
sweep figures above were taken against `6e9ab46`, and the two reject/accept regression files
plus a reject/accept spot-check were re-measured on each later base.

No new serialized field, so the `--init` round-trip is untouched; `test.sh` runs `--init-local` and
covers the warm-cache path.

## Landed: `fix/iface-selfassign` (2026-08-05) - receiver identity taken from the BOXED OBJECT, settled at end of body

Closes `p1/interface-field-self-assign-false-positive` and DELETES the file. `ic.slot = ia.slot`
between two boxed interface receivers with the SAME field name compiled clean and aborted (compile
rc 0, run rc 133/134, no diagnostic); the different-NAME control already rejected. Root cause held
exactly as filed: the interface-field materialization branch never assigns `CallerName`, so both
sides carry `""` and the same `FieldName`, and `selfFieldAssign` reads two different receivers as a
self-assign.

**The FIRST attempt (2026-08-01, reverted) compared variable NAMES.** The file's "what will not
work" list was re-verified rather than assumed, and all three entries hold: two NAMES can denote
one object; the interface LOCALS' storage is two allocas for two boxes of one object; and a bare
`Value` compare of the field address false-rejects even the true self-assign, since each access
re-loads the fat pointer.

**The mechanism: resolve each side's fat pointer back to the OBJECT its box wraps.** An interface
field address is a GEP chain off `extractvalue fat, 1`, so the fat value is recoverable
(`ResolveBoxedObjectOfInterfaceField`, `cflat/MainListener.h`). A fat value that is itself a
registered box answers directly from `interfaceBoxRecords_`; a fat value LOADED out of an interface
local is traced to the one box stored into that slot (`SoleStoreIntoSlot`, `cflat/LLVMBackend.h`).
The two data pointers then go to the EXISTING `ProvablyDifferentObjects` that `fix/uniq-global`
added - two distinct alloca/global roots are distinct objects. This is what makes it more than a
`Value` compare: two distinct boxes of ONE object are two allocas holding ONE data pointer, so they
answer "same" and keep compiling. Nothing resolvable answers "cannot tell" and is ACCEPTED.

**The verdict is DEFERRED to the end of the body, and that is load-bearing, not tidiness.** At the
store, a receiver's slot has only the stores emitted SO FAR. A loop can rebind it afterwards:

```cflat
ISlot ia = a; ISlot ic = c;
for (int k = 0; k < 2; k = k + 1) { if (k > 0) { ic.slot = ia.slot; } ia = ic; }
```
compiles, runs, and frees each pointee exactly once on master - on the only iteration that runs the
store, both receivers box ONE object. An at-site rule sees one store into `ia` and false-rejects it.
So the site RECORDS (recording cannot reject, so a missed shape degrades to today's missing
diagnostic) and `RunUniqueIfaceFieldStoreCheck` settles it at the same end-of-body hook as
`RunInterfaceReturnDangleCheck` / `RunNullIfaceDispatchCheck`, re-running `SoleStoreIntoSlot` where
a later rebinding is finally visible. `InterfaceBoxProvenanceUnknown` was NOT used - counting stores
on the slot answers the same question from the IR and also covers an escaping address.

**A NULL fat store is skipped when counting** - `I i = default; i = a;` is one binding written in
two statements, and a slot the null store reaches has no implementation, so an access through it
faults before ownership can matter. Without this the two-statement spelling stayed undiagnosed.

**The change is purely ADDITIVE: `selfFieldAssign`, `sameFieldStore`, `ProvablyDifferentSlots` and
every emission path are untouched.** So the four sibling `selfUniqueFieldAssign` traps the issue
file audited keep their exact polarity, and an ACCEPTED program's IR is byte-identical. The only
other edits are three `Reject*` helpers split into `Format*` + `Reject*` so the deferred site can
build the same message text (messages verbatim unchanged - the pre-existing legs still match).

Deliberate residue, each measured accepted on BOTH binaries and left that way: an interface
receiver boxed from a POINTER or from `new` (the data root is a load / a heap call, not an
alloca); an interface PARAMETER or a call RESULT (no box record); two boxes of two SUB-OBJECTS of
one container (one root - nothing to prove); a GLOBAL interface local (its slot is a
`GlobalVariable` whose stores span functions); and a flagged store inside a LAMBDA body (the
end-of-body hook runs only in the named-function path - the same architectural gap shared by
`RunNullDerefDataflow` and `RunInterfaceReturnDangleCheck`). All five are missing diagnostics,
never false rejections; they are consolidated in
[[unique-field-to-field-interface-receiver-residues]].

Evidence: a 20-cell matrix in `scratch/ifs_*` (repro, both witnesses from the issue file with
destructor counts, true self-assign, different-name control, pointer/`new`/parameter/return/
branch-rebound/global receivers, `move` spelling, non-`unique` field, owning-VALUE `string` field,
two interfaces over one object, sub-object receivers, a method receiver, and the loop-rebind
program above), every cell measured pre and post. `--check` differential sweep over **534 `.cb` in
`Test/`, `example/` and `cflat/core/`: 32 diffs, 31 of them the binary's own core path inside an
"imported file not found" message and ONE behavioural - the new test legs.**

Legs: three file-scope `expect_error` blocks in `Test/errors/err_unique_field_to_field.cb` (the
plain shape, the interface shape, the two-statement binding), each wrapping a whole FUNCTION because
the diagnostic is deferred to end of body - a scoped block INSIDE the function closes first. Each was
mutation-tested individually to a self-assign and each flipped the file to exit 1; the two new legs
FAIL on the pre-fix binary. Nine value legs in
`Test/test_move.cb::testUniqueFieldStoreRemedies` pin the accept set with destructor counts
(`uniq_iface_twobox_*`, `uniq_iface_alias_*`, `uniq_iface_rebound_*`). Those pass on both binaries
by construction, so they were mutation-tested against the COMPILER instead: resolving to the
interface local's storage rather than the boxed object flips `uniq_iface_twobox_value`, and
deleting the end-of-body re-verification flips `uniq_iface_rebound_value` - the two defects this
design exists to avoid.

Bar: macOS arm64 Release `./test.sh` **598 passed / 0 failed / 8 skipped**, `example_mac.sh`
**35 passed / 0 failed**, `test_lsp.sh` **152 passed / 0 failed**. `leaks --atExit` on
`Test/test_move.cb` is **13 leaks / 256 bytes** on (pre-binary, pre-tests), (post-binary,
pre-tests) and (post-binary, post-tests) - unchanged, new legs add none. No new serialized field,
so the `--init` round-trip is untouched; verified directly on a warm `--init-local` cache (all
three reject legs still fire).

> Measured in passing and NOT part of this change: those leak figures are the WARM-cache numbers.
> The same source compiled with a COLD cache gives 15 leaks / 304 bytes, on the pre-fix binary and
> the post-fix binary alike - so the `--init` bitcode cache changes generated code. Pre-existing,
> filed for its own investigation, and worth knowing before quoting a leak baseline from any of the
> records above.

## Landed: `fix/tempuniq` (2026-08-05) - a temp's `unique` field no longer escapes through a cast, a join, an array aggregate or a sink parameter

Closes `p1/temp-unique-field-escapes-through-unguarded-spellings` and DELETES the file. Every row
of that file was RE-MEASURED on a verified `14097e1` PRE binary before any guard was written (the
file's own table was taken on `6e9ab46`); identity confirmed the way the lessons file requires - the
PRE binary ACCEPTS every spelling and the programs print the `MallocScribble=1` fill at runtime.

MECHANISM - **record-then-resolve, keyed by VALUE IDENTITY**, the third use of this shape in this
repo and deliberately a PARALLEL ledger rather than an extension of `codeValues_` (the sibling
`fix/widengate` owns that neighbourhood). One vector on `LLVMBackend`, `owningTempUniqueFields_`,
parked and cleared exactly where `codeValueDataCasts_` is - per-function clear, `SaveBuilderState` /
`RestoreBuilderState` pair, `ResetForReanalysis`, and **statement-scoped, retired by
`FlushOwnedTemps` at the block-item boundary**. That scope is not defensive: it is the SAME boundary
that runs the temp's destructor, so an entry can never outlive the dangle it describes.

The read is ledgered in the by-value member-access branch of `MainListener.h`, three lines from
where `FromOwningTempField` / `OwningTempParent` are set, under exactly the predicate the persist
sites already used - extracted as `DeclaredOwningTempUniqueFieldRead`. **Recording cannot reject**,
so a read this misses degrades to no diagnostic, never to a false rejection.
`IsOwningTempUniqueFieldEscape` then answers from the declared facts OR from
`JoinCarriesOwningTempUniqueField`, which walks a PHI's incoming values for `?:` and
`nullCoalesceJoins_`'s recorded arms for `??`, recursing with the same depth cap that terminates a
PHI cycle in a loop. Because all FIVE pre-existing persist sites read that one predicate, the cast
and join spellings were closed at every one of them with no per-site work.

**Why value identity is safe here and needed occurrence keying there.** `codeValueDataCasts_` had to
be statement-scoped because a NAMED FUNCTION is one shared module-level constant, so every mention of
`ro` in a body is the same `llvm::Value` - the residual that became
[[same-statement-cast-launders-join-code-evidence]]. A temp field READ is a distinct `ExtractValue`
instruction per occurrence, so two spellings of `makeBox().t` in one statement are two different
values and cannot launder each other. The scoping here is belt-and-braces, not load-bearing.

FOUR STEPS, each landed with its own frozen accept set BEFORE its guard, in the order the issue
file specified:

1. **Array aggregate initializer.** `Node*[2] a = { makeBox().t, nullptr };` is lowered by
   `EmitPositionalFixedArrayInit`, NOT by `EmitOneFieldInit`, so the brace-init leg the previous fix
   added could never see it - it was a silent use-after-free even for the BARE spelling, which makes
   this the one step that was a plain missing call. Its array-VIEW twin `EmitArrayViewInferredInit`
   is a THIRD separate lowering and needed the same call; both legs quote the element INDEX, which
   is what proves the array site fired rather than a declarator gate. `EmitGlobalFixedArrayInit` is
   deliberately untouched: a global initializer already gets the truer pre-existing "must be
   compile-time constants" diagnostic, the same reason both decl-init sites are gated on
   `!global_scope`.
2. **Same-type C-style cast.** `ParseCastExpression` REUSES the operand's NamedVariable and
   overwrites `TypeAndValue` with the destination type, which drops `IsUnique` / `IsUniqueTypeArg` -
   the flags survive, the TYPE test does not. Fixed by ledgering the cast RESULT when the operand
   answered the predicate, mirroring `RegisterCodeValueDataCast` three lines away. A cast is the
   "I mean this" spelling, so its accept set was built first: casts off ordinary pointers, off
   BORROWED container elements (`(Node*)l.get(0).t`), and over joins of two ordinary reads are all
   untouched, because the ledger is keyed on the READ and only a read off an OWNING temp
   (`OwningTempParent`) ever enters it.
3. **`unique T*` / `move T*` parameters.** The claim is stated at the CALL SITE, so these are
   decidable there. One site: `RejectOwningTempUniqueFieldIntoSinkParam`, called from the top of
   `ApplyMoveParamTransfer`, which is the ONE shared helper both the free-function path
   (`ResolveAndCall`) and the INTERFACE-METHOD path already funnel through - so the two call kinds
   cannot drift. Gated on `param.Pointer && !param.IsAlias && (IsMove || IsUnique || IsUniqueTypeArg)`.
4. **Joins.** Served entirely by the ledger walk above; no new site - EXCEPT for the one arm
   position that must not answer, below.

**THE '??' FALLBACK ARM IS EXCLUDED FROM THE WALK, and that exclusion is measured, not cautious.**
The obvious polarity - "ANY arm answers yes", copied from `JoinCarriesCodeValue` - is wrong here,
and the ARM-POSITION probe is what caught it (`scratch/tu/arm_A..arm_D`, destructor counts, both
binaries):

| Arm position | pre-fix behaviour | verdict |
|---|---|---|
| `makeBox().t ?? nullptr` (LHS) | `v=garbage`, `dtors=1` | dangles - REJECT |
| `p ?? makeBox().t` (RHS/fallback) | `v=70` (LIVE), `dtors=0` | never freed - ACCEPT |
| `c > 0 ? makeBox().t : nullptr` (true arm) | `v=garbage`, `dtors=1` | dangles - REJECT |
| `c > 5 ? nullptr : makeBox().t` (taken false arm) | `v=garbage`, `dtors=1` | dangles - REJECT |

The asymmetry is real and has a mechanism: a `?:` arm gets an explicit `FlushOwnedTempsSince`
INSIDE the arm block - `FlushOwnedTempsSince`'s own comment says it exists because an arm block
does not dominate the join - while the `??` path makes no such call, so its `nullcoal_null` temps
are skipped by `OwnedTempDominatesHere` and never destructed at all. Rejecting the fallback arm
would have refused a program master compiles and runs correctly, which is the single
highest-cost failure mode in this workflow. The walk therefore follows only `Arms[0]` (the left
operand at the one `RegisterNullCoalesceJoin` call site), the leak is filed as
[[owning-temp-in-coalesce-fallback-arm-never-destructed]], and BOTH the code comment and that
file say the exclusion must be deleted in the same change that fixes the leak - at which point
the shape becomes a genuine use-after-free. `temp_uniq_accept_coalesce_fallback_arm_not_freed`
pins `dtors == 0` deliberately, so the leg goes red the moment the leak is fixed.

**One site was missed on the first pass and found by this fix's own matrix, not by review**:
`EmitOneFieldInit` skipped the escape reject whenever the destination field OWNS
(`!braceDestOwnsPointee`), on the assumption that the field-to-field source gate would catch it
there - and that gate reads the declared facts a cast or a join has already stripped, so
`Holder h = { slot = (Node*)makeBox().t };` still aborted (rc 134). The guard now runs whenever the
source gate did NOT fire, with the destination described as a `unique field` in that case. This is
the "N-1 sites" failure mode this test file already records three times.

**The join diagnostic needed its own wording, and the first cut printed an empty name.** A PHI has
no `CallerName`/`FieldName` to quote, so `DescribeUniqueFieldAccess` returned `""` and the message
read `cannot store unique field '' of a temporary`. It now says "a unique field of a temporary,
reached through a cast or a '?:' / '??' join" when there is no name - a rejection's message has to
be true of the site it fires at. The CAST legs keep the quoted name (the NamedVariable survives the
cast), which is itself the discriminator that tells the two paths apart in the test file.

MATRIX - **150 cells**, ten source spellings x fifteen destinations, every one measured on the PRE
binary before any guard was written. Spellings: bare, parenthesized, same-type cast, cast-of-paren,
paren-of-cast, `??`, `?:` (one temp arm), `?:` (two temp arms), cast-of-join, join-of-cast.
Destinations: local decl-init, assignment, `unique` field, plain field, global store, brace init,
fixed-array aggregate, array-view aggregate, return, `??=`, and the argument kinds (plain `T*`,
`unique T*`, `move T*`, constructor argument, `list.add`). **86 cells changed**, all from
memory-unsafe to diagnosed: exit 0 freed-then-read for the silent ones, and rc 134 (double free)
for the `unique`-field and brace-init destinations, which were the only spellings that were not
silent. Round-1 review caveat on the discriminator: the `MallocScribble` 0x55 fill
(`1431655765`) shows only in an ld64.lld-linked build; the PRE binary here links `Linking
(mach-o)` and shows allocator-REUSE values (e.g. `v=4`) instead, so the UAF was re-proven with
destructor counts plus a reallocation-aliasing witness (`raw` reads the fresh object's 99,
`same=1`), not with the fill. Do not compare the fill across differently-linked binaries.

**64 cells are unchanged, and they are exactly three things** - not a long tail:

- **14 cells: already rejected on PRE** - the `bare` and `paren` spellings at the seven
  destinations `fix/temp-uniq-borrow` already guarded (decl-init, assignment, `unique` field,
  brace init, global store, plain field, return).
- **40 cells: the plain-`T*` parameter**, in all four shapes it reaches (a free function, a
  global-storing callee, `list.add`, a constructor argument). This is the remainder the issue file
  itself declared undecidable at the call site, and it is filed as
  [[temp-unique-field-escapes-through-a-plain-pointer-parameter]] (P2, residue-not-regression
  precedent, re-rank hatch stated) rather than left as a footnote.
- **10 cells: `??=`**, which RETURNS before the shared store tail and so is not a persist site at
  all. Measured, and memory-unsafe in two different ways depending on spelling:
  `raw ??= makeBox().t` LEAKS (dtors=0 - the owning temp is never registered), while
  `raw ??= c > 0 ? makeBox().t : nullptr` is a use-after-free (dtors=1, garbage read). Both
  identical on both binaries. NOT filed as a new issue - the root is the seven skipped bookkeeping
  calls already recorded in [[coalesce-assign-skips-store-bookkeeping]], which this fix upgrades
  from a file of PREDICTIONS to one with a measured memory-unsafe repro. Adding `??=` to the list
  of guarded sites is not the fix; routing it through the tail is.

ACCEPT SET - frozen as value legs BEFORE the guards, per the ordering the lessons file requires.
**16 legs in `Test/test_move.cb::testTempUniqueFieldBorrowAccepts`**, and the whole file's output is
BYTE-IDENTICAL on the PRE and POST binaries (713/713 both) - these legs discriminate against an
over-broad guard, not against master, which is the right non-vacuity pairing for an accept set. The
cells: a cast off an ordinary pointer into a plain field and into an array element; a cast AND a `??`
join off a BORROWED container element's unique field (the polarity leg - `FromOwningTempField` is
set there too, so a ledger keyed on it alone would false-reject every cell); `?:`, `??` and
cast-over-join of two ordinary reads, plus a join stored into an array aggregate; the read-only
plain-`T*` parameter in bare and cast spellings with `dtors == 1` asserted either side; and `unique`
/ `move` SINK parameters fed a genuinely owned argument (`new Res()`, a moved-from local), again
with destructor counts, since those are the two parameter kinds the new call-site reject judges;
and the `??` FALLBACK arm, with `dtors == 0` asserted, per the table above.

EVIDENCE. `Test/errors/err_unique_borrow_into_field.cb` gains **11 reject legs** (34 PASS legs
total, file exits 0). Each was extracted to a standalone file and run on PRE, where all eleven
report `FAIL: expected error ... did not occur` - i.e. PRE compiled each one silently - and PASS in
isolation on POST, so no leg is satisfied by an earlier leg's error. Each was ALSO mutation-tested
individually in place (poison one expectation, the file must flip to exit 1): all 14 tail legs
flip. LEAKS on `Test/test_move.cb` are unchanged - **15 leaks / 304 bytes on both binaries** when
both are measured COLD (`--init-clear-local`). Worth recording because it corrects a trap: the
warm-cache number is 13/256 with run-to-run variance up to 16/320, so a PRE-cold vs POST-warm
comparison reads as a 2-leak improvement that the diff cannot possibly have caused - the cache
state, not the compiler, is the variable. Bar: macOS arm64 Release `./test.sh` **598 / 0 / 8** and
`example_mac.sh` **35 / 0**.

DIFFERENTIAL SWEEP: `--check` over all **534** `.cb` in `Test/`, `example/` and `cflat/core/` with
both binaries, diagnostics compared by normalized hash - **exactly one real difference, the intended
reject file**. Two protocol notes, both of which produced false alarms first: the two binaries must
be in the SAME `--init-local` cache state (a warm POST against a cold PRE reported seven `core/*.cb`
going rc 0 -> rc 1, which is the redeclaration a warm core cache produces and not a rejection at
all), and the runtime-core PATH has to be normalized out of the diagnostic text or every
Windows-only file diffs on its own "imported file not found" message.

NEIGHBOUR AXES probed beyond the matrix, each a measured pre/post pair: the WRITTEN `unique Node*`
field spelling (not just the generic type-arg one) through cast and join - UAF pre, diagnosed now;
a NESTED field (`makeOutr().inner.p`) through cast and join - same; a cast of a NAMED (non-temp)
`unique` field, which the pre-existing `IsUniqueFieldAlias` channel already rejects identically on
both binaries; the GLOBAL array aggregate, which keeps its truer pre-existing "must be compile-time
constants" message on both, confirming that leaving `EmitGlobalFixedArrayInit` alone was measured
rather than assumed; and a PHI reached through a LOOP, which terminates on the depth cap.

No new serialized field - the ledger is an `llvm::Value*` vector on the backend, not a
`TypeAndValue` / `StructData` / `AnnotationValue` member - so the `--init` cache round-trip is
untouched; `test.sh` runs `--init-local` and covers the warm-cache path.

## Landed: `fix/joinledger` (2026-08-05) - a `?:` / `??` JOIN no longer erases the code-value evidence

Closes `p1/join-erases-code-value-evidence-at-every-gate` and DELETES the file. Every repro was
re-measured per spelling on a verified `d93c359` PRE binary first (identity confirmed the way the
lessons file requires: it ACCEPTS the repros and the programs exit 138 at runtime).

MECHANISM - **record-then-resolve, keyed by VALUE IDENTITY**, the shape this repo already converged
on twice. Two ledgers on `LLVMBackend`, parked and cleared exactly where `nullCoalesceJoins_` /
`interfaceBoxRecords_` are (per-function clear, `SaveBuilderState` / `RestoreBuilderState` pair,
`ResetForReanalysis` clear) - NOT with `ownedNewTemps_`, since the argument gate does not
necessarily run before the end-of-expression flush:

- `codeValues_` - a value PROVEN to be code, recorded in `LoadNamedVariable` three lines from the
  fat-interface ledger it mirrors, gated on the gates' own `ArgumentIsCodeValue`. Recording cannot
  reject, so a read this misses degrades to no diagnostic, never to a false rejection.
- `codeValueDataCasts_` - a value an explicit cast to a data type produced. Load-bearing, not
  defensive: a ptr->ptr cast is a NO-OP under opaque pointers, so the cast result IS the ledgered
  read, and without the launder the escape hatch the rejection advises is itself refused. Proven by
  mutation - see the accept set below. **STATEMENT-scoped, retired by `FlushOwnedTemps` at the
  block-item boundary**, and that scope is itself memory-safety-critical: a NAMED FUNCTION is one
  module-level `llvm::Function` constant, so every mention of `ro` in a body is the SAME Value.
  Round 1 held this ledger for the whole function, and one `void* v = (void*)ro;` then laundered
  `ro` for every later gate in that function - `Rec* r = c ? ro : n;` accepted and exited 138 with
  the cast line present, rejected without it, and the same leak defeated the argument gate and the
  string-arm gate. Found by review round 1. The retire point is strictly later than every gate that
  reads the entry and no later than the next statement, so the accept cells `c ? (Rec*)ro : n` and
  `c ? (Rec*)(ro) : n` are untouched. **Reordering `isa<Function>` ahead of the launder check is NOT
  the fix** - the reviewer mutation-tested it and it false-rejects both of those. Residual, left
  deliberately and **memory-unsafe (exit 138)**: a laundering cast and a bare join of the same
  named function inside ONE statement still launder each other, and the reachable spelling is an
  ordinary two-argument call - `two((void*)ro, argc > 0 ? ro : n)` compiles clean and exits 138,
  while the same program with the cast hoisted to its own statement is diagnosed (round-2 probes
  `scratch/rev2/r2/g20`/`g21`). "Inherent" only to keying the launder on VALUE identity alone;
  occurrence keying (value + syntactic cast site) is an open direction. Filed as
  [[same-statement-cast-launders-join-code-evidence]] rather than left as a footnote here.

`ArgumentIsCodeValue` then answers through a join as well as from declared facts, and since every
gate (the NINE store sites, the argument gate, the return gate) already reads that one predicate,
all of them are served from one source of truth with no per-site work:

    if (FunctionPointerShapeOf(arg.TypeAndValue, &arg) != 0) return false;
    return ArgumentIsFunctionPointerish(arg) || JoinCarriesCodeValue(arg.Primary);

`JoinCarriesCodeValue` walks a PHI's incoming values for `?:` and `nullCoalesceJoins_`'s recorded
arms for `??` (whose arms are unrecoverable from the IR - it joins through a slot, so the joined
value is a plain load). It recurses through nested joins with a depth cap, so a PHI cycle in a loop
terminates. The widening is confined to `ArgumentIsCodeValue` and deliberately kept out of its
helper `ArgumentIsFunctionPointerish`, which the scorer's funcptr ACCEPT arm also reads - widening
THERE would have let a join start claiming function-pointer parameters.

**Overload resolution did move, and an earlier draft of this record and of the commit message said
it did not.** `ArgumentIsCodeValue` is read by the scorer's REFUTE arm, so a join argument now
refutes every data-parameter candidate it is scored against. Measured across the review's overload
probes, the movement is one-directional and bounded:

- A set where EVERY candidate takes a data parameter (`g(Rec*)` / `g(void*)`) went from silently
  SELECTING `g(void*)` and storing a code address there, to "no overload of 'g' matches" with a
  per-candidate line for both. That is accept -> diagnosed, and it is the same verdict the single
  candidate spelling already gave.
- A set that CONTAINS a candidate accepting code (`f(Rec*)` / `f(function<double(double)>)`) still
  binds the `function<>` one, identical on both binaries - the refute arm removes candidates, it
  never re-ranks the survivors.

No SILENT selection shift was found: every difference is a call that used to compile becoming a
diagnostic. Both cells are pinned - `cvj_overload_set_binds_funcptr` and
`cvj_overload_set_binds_data` on the accept side, and a reject leg pinning BOTH per-candidate
lines, since a single-candidate leg cannot show the whole set was refuted.

POLARITY - **any ONE code arm answers yes.** One arm is enough to write a code address into the
destination, and the join of two data pointers answers no because neither arm was ever ledgered. A
call RESULT is not code even when the callee is: the ledger records the callee read, and the
`CallInst` is a different value.

TWO SITES outside the predicate.

**`??=` is its own store path**, and on the merge base `r ??= w;` compiled clean and exited 138
while the bare `r = w;` was already diagnosed - the two spellings of one store disagreed. Found by
review round 1, and pre-existing rather than caused here. It emits its own compare/branch/store and
returns before the plain-`=` gate, and it parses its RHS through the lean VALUE path, which
discards the NamedVariable that gate reads - so it asks the ledger about the STORED VALUE directly
(`JoinArmCarriesCodeValue`), which answers for a `function<>` read, a bare function name and a join
alike, and leaves an explicitly cast RHS laundered like every other site. Its own mini accept set is
in `testCodeValueJoinAccepts` (`??=` into a function slot from a name and from a local, between data
pointers, and of a cast code value) and the 20 pre-existing `??=` uses in `Test/test_basic.cb` are
unchanged. `p1/codegen/coalesce-assign-skips-store-bookkeeping.md` documents seven OTHER bookkeeping calls
this path skips and stays at P2 - the memory-unsafe member is closed here, the rest are not.

The second, because it CONSUMES the evidence rather than erasing it: a `?:`
whose other arm is a `string` harmonizes the pointer arm through `WrapStringLiteralAsString`, so
the machine code is read as a NUL-terminated buffer and the join delivers a `string` VALUE with no
pointer left for any gate to question. On the merge base that is a SILENT MISCOMPILE (exit 0, empty
string), not a crash. `RejectCodeValueTernaryStringArm` refuses it in `UnifyTernaryArmTypes`, where
the arm value is still ledgered, with the arm wording rather than the store wording - the rejection
has to be true where it fires. Proof-only: an unledgered pointer arm (a literal, a `char*`) keeps
wrapping, and both spellings are accept legs.

MATRIX - 51 cells, every one measured on the PRE binary before any guard was written.
**29 cells changed**, all from memory-unsafe-or-silent to diagnosed: exit 138 for the pointer
destinations, 139 for the nested and false-leg spellings, and exit-0 silent miscompiles for the
`string`, `char*` and `void*` ones. Axes covered: both join kinds x {declarator, assignment,
argument, return, brace field, fixed-array element, array-view element, field default, parameter
default, global store}; arm PROVENANCE {local, parameter, struct field, array element, global,
alias-typed, bare function name, `Lambda<>` closure}; arm POSITION (true leg and false leg, and the
`??` left-hand side); and nesting {nested join, join-of-join, join under cast}.

ACCEPT SET - frozen as value legs BEFORE the guard, per the ordering the lessons file requires.
**22 cells, all identical pre and post**, now in `Test/test_function_ptr.cb::testCodeValueJoinAccepts`:
a join of two code values into a `function<>` slot (decl-init, argument and parameter positions,
both join kinds); arms read from a struct field and a fixed-array element into a code slot; a
`Lambda<>` join; `(T*)` / `(void*)` casts inside an arm and over the whole join; a join of two data
pointers; a join of funcptr CALL RESULTS whose return type is a data pointer; a `function<T>*` join
into `void*`; and the two legal `?:` string-arm harmonizations (a literal and a `char*`).

Two accept findings worth keeping, both from mutation rather than reasoning:

- **`(T*)w` and `(T*)(w)` are different programs.** The bare spelling materializes its OWN
  unledgered load inside `ParseCastExpression`, so it never needed the launder; the PARENTHESIZED
  operand casts the ledgered read itself, and with the launder removed it false-rejects a program
  master compiles and runs. `cvj_cast_paren_in_arm` is the leg that pins this - the bare spelling
  alone would have certified a dead check.
- **A cast OVER a join is protected by the shape check, not the launder**: the cast's own
  `TypeAndValue` is `T*`, so `FunctionPointerShapeOf` answers 1 and the predicate returns early.
- The `(T[])` array-view cast branch got NO launder: a `function<>` VALUE has `Pointer == false` and
  that branch already errors on a non-pointer source, so a launder there would be dead code.

RATIFIED, the join spelling of a rule both other spellings already carry: `void* v = c ? w : n;` is
an error. `fix/funcptr-close` ratified it for a `void*` PARAMETER and `fix/codeval-store` for a
`void*` STORE; the join spelling was the last one that disagreed. It is the one changed cell that
does NOT crash on the merge base (exit 0, the address is simply stored), so it is called out
separately rather than folded into the memory-unsafe count.

EVIDENCE / BAR: `./test.sh Release` 598 passed / 0 failed / 8 skipped; `bash example_mac.sh Release`
35 passed / 0 failed. Differential `--check` sweep of both binaries over all 534 `.cb` files in
`Test/`, `example/` and `cflat/core/`: ONE difference, `err_data_pointer_to_closure_param.cb` going
rc=1 -> rc=0, which is this change's own new legs starting to pass.

Two harness lessons paid for on the way to that number, both worth the next reader's attention:

- **The first sweep was VACUOUS and reported a clean zero.** It recorded
  `echo "$(basename $f) rc=$?"`, and the command substitution runs before `$?` is read - so every
  line recorded `basename`'s exit code, not the compiler's. It is the same mistake as piping the
  compiler into `head` and reading `$?`, in a new costume, and it printed exactly the answer the
  change wanted. Capture rc into a variable BEFORE any command substitution.
- **A warm `--init` cache produced 7 phantom diffs** - `memory.cb`, `os.cb`, `os.posix.cb`,
  `thread.cb`, `cruntime.cb`, `page_pool.cb`, `bucket_allocator.cb` all "newly" failing with
  `redeclaration of global '...' in the same scope`. The POST binary had a local cache from an
  earlier `--init-local` and the PRE binary did not; clearing it made all 7 pass. Both sides must
  be in the SAME cache state, and cold is the state to compare in.

Sixteen reject legs in
`Test/errors/err_data_pointer_to_closure_param.cb`, each mutation-tested INDIVIDUALLY - extracted to
its own file and run on PRE, where all sixteen report `FAIL: expected error ... did not occur`, i.e.
PRE compiled each one silently - and each verified to PASS in isolation on POST, so no leg is being
satisfied by an earlier leg's error.

RESIDUE. The neighbour audit found the MIRROR defect and filed it at P1 as
[[join-defeats-the-closure-widen-gate]]: a join of two `void*` values into a `Lambda<>` PARAMETER is
widened into the closure's code slot and called (exit 139 on `d93c359` and here), while the bare
`applyL(vp)` is diagnosed on both binaries. It is not closable from this ledger - `codeValues_`
records values proven to be CODE and answers "any arm", and that gate needs values proven to be
DATA answering "every arm". Writing that second guard here would have meant adding a rejection past
an accept set frozen for the opposite question.

Also measured and deliberately left: a `Lambda<>` (fat closure) arm joined against a thin
pointer is rejected by the PRE-EXISTING arm harmonizer ("ternary branches have incompatible types
'__closure_fat_ptr' and 'pointer'"), identically on both binaries. That is a different message from
this gate's and arguably a worse one, but it is a rejection, not a memory-unsafe accept, so it is
recorded here rather than re-worded.

---

## Landed: `fix/widengate` (2026-08-05) - a `?:` / `??` JOIN no longer defeats the CLOSURE-WIDEN gate

The declared MIRROR of `fix/joinledger`, on the gate that asks the opposite question. Closes
`join-defeats-the-closure-widen-gate` (file deleted). Measured on a verified `14097e1` Release PRE
binary kept outside the repo at `/tmp/cflat-pre-widengate/cflat`.

ROOT CAUSE, confirmed. `WidenToClosureFatChecked` widens a call argument into a fat `Lambda<>`
parameter's CODE slot and rejects only what `ArgumentIsProvablyDataPointer` can PROVE is data. That
polarity is correct and stays. Its only positive evidence was `arg.TypeAndValue.Pointer` - a
DECLARED fact - and a join carries no declared facts, so `applyL(c ? vp : vq)` fell through to
accept and the `void*` was called as code. Exit 139, no diagnostic, while the bare `applyL(vp)` was
already diagnosed on both binaries.

MECHANISM. Symmetric to `codeValues_`, and deliberately BESIDE it - nothing in the code-value
ledger or `JoinCarriesCodeValue` was rewired:

- `dataValues_` - values PROVEN to be DATA, recorded in `LoadNamedVariable` on the line after the
  code-value record, gated on `ArgumentIsDataValue` (declared `Pointer`, and not
  `ArgumentIsFunctionPointerish`). Recording cannot reject, so a missed read degrades to the
  pre-existing accept. Same lifetime and the same four park/clear points as `codeValues_`
  (per-function clear, `SaveBuilderState`/`RestoreBuilderState`, `ResetForReanalysis`), and like it
  NOT statement-scoped. No `TypeAndValue`/`StructData`/`AnnotationValue` field was added, so the
  `--init` round-trip is untouched.
- `JoinDeliversDataValue` - the join reader, with the OPPOSITE quantifier to
  `JoinCarriesCodeValue`: EVERY arm must be proven data and at least one must be proven. A literal
  `nullptr` arm is NEUTRAL (null can never be code, and a data-plus-null join still calls a data
  address); an `llvm::Function` arm, an unledgered arm, and depth overflow all answer "unproven",
  which leaves the whole join unproven and the widen PERMITTED. Fail-open, matching the gate.
- The gate's predicate gained one fall-through line, so the thin `function<>` sibling
  (`CheckThinFnPtrArgProvenance`) and virtual dispatch (`LowerByValueArg`) inherit it and the four
  combinations keep ONE accept set.
- `dataValueCodeCasts_` - the MIRROR LAUNDER, added in round 1 to close a false rejection the review
  found (F1): `applyL((function<int(int)>)(c ? fp : fq))` ran correctly on PRE and hard-errored on
  the first cut. A ptr-to-ptr cast is a no-op under opaque pointers, so the value reaching the gate
  is still the PHI and the `IsFunctionPointer` early-out never sees the user's cast - while the
  per-ARM spelling, which carries the declared flag, was accepted. That is a bare-vs-join asymmetry
  in the escape hatch, the exact class this fix exists to remove. The cast is now recorded against
  the value at the same cast site as `codeValueDataCasts_` and consulted in two places: the arm walk
  (an arm cast to code stops it) and the gate's join fall-through. STATEMENT-SCOPED exactly like its
  mirror, retired by `FlushOwnedTemps` - and, after round 2, STRUCTURALLY barred from holding a
  shared constant. Statement scope alone was NOT enough: round 2 found (F4) that
  `(function<>)nullptr` casts the ONE shared `ConstantPointerNull`, so registering the cast source
  made an unrelated join's null arm in the SAME statement read as user-asserted code and re-opened
  the widen - the mirror of the hole this ledger's twin still has, created and then closed inside
  this change. `RegisterDataValueCodeCast` now refuses `ConstantPointerNull` and `llvm::Function`
  outright: null needs no launder (the arm walk already treats it as neutral), and refusing the
  function constant makes explicit an invariant that previously held only by short-circuit order.
  Two reject legs pin it, one with the cast on a DIFFERENT argument from the join - the only shape
  where a launder keyed on the shared null can connect them.

MATRIX, five buckets, 57 cells, every one measured on PRE first and then on POST, and recomputed
mechanically from the two result tables at each round rather than edited by hand. The first version
of this table was corrected twice by review - the original arithmetic was internally inconsistent
(19 unsafe minus 18 diagnosed does not leave three), and the corpus then grew by ten cells in round
1 and another ten in round 2. Note the round-2 cells landed in the DIAGNOSED bucket, not the
tightening one: `w02` and `w09` were both exit 139 on PRE, so closing F4 closed two more
memory-unsafe accepts rather than tightening a working program:

| bucket | n | cells |
|---|---|---|
| unsafe on PRE (clean compile, exit 139) -> DIAGNOSED | 23 | `?:` and `??`; direct call, method call, virtual dispatch, constructor argument, nested join, both argument positions, second position behind a legal lambda, thin `function<>` parameter, data-plus-literal-null, a `void*` PARAMETER arm, the field / fixed-array-element / global / `int*` arm spellings, a cast-to-DATA arm in either company, and the five shared-null launder shapes round 2 added |
| unsafe on PRE -> STILL UNSAFE (residues) | 5 | the two RETURN-path spellings, and three cast-to-CODE spellings where the user's assertion is simply false |
| correct on PRE -> UNCHANGED | 20 | the whole accept set, plus every escape-hatch spelling (over the join, per arm, over a `??`, into a thin parameter, through virtual dispatch, via a local) |
| correct on PRE -> REJECTED (ratified tightening) | 3 | a `void*` holding a code address, joined: directly, through a CALL-RESULT arm, through a DEREFERENCE arm |
| already rejected or unsupported on BOTH | 6 | bare `void*` argument (local and parameter), two arm-type harmonizer rejections, the two `list<Lambda<>>` verifier failures |

Two cells carry misleading names from the first cut and are classified by MEASUREMENT above, not by
their prefix: the `b07` parameter-arm cell was written into the accept series and is an unsafe
accept (now diagnosed), and `b16` likewise but is a residue.

THE TIGHTENING BUCKET is new in round 1 and is the reason it exists as a bucket: a class that
compiles and RUNS CORRECTLY on PRE and is refused here can only be found by enumerating it, and the
first cut had no cell for it at all. All three members are the same shape - a `void*` holding a code
address - and all three are RATIFIED rather than reverted, because the arms are declared `void*` and
nothing at the call site distinguishes them from the memory-unsafe legs, and because the BARE
spelling (`void* fp = (void*)hInc; applyL(fp);`) is a hard error on BOTH binaries. Refusing the join
is what makes the two spellings agree. The remedy is the escape hatch, carried as an accept leg in
both placements - and round 2 made the DIAGNOSTIC actually name it (both the fat and thin wordings
now end "If the value really holds a code address, assert it with an explicit cast:
'(function<...>)value'."). Round 2 caught that the record and a test comment had been claiming the
message named the cast when it did not; extending the message was the cheaper of the two fixes
offered, and `expect_error` is a substring match, so no existing leg was disturbed. All three are pinned as reject legs so the tightening
cannot silently regress.

ACCEPT-SET PROOF, frozen BEFORE the guard. `Test/test_function_ptr.cb` goes 68 -> 69 test functions
(`testDataJoinClosureAccepts`, 14 value legs) and passes on BOTH binaries, by construction: two
`function<>` values (the named first cell), two named functions, two lambdas, `??` of two
`function<>` values, a call-result arm, the method and virtual-dispatch spellings, the thin
parameter, the ESCAPE HATCH in both placements (cast over the whole join, cast per arm), and - the
cells a wrong quantifier breaks - three MIXED joins with exactly one proven-data arm, each measured
running correctly on PRE with the code arm taken.

QUANTIFIER MUTATION, the evidence the polarity is right. Flipping EVERY-arm to ANY-arm (both loops)
and rebuilding turns `Test/test_function_ptr.cb` into a HARD ERROR at the first mixed leg
(`2604,11`): `dj_tern_mixed_code_then_data` and `dj_tern_unknown_param_arm` become false rejections
of programs the merge base runs correctly, while the all-code legs stay green. Second mutation:
deleting the `RegisterDataValue` record site turns the FOURTEEN legs that predate round 2 red
individually and
leaves every accept leg green - so no leg is satisfied by a pre-existing guard. The two escape-hatch
accept legs have their own discriminator, measured rather than constructed: `dj_cast_over_join` is a
HARD ERROR on the round-1 binary that lacked `dataValueCodeCasts_`, which is the same evidence a
deletion mutation would give.

REJECT LEGS. SIXTEEN added to `Test/errors/err_data_pointer_to_closure_param.cb` (94 `PASS` lines
total) - thirteen closing memory-unsafe accepts and three pinning the ratified tightening. Each was
extracted to its own file and run on PRE, where every one reports
`FAIL: expected error ... did not occur` - PRE compiled all sixteen silently.

BAR. macOS arm64 Release `test.sh` **598 / 0 / 8**, `example_mac.sh` **35 / 0**. Cold differential
`--check` sweep (`--init-clear-local` first) over `Test/`, `example/` and `cflat/core/` - 534 files,
both binaries: the ONLY behavioural difference is
`Test/errors/err_data_pointer_to_closure_param.cb`, the intended new legs. Both the bar and the
sweep were RE-RUN after round 1 changed the polarity, not carried over. Every other diff is the
runtime-core PATH string inside "imported file not found" on Windows-only files, which differs
because the two binaries sit in different directories.

RESIDUES, all measured identical on both binaries and therefore not regressions:

- The RETURN path is still ungated for BOTH the join and the bare spelling - already filed as
  [[data-pointer-returned-as-closure-not-gated]], annotated there with the measurement and with the
  instruction to reuse these two predicates so the accept sets cannot drift.
- `list<Lambda<>>::add` of a raw pointer dies in the module verifier before any gate sees it,
  join or no join - annotated on
  [[list-of-function-element-into-closure-param-fails-verifier]]. The container axis of this accept
  set cannot be exercised in either direction until that is fixed.
- Three spellings where the user's cast to a code type is simply FALSE - an arm cast (`c ? (fn)vp
  : vq`), a whole-join cast over two genuine data pointers, and the plain arm-cast cell - still
  widen and exit 139, identically on both binaries. That is the escape hatch behaving as an
  ASSERTION: the cast is the user's own claim, exactly as `codeValueDataCasts_` is on the mirror
  side, and a false assertion is not something this gate can second-guess without destroying the
  hatch. Unchanged by both the round-1 launder and the round-2 shared-constant bar.
- THE UNPROVEN-ARM BRANCH IS A SAFETY PROPERTY, NOT AN ACCEPT PATH, and the first cut described it
  wrongly. A comment claimed a `void*` PARAMETER arm was "unproven code-or-data"; it is proven data
  by its declared type, and a reject leg proves it. Probing for a genuinely unproven DATA-pointer
  arm found none: local, parameter, struct field, fixed-array element, global, dereference, call
  result and cast-to-data are ALL proven. What actually leaves a join unproven is an arm whose
  declared type is CODE (which makes the join mixed - that is the accept path, and it is covered)
  or an arm the user cast to code. So the fail-open branch is there for spellings nobody has
  enumerated, and no accept leg depends on it - stated here rather than left as a false claim in a
  test comment.
- NOT DONE, deliberately: the join spelling's diagnostic says "a non-function pointer value" where
  the bare spelling says "a 'void*' value". `DescribeNonFunctionArgument` reads the argument's
  `TypeName`, which a join does not carry, so naming the arm's type would mean plumbing an arm
  through the gate. Judged not cheap enough to fold into this change.
- Nothing was added to `codeValueDataCasts_`, and NO cast-expression result is ledgered as DATA.
  That is deliberate, not an oversight: a ptr-to-ptr cast is a no-op under opaque pointers, so
  ledgering the cast RESULT would ledger the SOURCE, and for a named function that source is one
  module-level constant shared by every mention - the exact hazard behind
  [[same-statement-cast-launders-join-code-evidence]]. Measured consequence: none of the axis is
  lost. A cast-to-DATA arm is reached through the READ ledger anyway - both
  `applyL(c ? (void*)&q : vq)` and `applyL(c ? (void*)&q : nullptr)` compiled clean and exited 139
  on PRE and are diagnosed now - so the launder hole was not extended and no coverage was traded
  away for that.

## Landed: `fix/macos-sdkstamp` (2026-08-05) - the emitted `LC_BUILD_VERSION` sdk no longer depends on cache state

Closes `macos-sdk-stamp-differs-by-cache-state.md` (P2). `EmitExecutableMachO` resolved the
`-platform_version macos 11.0.0 <sdkVer>` argument's sdk field ONLY on the branch where the
harvested `macsdk` stub was absent; with the stub present (i.e. after any `--init`) it used a
hardcoded `"11.0"`. So the compiler cache - a pure-performance artifact by contract - decided the
SDK version stamped into every macOS executable cflat emits, and macOS gates real runtime
behaviour on that field.

What landed:

- `HarvestMacSystemStub` now writes `<cacheDir>/macsdk/SDKVersion` alongside `libSystem.tbd`. The
  stub's symbols come from the LIVE dyld shared cache of the running OS, so that OS version IS the
  SDK the binary is really built against. Sourced from `sysctlbyname("kern.osproductversion")` -
  NOT `xcrun` - so the self-contained (no Xcode / no CLT) property does not regress. Non-fatal.
- `sdkVer` is resolved on BOTH branches: stub present -> its provenance file, falling back to the
  live host version for a cache harvested by an older cflat; stub absent -> `xcrun
  --show-sdk-version`, same fallback. `"11.0"` survives only as the last-resort default.
- `TwoComponentVersion` trims to major.minor at the stamp. `kern.osproductversion` carries a patch
  digit (`26.5.2`) and `xcrun --show-sdk-version` does not (`26.5`); without the trim the field
  still differed by cache state, just one digit further right. Apple's own convention for this
  field is two components.
- `minos` stays `11.0.0`. The deployment target is the separate, correct knob and was not touched.

Verified by the issue's own repro - warm / cold-bitcode-stub-present / bare-no-stub all stamp
`minos 11.0, sdk 26.5`, and `leaks --atExit` on `Test/test_move.cb` converges on 16 leaks / 320
bytes across all three (previously 14/272 warm vs 16/320 bare). The cold path was the correct side,
as the issue predicted. `test.sh Release`: 598 passed, 0 failed, 8 skipped.

Standing consequence worth keeping: leak counts measured before this change are NOT comparable with
counts measured after it. The pre-fix warm numbers were taken under a false `sdk 11.0` stamp that
opted every binary into legacy libSystem behaviour.

## Landed: `fix/ptrarg-byval` (2026-08-05) - a POINTER argument no longer binds a by-value parameter

Closes `p1/crash/pointer-arg-binds-by-value-class-param` and DELETES the file. The filed repro
reproduced verbatim on the `f45c9ad` Release binary: `Circle* a; byVal(a);` against
`int byVal(Circle c)` exits 1 with `Call parameter type does not match function signature!` and no
`file(line,col):` prefix.

**The filed root cause held exactly.** `TypeAndValue::IsTypeMatch` compared `TypeName` and never
consulted `Pointer`, so `{Circle, Pointer}` scored a PERFECT 0 against `{Circle}`; the by-value arm
of `CreateOverloadedFunctionCall` then lowered through `Upconvert`, which has no ptr-to-struct arm
at all and returns the value unchanged, dropping a raw address into the struct slot. Two things the
file did not say were measured and are what set the scope: `MatchFunction` does no type filtering
whatsoever (it binds names to slots), so `IsTypeMatch` is the ONLY gate on the direct path; and
virtual dispatch does not use that gate at all - `ResolveInterfaceMethodSlot`'s lone-slot arm picks
by ARITY - so an interface method with a by-value class parameter fails identically and needs its
own proof.

**The end state is a LOCATED REJECTION, not an auto-dereference, and the choice was measured.**
Every neighbouring shape the language already rejects rather than adjusts: `int*` into `int`,
`int[]` into `int`, a pointer into a CONSTRUCTOR or an `operator+` parameter all give the located
"no overload ... matches" today. Rejecting brings the class/struct case into that existing set.

**The gate is ONE-SIDED and that is the whole safety argument.** `IsTypeMatch` refuses only
`Pointer && !other.Pointer` - an ARGUMENT that is a pointer at a by-value PARAMETER. The reverse,
`T` into a `T*` parameter, keeps matching, because it is a real working capability and not an
accident: `--no-opt` IR for `Circle v; byPtr(v);` is `call i32 @_byPtr_i32_CirclePtr_(ptr %v)` - the
caller's alloca, an implicit address-of. The predicate is therefore ASYMMETRIC and only correct at
its one caller's operand order (`argument.IsTypeMatch(parameter)`); the comment says so.

**What the rejection takes away, stated exactly - an earlier draft of this record claimed "nothing"
and that was WRONG.** For a MEMORY-CLASS CFlat by-value struct parameter the claim does hold: such a
parameter is an LLVM struct slot at EVERY size - a 16-field class was measured failing verification
exactly like a 1-field one - so no program containing that shape ever built, on any platform, and
the rejection cannot refuse working code. That is the argument for the Windows-only core the macOS
sweep never compiles, and it is the only place the argument applies.

Two measured shapes DID compile and run before and are now rejected, both reading wrong bytes:

- **`string*` into a by-value `string`** (matrix cell `ptrarg_b4`) built and ran: `s.length()` reads
  the length field out of a slot that only ever received the pointer, so the answer is whatever was
  left in it. ENVIRONMENT-dependent again, and this one is a trap worth stating: changing only the
  output FILENAME flips it between `1005` and `1003` on BOTH binaries, and `1003` is also the
  CORRECT answer - so a single run of this shape can look like a working program. It is not; the
  `*p` spelling is the one that computes `1003` on purpose.
- **A C by-value struct through the C binder.** `struct RP { int x; }; int rp_by_val(struct RP p);`
  called as `RP* p; rp_by_val(p)` builds, links and RUNS on the pre-fix binary, returning an
  ENVIRONMENT-dependent wrong value - `200` and `216` both observed, from different output
  directories - where `203` is correct. That is the same garbage class the deferred `T**` issue
  describes: the coerced scalar carries address bytes, so the number tracks the process image and
  must not be quoted as a fixed figure. A small C struct is lowered to a COERCED SCALAR by the C
  ABI, so nothing structural is left for the module verifier to catch. The `*p` spelling gives
  `203` on both binaries. This is the shape that matters for the Windows core, which passes by-value
  `POINT` / `RECT` / `GUID` through the same binder: a call site there holding a pointer compiled and
  ran before and is rejected now.

Both are silent wrong values, so rejecting them is the intended outcome rather than a cost - but the
blast radius is NOT zero and must not be described as such.

**Virtual dispatch got its own proof rather than a share of the scorer's, in TWO forms.**
`PointerArgIntoByValueParam` (`cflat/LLVMBackend.h`) proves a pointer argument whose TypeName IS the
by-value parameter's type, at a registered data structure, excluding `IsInterface` and
`IsFunctionPointer` parameters. The same-TypeName requirement is what keeps `char*` -> `string`
coercion and the `Circle*` -> `IShape` upcast out of it. `PointerArgIntoByValuePrimitiveParam` is
the second form and was added in round 2 after a review found the original verifier dump still live
for a by-value PRIMITIVE slot (`interface ITaker { int take(int n); }` fed an `int*`): a primitive
pointer argument carries an EMPTY CFlat TypeName, so the same-name test structurally cannot see it.
It requires the type flag AND the lowered LLVM type to agree the argument is a pointer - a by-value
struct's `Primary` is an alloca address, so the LLVM type alone would mistake one for a pointer -
and it takes a bare `nullptr` from the null CONSTANT, which carries no flag at all. Both fire from
`DiagnoseProvableInterfaceArgMismatch`, so BOTH slot-picking arms carry them: the lone-slot arm
(arity-only), and the multi-slot fallback that runs when the scorer - now correctly - ranks nothing.

**The INDIRECT call path needed a third site, also found by review.** Invoking a
`function<int(Circle)>` VALUE with a `Circle*` goes through neither `ComputeOverloadFunction` nor
`ResolveInterfaceMethodSlot` - `CreateIndirectCall` lowers the arguments itself - so it kept dumping
the verifier unlocated. `CheckIndirectCallArgShape` runs in both of that function's argument loops
(thin C-pointer and fat closure), AFTER the conversion attempt, so it judges a RESIDUE rather than
predicting one: `Upconvert` has no ptr-to-struct or ptr-to-arithmetic arm and returns the value
unchanged, and LLVM requires an exact per-argument type match, so a pointer left in a non-pointer
slot always fails verification. Only that one direction is judged; every other residual mismatch is
left exactly as it was. Scoped deliberately to argument checking so it stays disjoint from the
concurrent `fix/genfp-return` work on return lowering in the same area.

**Diagnostics render a mangled instantiation only when the rendering is PROVABLY writable source.**
A generic slot's TypeName is mangled (`Box__i32`), and quoting that raw in "declare the parameter as
'...'" is advice nobody can follow, so `DisplayNameOfMangledType` renders `Box<i32>` - verified
writable and binding the same instantiation as the `Box<int>` the user wrote. But the mangling is
AMBIGUOUS: `dictionary__string__int` is two sibling arguments and `Box__Box__i32` is one nested one,
and the string cannot tell them apart - the naive render gave `Box<Box, i32>`, which names no type.
So a name whose `__` segments include a TEMPLATE name (asked of `IsGenericTemplateKey`, plus a
namespace-tail scan, deliberately over-broad since a false "ambiguous" costs only prettiness) keeps
the RAW name and DROPS the advice clause entirely. Measured: `Box<i32>`, `Pair<i32, float>` and
`dictionary<string, i32>` render with advice; `Box<Box<int>>` and `Pair<Box<int>, float>` print raw
with no advice. Found by the round-2 review.

Predicate audit, per site: `IsTypeMatch` has exactly ONE caller (`ComputeOverloadFunction`).
`IsTypePromotion` was verified as an oracle before being imitated - its `Pointer != other.Pointer`
gate is correct for ITS question (a numeric widening into a differently-shaped slot is never valid)
and `int*`->`long`, `float*`->`double` were measured rejected on both binaries. The five other
`Parameters[0].TypeName == typeName` sites (`HasUserCopyMethod`, `HasCopyOverloadFor`,
`HasRealCopyOverloadFor`, `HasArrowOverloadFor`, and the member-exists probe) ask "does a method
exist for this TYPE NAME" - a receiver lookup, whose `this` is legitimately `T` or `T*` for the same
method - so adding a `Pointer` gate there would BREAK them; they are not the same defect.
`CompareUpconvert` (the empty-TypeName arm) is unreached by these shapes: a primitive pointer
argument arrives with no TypeName and was already rejected on both binaries.

Coverage matrix, 46 probes in `scratch/p/ptrarg_*` plus the round-2 batteries in `scratch/r2/` and
`scratch/verify/`, every cell measured pre and post. Fixed by this change: class, struct,
`Box<int>` instantiation, `string`, array-view (`Circle[]`), 16-field class, a C by-value struct
through the C binder;
sources spelled bare, through a member (`h.p`), through a call result; call shapes direct, method,
generic (`g<Circle>`); overload sets with the by-value candidate declared first (which used to
verifier-dump, and now correctly picks the `Circle*` sibling, matching what the swapped declaration
order already did); interface method, lone slot and two same-arity slots. Already rejected before
and unchanged: `int*`->`int`, `int[]`->`int`, constructor argument, `operator+` argument, `A*`->`B`,
raw `T*` into a `T[]` view. Accepted before and still accepted: `T*`->`T*`, `*a`->`T`, `&v`->`T*`,
`T`->`T*`, `nullptr`, `void*`, `move T*`, an alias-spelled `T*`, `list<Circle*>`, `g<Circle*>`,
`Circle*` into an `IShape` value parameter, `T[]`->`T*`, and the interface-method twins of the last
several. Also fixed, found by the round-1 review: a by-value PRIMITIVE slot on the virtual path
(`int*` and `nullptr` into an `int` parameter), and the indirect call through a `function<>` value
in both spellings (invoked directly, and invoked inside a callee that received the lambda as a
parameter) - all three were byte-identical unlocated verifier dumps before round 2.

Differential corpus sweep, both binaries, real `-o` compile AND run, **446 `.cb` across `Test/` and
`example/`: exactly ONE behavioural difference - `Test/test_basic.cb`, the new legs.** The harness
was validated non-vacuous by running the PRE binary TWICE: that self-check reproduces 9 files whose
stdout md5 varies run to run (timings, addresses, `hpc` and `macos` examples), and those same 9 are
the entire md5-only residue of the pre/post diff. Exit codes are identical on all 446.

The baseline is the VERIFIED MERGE-BASE, re-measured after the rebase: the figures above are from
`3c90ab4`, built in a detached worktree and confirmed by `git log` on that worktree. The sweep was
run once against `f45c9ad` before the rebase with the identical result, and the 46-cell coverage
matrix was regenerated against `3c90ab4` too - every cell lands where it did against `f45c9ad`, so
the intervening master commit moves none of them.

Legs: SIX scoped `expect_error` blocks in the new `Test/errors/err_pointer_arg_byvalue_param.cb`
(direct call, array-view source, interface dispatch on a class slot, interface dispatch on a
primitive slot, the `nullptr` spelling of that slot, and the indirect call through a `function<>`
value), each mutation-tested ALONE against the pre-fix binary and each flipping that file to exit 1.
Eleven value legs in `Test/test_basic.cb::testOverloadResolution`: one FIX leg
(`pab_ptr_arg_picks_ptr_overload` = 2003, which pre-fix took down the whole file with a verifier
dump) and ten ACCEPT legs that pass on both binaries by construction and were mutation-tested
against the COMPILER instead - each names the widening of the gate that would flip it (symmetric
gate, value-arg preference, explicit deref, `IsInterface` ignored, and for the round-2 gates: a
primitive value argument, a pointer PARAMETER, `nullptr` at a pointer parameter, and three
indirect-call shapes).

Round-2 accept set for the two new gates, 28 probes in `scratch/r2/` (`scratch/MATRIX_R2.txt`),
every accept cell measured identical pre and post: virtual dispatch with
`int`/`bool`/`double`/`string`/`char*`/`void*`/`int[]`/`int*` parameters, `nullptr` at `int*`,
`void*` and `Circle*` parameters, and indirect calls with `Circle*`, `int*`, `void*`, `char*`,
`string`, `int`, interface-value and dereferenced-value arguments, a named function assigned to a
`function<>`, and a `function<>` copied into another (`r2_i_funcptr_copy_OK` - renamed from a
misleading `thin_extern` label, which it never was).

**Both arms of `CreateIndirectCall` are crossed, and the FAT arm alone would not have proved it.**
Every `function<>` cell above lowers through the fat closure arm; the THIN arm needs a real C
function-pointer typedef, so it is crossed by two dedicated cells built against a `.c`/`.h` pair
(`r2_i_thinarm_value_OK`, `r2_i_thinarm_ptr_REJECT`, from the round-1 reviewer's probes): a by-value
`RP` argument through `int (*)(struct RP)` runs `103` on BOTH binaries, and the same call with an
`RP*` goes from an unlocated verifier dump to the located rejection. One further cell,
`r2_i_list_of_lambda_PREEXISTING_REJECT`, is VACUOUS for this gate and labelled so: it is refused on
both binaries at its declaration by an unrelated pre-existing rule and never reaches an indirect
call at all.

The only cells that moved are the intended rejections.

Deliberately NOT fixed, filed as [[double-pointer-arg-binds-single-pointer-param]]: `Circle**` into
a `Circle*` parameter still binds and returns garbage. Same predicate, DEPTH axis rather than
pointer-ness. It is held out on the polarity argument above - that rejection would refuse programs
that compile and run today, so it needs its own accept set - and because `ElemPointer`'s population
is not uniform across producers.

Bar: macOS arm64 Release `./test.sh` **600 passed / 0 failed / 8 skipped** (598 before the new
legs), `example_mac.sh` **35 passed / 0 failed**, `test_lsp.sh` **152 passed / 0 failed**. No new
serialized field, and every field the three guards read already round-trips in `LLVMBackend.cpp`
(`Pointer` `p`, `IsInterface` `if`, `IsFunctionPointer` `fp`, `IsArrayView` `av`, `ConstArraySize`
`as`); the error test was verified firing on a COLD cache and a warm one alike.
## Landed: `fix/genfp-return` (2026-08-05) - generic instantiation leaked its return TypeAndValue into the caller

Closes [[generic-funcptr-return-poisons-enclosing-return]] (file deleted). The filed root-cause
LEAD named `CoerceToFuncPtrReturn` and guessed at unscoped "current function returns a thin
funcptr" state; that was correct in direction and one field off in detail.

**Root cause.** `LLVMBackend::BuilderState` exists so that emitting a nested function mid-body
(lambda invoker, global init, program shim, and - the case here - a monomorphized generic
instantiation) can restore the enclosing function's return shape afterwards. It snapshotted THREE
of the four `currentFunctionReturn*` fields (`returnsOwned`, `returnIsArrayView`,
`returnTypeName`) and not the fourth, `currentFunctionReturnTV`, which was added later beside them
in `CreateFunction` (`LLVMBackend.h`, right after the `createFunctionBlock` call) and never joined
the snapshot. A generic producer whose return type is `function<>` / `Lambda<>` therefore left
`currentFunctionReturnTV.IsFunctionPointer` set, and `ParseJumpStatement` reads exactly that flag
to decide whether the ENCLOSING `return` goes through `CoerceToFuncPtrReturn`.

Two consequences that make the fix a one-liner rather than a new guard: nothing about the CALL
matters (the leak happens at instantiation, so discarding the result poisons the return just the
same - `return 42;` came out as `ret ptr bitcast (i8 42 to ptr)` from an `i32` function), and the
leak is bidirectional (a genuinely `function<>`-returning caller that instantiated a `Lambda<>`
producer emitted `ret %__closure_fat_ptr` against a `ptr` return).

**Fix.** Add `TypeAndValue returnTV` to `BuilderState` and save/restore it with the other three.
No predicate changed, no rejection added, no new state.

**Severity was understated in the filed row.** It said "no spelling is silently wrong, so P2".
True, but the measured axes were worse than the file's list in two places: a STRUCT-VALUE
enclosing return is a compiler SIGSEGV (exit 139, zero output), and a `string` enclosing return is
a FALSE REJECTION with the capturing-closure diagnostic ("call .toFunction() instead"), which
blames the user's code for a bug in the caller's bookkeeping. The round-1 review added a third:
an OWNING-CONTAINER enclosing return (`list<int> f() { ...mk<int>(1)...; return l; }`) is the same
false rejection with the same message, so the shape is any struct-by-value return, not `string`
specifically - re-measured on both binaries from `scratch/rev_p8_owned.cb` (PRE: the
`.toFunction()` diagnostic at (5,92); POST: prints `10 10.500000`). The re-triage to P1 by
`f45c9ad` was right.

Three further axes the review attacked were independently verified FIXED by this same change and
had not been listed: a producer instantiated inside an `if const` block
(`scratch/rev_p3_ifconst.cb`, PRE `Invalid bitcast` -> POST 11), inside a `program` shim body
(`rev_p7_prog.cb`, PRE `Invalid bitcast` -> POST `7 2`), and inside an `expect_error` scope
(`rev_p6_experr.cb`). The last is the sharpest: the expectation PASSES on both binaries, and PRE
then fails the module with `ret ptr null` against `i32` - the `LogError` unwind left the leaked TV
live and poisoned `main`'s own return, which is the concrete form of the RAII hazard noted below.

**Per-site audit of the same boundary - and it found a second leak, filed not fixed.** Take
`createFunctionBlock`'s clear-list as the definition of "per-function state" and check each entry
against `BuilderState`. The three return-shape siblings were already saved; all seven owned-temp /
provenance ledgers (`interfaceBoxRecords_`, `nullCoalesceJoins_`, `codeValues_`, `dataValues_`,
`codeValueDataCasts_`, `owningTempUniqueFields_`, `dataValueCodeCasts_`) are moved out and
restored. The three that are NOT saved are `aliasDomain_`, `aliasScopes_` and
`viewScopeByOrigin_` - the array-view noalias registry - and they have exactly this defect, with
teeth, because `TypeAndValue::NoaliasScopeId` is a vector INDEX: a generic instantiation mid-body
clears the vector and an enclosing local's id then names a different scope. Measured IR witness
(one view's store on `!alias.scope !0`, its own load on `!10`) in
[[nested-emission-clears-enclosing-alias-scope-registry]]. Deliberately NOT folded into this
commit: it changes emitted alias metadata for real programs and needs an `-O2` verification pass
that neither suite performs, which is a different bar from this fix's.

The other remaining hazard at this boundary is unchanged by this fix and worth knowing:
of the ten `SaveBuilderState()` call sites (`LLVMBackend.h:11748` and `:14649`; `MainListener.h`
4960, 6019, 8423, 9642, 18791, 19018, 25893, 26202 - counted by grep on the merged tree, excluding
the definition itself), only `FullBuilderStateScope` (`LLVMBackend.h:11743`, which owns 11748) is
RAII, so a `LogError` unwind past any of the other nine still leaks
whatever they were bracketing. That is a pre-existing structural item, not a regression, and it is
the reason the fix went into the shared struct rather than into a hand-rolled save at the generic
path.

**Two neighbouring defects probed and NOT fixed here** (deliberately, different roots, both
measured identical on the pre- and post-fix binaries):

- The zero-argument spelling `function<int(int)> mk<T>() { ... }` / `mk<int>()`. The filed issue
  called this "a second defect on the neighbouring spelling"; it is not a `function<>` defect at
  all. `int mk<T>() { return 5; }` fails identically, and the IR shows
  `%mk__i32 = type opaque` - an EMPTY argument list parses as a generic TYPE construction and is
  eaten by the ungated bare `tryPreDeclare` shell. Fixed later by `fix/generic-shell` (landed
  record below), which gates that shell; `mk<int>()` now runs and returns 5. The namespaced
  (`N.mk<int>()`) and method spellings already worked, which is exactly the gated-vs-ungated
  asymmetry that motivated the gate.
- A generic function called ABOVE its definition does not resolve. Filed as
  [[generic-function-cannot-be-forward-referenced]].

**Coverage.** `Test/test_function_ptr.cb` gained `testGenericFuncPtrProducer()` - 23 value legs
(`gfp_*`) across the enclosing-return-type axis (int, bool, struct value, string, owning container,
`function<>`, `Lambda<>`), the producer axis (thin, fat, `function<T(T)>`, non-generic control, two
producers, two instantiations of one template), the placement axis (nested loop, class method,
lambda body, generic enclosing function, nested generic producer) and the call-context axis
(discarded, as argument, immediately invoked, returned directly). SIX of the 23 pass on the pre-fix
binary too and are labelled accept-set in the file - `gfp_encl_nongeneric`, `gfp_thin_from_thin`,
`gfp_encl_fat_prod`, `gfp_as_argument`, `gfp_immediate_call`, `gfp_return_direct` - so they are
there to prove the fix breaks nothing, not to discriminate. The other 17 fail on the pre-fix binary
for the reason each one's comment states. `test.sh Release`: 600 passed, 0 failed, 8 skipped.
`example_mac.sh Release`: 35 passed, 0 failed.

## Landed: `fix/lamptr-generic` (2026-08-05) - a closure POINTER as a generic type argument: fat rejected, thin supported

Closes [[lambda-pointer-as-generic-type-arg-bypasses-guard]] (P1/crash). The asymmetric outcome
the issue file called for is the one that landed: a FAT `Lambda<T>*` generic type argument is
rejected with the declarator guard's own wording, and a THIN `function<T>*` generic type argument
compiles, links and calls through correctly.

### The filed repro had DRIFTED - re-measured before any fix

The issue file (filed against `8c29ca7`) recorded both spellings dying in an unlocated LLVM
module-verifier dump. On master `4c06cce` neither does. Measured on a Release binary built from
`4c06cce` in a detached worktree:

| Spelling | Filed (`8c29ca7`) | Measured on `4c06cce` |
|---|---|---|
| `Box<Lambda<int(int)>*> b; b.item = &f;` | `Error: module verification failed.`, unlocated, exit 1 | `(6,4): cannot store a pointer value into struct storage - a fixed array is not assignable from a pointer ...`, LOCATED, exit 1 |
| `Box<function<int(int)>*> b; b.item = &g;` | same verifier dump | same fixed-array message, LOCATED, exit 1 |

So the recorded severity ("no source diagnostic") was stale. What survived was the real defect
underneath: a MISLEADING diagnostic naming a fixed array where the source contains none. The
severity category still justified P1 - see the crash cell found in Phase A below, which really did
die with no diagnostic.

### Root cause

`MainListener::ResolveTypeArgEntry` is the generic type-argument funnel. Its
`functionPointerSpecifier` branch encoded the closure and `return`ed immediately, on the stated
reasoning that "a closure arg is a value", so the entry's `pointer()` was never consulted and the
`*` was silently DROPPED. `Box<Lambda<int(int)>*>` and `Box<Lambda<int(int)>>` resolved to the
same instantiation.

That explains the misleading store diagnostic too. With the pointer dropped, the field's type is
the encoded closure's backing VALUE type - a struct (`__closure_fat_ptr` for fat, a `{ i8* }`
wrapper for thin, `LLVMBackend.h:5049`). `b.item = &f` is then a pointer going into struct
storage, and the store gate reports the fixed-array case of that message. Nothing in the program
is a fixed array; the diagnostic was true of the LOWERED type and false of the source.

The `ForwardRefScanner` twin (`ResolveForwardTypeArg`) already appended `"*"` after its closure
branch, so the two passes disagreed on the instantiation name (`Box____thinfn_..._ptr` vs
`Box____thinfn_...`). The fix makes them agree.

### What changed (`cflat/MainListener.h`, 3 sites, all in `ResolveTypeArgEntry`)

1. **`functionPointerSpecifier` branch** - the direct `Lambda<...>*` / `function<...>*` spelling.
   Encode as before, then reject the fat case and append `"*"` for the thin one.
2. **`functionTypeAliases` branch** - the `using FA = Lambda<int(int)>; Box<FA*>` spelling. The
   arm was gated `if (!hasPointer && !hasArrayView)`, so a pointer fell past it into ordinary name
   resolution and produced `unknown type 'FA'`. Re-gated to `if (!hasArrayView)` with the same
   two outcomes. The message names the ALIAS (`'FA'`), matching the declarator guard's existing
   alias leg.
3. **The pointer-suffix funnel** - `RejectFatEncodedClosurePointerArg`, next to the existing
   interface-pointer rejection. A type parameter already BOUND to a closure arrives as an encoded
   name, so neither branch above can see it.

Two helpers were added: `RejectFatClosurePointerArg` (the shared message) and
`ClosureArgSpelling`, which rebuilds `Lambda<int(int)>` from the registered signature so the
funnel's diagnostic names the closure rather than `__fatfn_1_3_i32_3_i32`.

**`ClosureArgSpelling` is all-or-nothing about writability.** A signature component that is itself
a generic instantiation arrives mangled, so each one goes through the existing
`DisplayNameOfMangledType` / `MangledGenericNameIsAmbiguous` pair rather than a second demangler.
If ANY component comes back not-provably-writable, the whole spelling falls back to the raw
encoded name. Measured, on a `Box<T*>` over `Outer<Lambda<int(list<int>*)>>`:

| Signature component | Before this rule | After |
|---|---|---|
| `list<int>*` (unambiguous) | `Lambda<int(list__i32*)>` | `Lambda<int(list<i32>*)>` |
| `list<list<int>>*` (ambiguous - nested) | `Lambda<int(list__list__i32*)>` | `__fatfn_1_3_i32_18_list__list__i32ptr` |

The first is real source: `XBoxW<Lambda<int(list<i32>*)>> b = default;` compiles and runs. The
second is deliberately ugly - a nested instantiation cannot be rendered from the flat mangled
string (`Box__Box__i32` would come out `Box<Box, i32>`, which names no type), so the raw name is
the honest answer and a half-demangled hybrid would be worse than either. This is the one place
the diagnostic quotes a mangled symbol, and it is bounded to closures whose signature contains a
NESTED generic.

**Guard polarity, per site.** Nothing is rejected without proof of FAT. Sites 1 and 2 are inside
branches that only a closure reaches, where "not thin" IS fat (a `functionPointerSpecifier` is
either `function` or `Lambda`). Site 3 is the one that sees every type argument in the language,
and it fires only when the base name is a REGISTERED fat encoded closure (or literally
`__closure_fat_ptr`); every other pointer type argument - `int*`, `Circle*`, interfaces, nested
generics, thin closures - returns before the message is built.

**No `--init` round-trip change.** No field was added to `TypeAndValue` / `StructData` /
`AnnotationValue`. The mangled name DOES change for `Box<function<T>*>`, but only for a spelling
that did not compile before, and no file under `cflat/core/` uses a closure pointer as a generic
type argument (grepped), so no cached core symbol moves.

**`ParseDeclarationSpecifiers` was NOT touched**, so the both-copies rule does not apply to this
diff. The declarator guard it already carries is unchanged and still fires for direct declarators
(`Lambda<int(int)>* p = &f;` -> same message, both binaries).

### Phase A coverage matrix - every cell measured on BOTH binaries

PRE is a Release build of `4c06cce`; POST is the branch. `12` is `doubleIt(6)`, `6` is the fat
closure `f(5)`, `36` is `square(6)`.

**Fat `Lambda<T>*` - REJECT (all now the guard message, located):**

| Cell | PRE | POST |
|---|---|---|
| `Box<Lambda<T>*>` declare only | compiles + RUNS (pointer silently dropped) | guard message |
| `Box<Lambda<T>*>` store `&f` | fixed-array message | guard message |
| `Box<Lambda<T>*>` load / call-through | fixed-array message | guard message |
| `Box<Box<Lambda<T>*>>` nested | fixed-array message | guard message |
| `list<Lambda<T>*>` | fixed-array message | guard message |
| `using FA = Lambda<T>; Box<FA*>` | `unknown type 'FA'` | guard message naming `'FA'` |
| `Outer<Lambda<T>>` over `Box<T*>`, declare only | compiles + RUNS | guard message |
| `Outer<Lambda<T>>` over `Box<T*>`, store + call | **compiler SIGSEGV, exit 139, no diagnostic** | guard message |

The substitution cell is the one that justified the P1 severity on its own merits, and it was
never in the issue file. It is pre-existing: measured 139 on the `4c06cce` binary, not introduced
by this branch.

**Thin `function<T>*` - SUPPORT (all now compile and run):**

| Cell | PRE | POST |
|---|---|---|
| `Box<function<T>*>` store `&g` | fixed-array message | compiles |
| `Box<function<T>*>` load into `function<T>*` | fixed-array message | `12` |
| `Box<function<T>*>` call `(*b.item)(6)` | fixed-array message | `12` |
| generic METHOD `int use(int v) { return (*item)(v); }` | fixed-array message | `12` |
| `Box<Box<function<T>*>>` nested | fixed-array message | `12` |
| `list<function<T>*>` + `get(0)` | `cannot assign a struct value to a pointer variable` | `12` |
| `using TA = function<T>; Box<TA*>` | `unknown type 'TA'` | `12` |
| `N.Box<function<T>*>` namespaced | fixed-array message | `12` |
| `Box<function<T>**>` double pointer | fixed-array message | `12` |
| `Outer<function<T>>` over `Box<T*>` | `12` (already worked) | `12` |

The thin lowering was verified from `--no-opt` IR, not from the value alone: the field is a real
`ptr` slot (`store ptr %g, ptr %1`) and the call loads through it
(`%7 = load ptr, ptr %5` / `call i32 %7(i32 6)`). It is a genuine store-through, not a fold.

**Accept-set, frozen BEFORE the guard was written - unchanged PRE and POST:**

| Cell | PRE | POST |
|---|---|---|
| `Box<Lambda<T>> b; b.item = f;` (fat by value) | `6` | `6` |
| `Lambda<int(int)>[2] arr;` (fixed array) | `6` | `6` |
| `function<int(int)>[2] arr;` | `12` | `12` |
| `function<int(int)>* p = &g;` (direct declarator) | `12` | `12` |
| `list<function<int(int)>>` by value | `12` | `12` |
| plain struct field `function<T>` / `function<T>*` / `Lambda<T>` literal | `12` / `12` / `6` | same |
| `idf<int*>(&n)` (non-closure generic ptr arg) | `7` | `7` |
| `Lambda<int(int)>* p` declarator | guard message | guard message |
| `Box<Lambda<T>[]>` / `Box<function<T>[]>` declare only | compiles | compiles |

### Differential corpus sweep

Compile AND run every `.cb` under `Test/` and `example/` (447 files) with both binaries. The PRE
side was run TWICE first to identify nondeterminism before comparing PRE to POST: 8 files differ
PRE-vs-PRE (timings, addresses, thread-scheduling counts) and were excluded. Comparing PRE to
POST over the remaining 439 gives **exactly 2 differences, both the intended new test legs** -
`Test/errors/err_lambda_array_view.cb` (its three new legs now pass) and
`Test/test_function_ptr.cb` (`unknown type 'TgpAlias'` -> `71/71 passed`). Those two are also
what proves the comparison is not vacuous. Harness and verbatim outputs are in the branch's
`scratch/` (`sweep.sh`, `sweep_pre*.txt`, `sweep_post.txt`); the diff normalizes the worktree
path and the harness's own temp-exe name out, which otherwise show up as differences in the
"imported file not found (searched ...)" rows for the 29 Windows-only files.

A note on the harness, because the first version was wrong: exit status was being read from a
`grep` at the end of a pipe rather than from the compiler, which made every row's status
meaningless. The corrected version redirects to a file and reads `$?` directly. This is the
mistake `fix-issue-lessons.md` already records under the corpus sweep; it happened again.

Weighting, per the standing rule: this change ADDS a rejection, so the sweep is the weaker half
of the evidence - no corpus file writes a closure pointer as a generic type argument, so the
sweep structurally cannot see the new rule at all. The strong half is the targeted accept-set
above, which deliberately crosses every fat/thin, direct/alias/substitution boundary the new rule
could confuse.

### Coverage landed

- `Test/errors/err_lambda_array_view.cb` gains THREE legs, one per resolver arm: direct spelling,
  `using` alias, and substitution. Each was mutation-tested individually against the `4c06cce`
  binary and fails there for a DIFFERENT stated reason - `unknown type
  'LavBoxDirect____fatfn_1_3_i32_3_i32'`, `unknown type 'LavAlias'`, and "did not occur"
  (the SIGSEGV cell) respectively - which is what proves the three legs reach three different
  arms rather than one shared one.
- `Test/test_function_ptr.cb` gains `testThinFuncPtrGenericArg()` - 11 value legs (`tgp_*`)
  across the direct, method, reseat, load, nested, substitution, alias, namespace, double-pointer
  and container spellings. The reseat leg (`&g` -> `&g2`, `12` -> `36`) is the one that proves the
  field is a pointer SLOT and not a copied value. `tgp_subst` already passed on the pre-fix binary
  and is labelled in the file as a must-still-work leg, not a discriminator - it is there because
  that funnel is where the fat rejection was added.

`test.sh Release`: 600 passed, 0 failed, 8 skipped. `example_mac.sh Release`: 35 passed, 0 failed.
`test_lsp.sh Release`: 152 passed, 0 failed.

### Diagnosed and deliberately NOT fixed here

Four files filed in this commit. The first three are measured identical on both binaries; the
fourth is a wording defect the branch EXPOSES on one of two arms without changing the wording:

- [[closure-by-value-into-generic-struct-field]] (P2). `Box<function<T>> b; b.item = g;` is
  rejected with the same misleading fixed-array message - a BY-VALUE root (the thin encoded
  closure's `{ i8* }` backing type vs a bare `ptr` value), not a pointer-depth one. Also carries
  the lambda-LITERAL-into-a-generic-field cell, which is an unlocated verifier dump. This is why
  the accept-set could freeze "by value must keep working" for `Lambda<T>` but not `function<T>`.
- [[closure-type-argument-to-a-generic-function]] (P2). A closure type argument to a generic
  FUNCTION does not resolve, explicitly (`unknown type 'function<int(int)>'`) or by inference
  (mangles to `idf__i8`). The generic-STRUCT path funnels through `ResolveTypeArgEntry`; this one
  appears not to, so the asymmetry that landed here does not reach it.
- [[closure-arg-suffixes-unvalidated-in-signature-position]] (P3). The `[]` suffix on a closure
  type argument is unvalidated (the structural twin of this bug, one suffix over), and a fat
  closure pointer inside a closure SIGNATURE is accepted by `ResolveSigComponentCodegen` - a
  third site. Neither is reachable to a store or a call today, so neither was rejected: the
  standing rule is that a site added to a reject must be shown broken from the IR first.
- [[unique-on-closure-arg-message-denies-the-pointer]] (P3). `RBox<unique TA*>` is rejected with
  `unique requires a pointer or interface type`, which is false - `TA*` is a pointer. The
  rejection is CORRECT (a closure owns no allocation) and the declarator path already words it
  right. The direct arm measures identical on both binaries; the ALIAS arm's message is newly
  reachable here because this commit re-gated that arm, with the wording itself untouched. Left
  alone deliberately: editing a rejection's text is a different concern from carrying pointer
  depth, and both arms should change together.
---

## Landed: `fix/chain-coalesce` (2026-08-05) - chained and nested joins boxed into an interface

Closes [[chained-nullcoalesce-not-boxed-into-interface]]. The filed issue was CHAINED `??`
(`a ?? b ?? c`) into an interface in call-argument, decl-init and return position. The measured
area is larger and the root cause is one predicate, not a `??` problem.

**Root cause.** A join arm whose value is ITSELF a join has no concrete class.
`ResolvePointerElementTypeName` answers an arm from the declared type of the binding a load reads;
the result of an inner join is a load off the coalesce SLOT (for `??`) or a bare PHI (for `?:`),
neither of which names a class. `BoxInterfaceJoinArms` then reported an unresolvable arm and
`BoxNullCoalesceJoinArgument` bailed. Both bails were correct - no IR emitted, no wrong overload
selected - nothing recursed.

**The issue file's fix direction was measured WRONG and not followed.** It said to flatten the
chain at the ledger, splicing an inner join's arms into the outer entry as `{a,b,c}` while
"keeping each spliced arm's OWN predecessor block for the fat phi". The IR says that phi is
invalid. For `Circle* p = z ?? y ?? a;` the `--no-opt` CFG is:

```
nullcoal_resume:   ; preds = %nullcoal_resume3, %nullcoal_notnull
nullcoal_resume3:  ; preds = %nullcoal_null1, %nullcoal_notnull2
```

The inner arms live in `nullcoal_null1` / `nullcoal_notnull2`, which branch to the INNER resume
block. They are not predecessors of the outer join point, so a flat phi over them has incoming
edges from blocks that do not branch to it. Giving the spliced arms the outer arm's block instead
is equally invalid - both inner arms would share one incoming block with different values.

**What landed instead: recursion, not flattening.** The nested join is boxed at ITS OWN join
point, so its fat phi lands in the block that really does branch to the outer join, and the outer
phi takes that block. Three pieces in `cflat/MainListener.h`:

- `CollectPointerJoinArms` - the arms of a join of EITHER spelling in one call (`?:` from the
  PHI's incoming edges, `??` from `nullCoalesceJoins_`).
- `NestedJoinArmsBoxable` - an IR-EMITTING-FREE recursive validation, so the existing "resolve
  every arm before rewriting any" invariant survives: a later arm's failure cannot leave
  half-boxed IR behind.

The `kMaxNestedJoinDepth = 8` cap is ASYMMETRIC between the two paths and the two effective limits
were measured, not derived. The argument path descends a whole chain inside ONE capped call, so the
cap bounds the chain; the boxing path re-enters `NestedJoinArmsBoxable` at depth 0 once per level,
so there the cap bounds only the per-level LOOK-AHEAD window and the chain length is not directly
limited by it. Measured on the fix binary: argument position accepts a 10-arm chain and rejects an
11-arm one; decl-init accepts 11 and rejects 12. Both are clean located rejections, no crash and no
malformed IR, so the asymmetry is a cosmetic limit difference rather than a soundness issue and was
deliberately NOT redesigned here.
- `BoxInterfaceJoinArms` - an unresolvable arm that validates as a nested join is boxed by a
  recursive `UpcastPointerJoinToInterface` instead of a vtable lookup it has no class for.

The recursive box also THREADS the inner verdict back out (`armFailure` / `armNotOwned`). Without
that, a `move`-interface return of a NESTED join whose inner arm is provably non-owning reported
"bind the arm to a local variable of the class type first" - a remedy that does not work at a
`move` return - where the length-1 spelling correctly reported the ownership diagnostic. Measured
with one `unique` arm and one borrowed parameter arm, so the whole-expression check passes and the
per-arm check is really reached: before the threading the nested form gave the wrong remedy and the
length-1 form gave "returned expression is not owned"; after, both give the ownership message. It
is a rejection on both sides either way - only the wording changed - and the underlying gap that
makes these shapes reject at all is
[[move-interface-return-of-nullcoalesce-join-not-owned]].

`BoxNullCoalesceJoinArgument` was renamed `BoxPointerJoinArgument` and now collects LEAF arm
classes recursively (`CollectJoinArmClasses`) and accepts either spelling. Its bail rules are
untouched: still no boxing past ANY pointer parameter at that position, still a bail on two
candidates offering different interfaces.

**Why recursion and not flattening also matters downstream.** Four other consumers read the same
ledger and ALREADY recurse through nested joins on their own terms - `JoinCarriesCodeValue`,
`JoinDeliversDataValue`, `JoinCarriesOwningTempUniqueField` (arm 0 only) in `LLVMBackend.h`, and
`JoinArmsKeepOwner` in `MainListener.h`. Flattening the ledger would have silently changed all
four, `JoinArmsKeepOwner` most sharply: a chained join's arms would newly PROVE another owner and
suppress a receiver's free. Recursion leaves every one of them seeing exactly what it saw before.

**Per-site audit of the same predicate.** `ResolvePointerElementTypeName` has FIVE call sites
(a repo-wide grep returns seven hits: its definition in `LLVMBackend.h` and one prose mention in a
`BoxInterfaceJoinArms` comment are not calls). Two of the five are the new helpers
`NestedJoinArmsBoxable` and `CollectJoinArmClasses`. `BoxInterfaceJoinArms` is fixed. The remaining
two, plus the `as`/`is` classifier that never reaches this helper at all for a `??`, carry the same
defect, were
measured IDENTICAL on `4c06cce` and the fix binary, and are filed rather than fixed, because none
is a drop-in recursion and each rejects today (so the hazard of a hasty widening is a silent wrong
arm): the two remaining call sites `ResolveTernaryArmClasses` and
`BoxTernaryThinArmToInterface` (both in `MainListener.h`) in
[[nested-join-arm-unresolved-in-is-as-and-mixed-ternary]], and the `as`/`is` classifier that never
recognizes a `??` at all in [[as-is-does-not-recognize-nullcoalesce-join]]. Two further neighbours
found by the matrix, both reproducing at chain length 1 and so not chaining defects:
[[join-arm-from-call-result-not-boxed-into-interface]] and
[[move-interface-return-of-nullcoalesce-join-not-owned]].

**Measured behaviour change.** Every cell below is a compile+run pair on the two Release binaries
(`4c06cce` pre, this commit post). `4c06cce` was the merge-base when the pairs were taken; this
commit was later rebased onto `8c5a860` (`fix/lamptr-generic`), and the whole 43-cell matrix, the
25 leg probes and the ownership-diagnostic probes were re-run on the rebased binary and came back
byte-identical - so the pairs above are not stale and the two lanes do not interact. Exit codes of the accept-set cells are the probes' own encoding
of a value, not compiler exit codes.

| Cell | Pre | Post |
|------|-----|------|
| `take(z ?? y ?? a)` (arg, len 3) | `no overload of 'take' matches` | 9 |
| `take(z ?? y ?? x ?? a)` (arg, len 4) | `no overload of 'take' matches` | 9 |
| `IShape j = z ?? y ?? a` (decl-init) | `cannot convert '??' arm ... cannot be determined` | 9 |
| `IShape j = z ?? y ?? x ?? a` (decl, len 4) | same | 9 |
| `return p ?? q ?? r` (return) | same | 9 |
| `j = z ?? y ?? a` (assign to iface local) | same | 9 |
| `h.s = z ?? y ?? a` (assign to iface field) | same | 9 |
| `take(z ?? (y ?? a))` / `take((z ?? y) ?? a)` | `no overload` | 9 / 9 |
| mixed classes in a chain (`z ?? b ?? a`) | `no overload` / `cannot convert` | 8 |
| `nullptr` literal arm in a chain | `no overload` | 9 |
| all-null chain into an interface | `cannot convert '??' arm` | 4 |
| chain into a VIRTUAL method's iface param | `no overload of 'put' matches` | 9 |
| `take(z ?? (k>0 ? a : b))` (`?:` in `??`) | `no overload` | 9 |
| `take(k>0 ? (z ?? a) : b)` (`??` in `?:`) | `no overload` | 9 |
| `take(k>0 ? (m>0?a:b) : b)` (pure `?:` chain) | `no overload` | 9 |
| `IShape j = k>0 ? (m>0?a:b) : b` | `cannot convert '?:' arm` | 9 |
| `return k>0 ? (m>0?p:q) : q` | `cannot convert '?:' arm` | 9 |
| `take(k>0 ? a : b)` (SINGLE `?:`, arg) | `no overload of 'take' matches` | 9 |
| `(ib(t) ? (ib(f) ? a : b) : a) as IShape` | `cannot convert '?:' arm` | 25 |

Accept-set, frozen BEFORE the change and measured identical on both binaries: single `??` in arg
/ decl / return (9, 9, 9); single all-null `??` into an interface (4); the documented `T*`-local
workaround (9); chains NOT involving an interface - `Circle*`, `int*`, `void*` parameter, chained
`??` return of `Circle*` (9, 9, 7, 9); a `void*` overload still beating the interface candidate at
single AND chained length (7, 7); two candidate interfaces of which only one is implemented by
every arm (`g(IShape)` / `g(IOther)` with `Circle` arms) at SINGLE `??` length, which selects
`g(IShape)` and prints 1 on BOTH binaries; and a chain arm whose class implements no candidate
interface still rejected.

**No accept-set cell changed.** An earlier draft of this record claimed one did - that the two
candidate-interface cell went from `no overload of 'g' matches` to selecting correctly. That claim
was FALSE and is retracted. The probe behind it was chain-length THREE, so its pre-fix rejection
was the ordinary chain rejection and it belongs with the reject-to-accept cells above, not with
the accept set. Re-measured in isolation, the SINGLE-`??` spelling of the same program already
printed 1 on `4c06cce`, and the diff does not touch the target-selection loop. This is the
sibling-inference error `internal/fix-issue-lessons.md` warns about under "On claims of
equivalence between two binaries": an accept-set claim was read off a cell one axis away from the
one the claim was about.

**Direction of the change.** Every cell moves from REJECT to ACCEPT; the diff adds no rejection.
That is why the differential sweep is the favourable-polarity evidence here rather than the weak
half - there is no new guard for a corpus to fail to cross.

**Verification.** `test.sh Release`: 600 passed, 0 failed, 8 skipped. `example_mac.sh Release`:
35 passed, 0 failed. `test_lsp.sh Release`: 152 passed, 0 failed. Differential corpus sweep over
every `.cb` in `Test/` and `example/` at the MASTER sources (so the test-file diff cannot pollute
it), compiling AND running with both binaries - 447 files: **0 behavioural differences** across the
441 deterministic files.

The COMPOSITION of that sweep is weaker than the file count suggests, and is stated here so nobody
reads more into it than it carries. 157 rows are compile-rejections (signature = the normalized
diagnostic text). The other 290 rows had a successful compile and were then run, but they break
down as: 208 `Test/errors/` fixtures (79 exit 0, 76 exit 133 SIGTRAP, and **53 exit 127 - no binary
was ever produced, so those 53 contribute only "compile succeeded" and nothing about runtime**),
and just 82 ordinary programs (60 exit 0, 14 exit 1, 6 example timeouts at the 60s cap, 1 abort,
1 exit 49). So roughly SIXTY non-error programs actually ran to a clean completion. The
zero-difference conclusion stands for what it covers; it is not evidence about the 53 that
produced no binary. The 6 excluded
(`Test/test_hpc.cb`, `example/hpc/{lu_bench,mc_pi,nbody,poisson_cg}.cb`,
`example/macos/sysinfo_mac.cb`) print timings and were proven nondeterministic by running the PRE
binary TWICE before diffing, not assumed. The sweep harness was shown non-vacuous by dropping a
known-differing file into the swept tree and confirming it was reported as a difference.

**Coverage.** `Test/test_move.cb` gained 25 value legs in `testUniqueInterfaceTernary` -
`iface_ncchain_*`, `iface_tern_in_nc_*`, `iface_nc_in_tern_*`, `iface_ternchain_*`,
`iface_tern_single_arg` - across chain length (2/3/4), position (decl-init, argument, return,
assignment), arm kind (nullable local, null literal, mixed concrete classes), explicit
parenthesization, and both spellings nested in each other. Every leg asserts a VALUE and each
selects a DIFFERENT arm with a different `area()`, so no leg can pass off another arm's box. All
25 were mutation-tested individually against the `4c06cce` binary and each fails there for the
reason its position implies - `no overload of 'takeJoinIface'` in argument position, `cannot
convert '??' arm` / `cannot convert '?:' arm` elsewhere. The first cut of that mutation test was
INVALID and was redone: every probe carried the `pickJoinNC3` helper in its header, so all 25
"failures" were the header failing to compile, not the leg. Per-leg headers made the pre-fix
diagnostics vary by position, which is the evidence that each probe reaches the arm it names.

## Landed: `fix/genfn-lowering` (2026-08-05) - a generic instantiated over a closure type lowers its element like the spelling it encodes

Consolidated fix for the last two `p1/crash/` closure rows -
`generic-wrapper-over-function-type-llvm-fatal` and
`list-of-function-element-into-closure-param-fails-verifier` - plus the P2
`closure-by-value-into-generic-struct-field` that shared their root. All three files are DELETED
by this commit. The P2 deletion was re-justified after review round 1 rather than carried over,
by compiling and running the file's OWN two repro programs verbatim rather than a sibling probe:
sub-case 1 (`function<int(int)> g = dbl; b.item = g;` then `b.item(6)`) prints `byval=12`,
sub-case 2 (`b.item = (int x) => x + 1;` then `b.item(5)`) prints `byval=6`, and the capturing
variant of sub-case 2 - the cell review round 1 found miscompiled - prints `byval=11`. Every cell
the file recorded is fixed and measured so, which is the condition for deleting rather than
trimming it. (The first of those numbers was quoted wrong in the round-1 draft: it read `R=6`,
which is a NEIGHBOURING probe's output - one that stores `triple` instead of `dbl`. Sibling
inference, caught in review round 2; the repros are now run as written.)

### The filed repros had DRIFTED, and re-measuring found worse

Every cell below was re-measured on a Release binary built from master `8c5a860` before any code
was written. Two of the three filed headline symptoms were already gone, and two spellings nobody
had filed were compiler SIGSEGVs:

| Filed as | Measured on `8c5a860` |
|---|---|
| `Box<function<int(int)>>` invoke -> `LLVM ERROR: Cannot select: AArch64ISD::CALL`, exit 134 | The ISel fatal is GONE. The STORE `b.value = triple` now rejects first with a located but false message: `cannot store a pointer value into struct storage - a fixed array is not assignable from a pointer...` (exit 1). The invoke is never reached. |
| `list<function<>>` element into a closure param -> unlocated verifier dump | Reproduces exactly as filed (`Call parameter type does not match function signature!`, exit 1, no `file(line,col):`). |
| `list<Lambda<>>.add(void*)` -> unlocated verifier dump (annotated 2026-08-05) | Reproduces. |
| not filed | `int callit(function<int(int)> f); callit(ls.get(0))` - **compiler SIGSEGV, exit 139, zero output**. |
| not filed | `function<int(int)> unwrap(Box<function<int(int)>> b) { return b.value; }` - **compiler SIGSEGV, exit 139, zero output**. |
| not filed | `list<Lambda<int(int)>>.add(namedFn)` - unlocated verifier dump. |
| not filed | A lambda LITERAL brace-initialized into a closure field - unlocated verifier dump, and **not generic-specific**: plain `struct S { function<int(int)> f; }; S s = { f = (int x) => x + 1 };` fails identically. |

The lesson that "a stale crash SIGNATURE does not mean a healthy area" held again: the file named
after the ISel fatal pointed at a live area whose worst symptom was a different, unfiled crash.

### Root cause - one representation decision, leaking at every boundary

`RegisterEncodedClosureType` (`LLVMBackend.h`) gave a THIN encoded closure - the element type a
generic instantiation over `function<T>` gets, e.g. `__thinfn_1_3_i32_3_i32` - a `{ i8* }` STRUCT
backing "so it stores/copies like a normal value-type element". A `function<T>` VALUE is a bare
machine `ptr`. Those two facts cannot both hold, so every place the encoded element met ordinary
code, the struct wrapper leaked:

- a FIELD store saw a pointer going into struct storage -> the fixed-array message, which named a
  construct the program did not contain;
- a field LOAD into a `function<T>` local saw a struct going into a pointer -> `cannot assign a
  struct value to a pointer variable`;
- an element passed ON to a `Lambda<>` or `function<>` parameter reached the callee as the wrapper
  struct -> the module verifier, or (for the thin destination) a compiler SIGSEGV;
- a lambda LITERAL assigned to a substituted field inferred its return type from a target that
  reported no signature -> a `void`-returning body with an `i32` return -> the verifier.

Only two boundaries had been patched around it (a wrap in `LowerByValueArg`, an unwrap at the
invoke site), which is exactly why `list<function<>>` could be BUILT and its element INVOKED while
every other use died.

The fix deletes the representation, rather than adding a third and fourth patch:

1. **`RegisterEncodedClosureType`** no longer creates a `dataStructures` entry for a thin encoded
   closure. **`GetType`** resolves the encoded name straight to `BuildThinFnPtrType` - the same
   type the spelled `function<T>` gets. Pointer and array wrapping run unchanged after it, so
   `Box<function<T>*>` (the `fix/lamptr-generic` shape) keeps working and now lowers to `ptr*`
   rather than `{ i8* }*`.
2. **`LowerByValueArg`** converts an encoded closure argument exactly as an `IsFunctionPointer`
   parameter does - fat->thin narrowing, thin->fat widening - and routes BOTH through the existing
   provenance gates (`CheckThinFnPtrArgProvenance`, `WidenToClosureFatChecked`). The old wrap
   bypassed them, which is why `list<function<>>.add(vp)` on a `void*` COMPILED on `8c5a860`.
3. **`ParseAssignment`** applies the same two conversions when the destination is an encoded
   closure field, and threads the encoded signature into `lambdaExpectedType` so a lambda literal
   on the RHS infers its return type.
4. **`EmitFieldInitializer` / `EmitOneFieldInit`** do the same for the BRACE-INIT spelling, which
   had NEITHER the signature threading nor any thin/fat conversion. This is the one part of the
   change that also fixes a non-generic program.
5. Two small routing follow-ons in the postfix walk: a thin encoded receiver reaches the UFCS
   member-name arm and the pointer-identity `.copy()` arm. Without them `list<T>::copy()`'s
   `_data[i].copy()` stopped resolving the moment the element lost its struct backing - the
   regression this fix introduced and closed mid-flight, caught by the probe corpus, not by
   reasoning.
6. **The `=` path's closure OWNERSHIP-transfer arm is re-keyed on the REPRESENTATION**, not the
   spelling. It tested `TypeName == "__closure_fat_ptr"`; a generic-substituted FAT field carries
   the encoded name (`__fatfn_...`) but is bit-for-bit the same `{ code, env }` struct and owns the
   same env, so it now takes the same arm (via the new `IsFatEncodedClosureType`, mirroring what
   `EmitOneFieldInit` already did by testing `val->getType() == GetClosureFatPtrType()`). See the
   review-caught regression below - this is the fix for it, and it is why the rule is stated as
   "same representation, same ownership rule" rather than as a fourth special case.

### Guard polarity - what is REJECTED, and what the accept set proves

Two spellings that PRE accepted are now rejected, both by pre-existing provenance gates the old
wrap had bypassed, both located:

- `list<function<int(int)>>.add(vp)` on a `void*` - PRE compiled it and would have called a data
  address as code. Now: `cannot pass a non-function pointer value to 'function<>' parameter
  'value'`.
- `Box<Lambda<int(int)>>.item = vp` - PRE rejected it, but only by accident (struct storage), with
  the fixed-array message. Now: the closure-parameter provenance message, which is true of the site.

The must-still-work accept set was frozen BEFORE those gates were wired in, as a probe corpus that
grew to 59 programs.
The ones that cross the boundary the new gates could mistake for a violation - a named function, a
`function<>` value, a non-capturing lambda literal, a thin `function<T>*`, an aliased spelling
(`using IntFn = function<int(int)>`), a `Lambda<>` local, and the brace-init form of each - all
compile and produce their expected values. Three FAT-into-thin rejections (`Box<Lambda<>>` and
`list<Lambda<>>` elements into a C `function<>` parameter) are unchanged in both verdict and
wording from PRE.

### The regression review round 1 caught, and the matrix hole that hid it

The first version of this branch MISCOMPILED a capturing lambda LITERAL assigned with `=` to a FAT
generic-substituted field: `Box<Lambda<int(int)>> b; int cap = 10; b.item = (int x) => { return x
+ cap; };` then `b.item(1)` printed **1** instead of 11 and then SIGSEGVed. Same on a generic
`class`, on a nested `Outer<T>{Box<T>}`, on a heap receiver `bp->item`, and on a reassign; the
spelled non-generic control printed 11 on both binaries. On `8c5a860` that program did not compile
at all (the unlocated verifier dump), so the branch turned a compile-time rejection into a wrong
answer plus a crash - strictly worse.

Cause: the `=` path's ownership-transfer arm keyed on `TypeName == "__closure_fat_ptr"`, so the
encoded fat field skipped it; `FlushOwnedClosureTemps` then freed the env at end-of-full-expression
while the field still pointed at it (visible in `--out-lli` as an extra `__closure_fat_ptr.dtor`
right after the store, with the later `Box` dtor as the double free). The `lambdaExpectedType`
threading is what made the program compile in the first place, which is exactly what exposed the
pre-existing spelling-keyed predicate: **unblocking compilation moves a program onto ownership
paths that a compile-time rejection had been hiding.** The brace-init path never had the bug,
because it keys on the VALUE's type (`val->getType() == GetClosureFatPtrType()`) rather than on the
destination's spelling.

Why the matrix missed it: the operation axis listed "lambda literal" and "capturing literal", and
the element axis listed thin and fat, but the fat x capturing-literal CROSS had no cell - fat
literals were probed only non-capturing, and capturing literals only against thin destinations
(where they are rejected outright, so no ownership path runs). A cross that both axes cover
individually is exactly the cell an axis-at-a-time matrix drops. The nine cells below close it.

### Coverage matrix (70 probe programs, every cell measured on both binaries)

Shapes: `Box<T>` generic struct, generic `class`, nested `Box<Box<T>>`, `list<T>`, alias spelling,
heap-pointer receiver. Element types: `function<int(int)>` (thin), `Lambda<int(int)>` (fat),
`function<int(int)>*`. Operations: field store (named fn / `function<>` value / `Lambda<>` value /
non-capturing literal / **capturing literal** / `void*`), brace-init, reassign, field load into a
local, invoke the element, pass it to a `Lambda<>` param, pass it to a `function<>` param, return
it, pass the whole instantiation, `list` add/get, and the non-generic controls for each.

Net over 70 cells, every one measured on both binaries (re-measured on the rebased binary):

| Transition | Cells |
|---|---|
| hard failure -> compiles and produces the expected value | **31** |
| hard failure -> COMPILES, matching its non-generic control (see the loosening note) | 1 |
| rejected -> rejected with a DIFFERENT and truer message | 4 |
| accepted -> rejected (the `void*` tightenings) | 3 |
| unchanged, compiles with the identical output | 24 |
| unchanged, rejected with the identical message | 7 |

Two of the three tightenings were only PROBED in review round 2 - they are not new behaviour from
it. `Box<Lambda<int(int)>> b = { item = vp };` on a `void*`, and its NON-generic twin
`struct Plain { Lambda<int(int)> item; }`, both compiled on `8c5a860` (a data address stored in a
code slot) and are refused now; the brace-init widen gate that refuses them landed in round 1,
before either cell existed. The third is `list<function<>>.add(vp)`.

The generic one is also where the field-store diagnostic quoted a MANGLED instantiation name -
`closure field 'Box____fatfn_1_3_i32_3_i32.item'`. It now renders `Box<Lambda<int(int)>>.item`:
`DisplayNameOfMangledType` learned that an argument segment beginning with `__` is an encoded
closure and spells it back from its recorded signature, and returns the raw mangled name with
`*writable = false` when it cannot - never a half-demangled hybrid like `Box<, fatfn_1_3_i32_3_i32>`.
The `=` path (`b.value`) and the non-generic path (`Plain.item`) are unchanged, measured.

No cell produces a different VALUE than it did before - but that claim was worth little as
originally written, because on this branch's first version the 59-cell matrix had no cell that
COULD have shown the miscompile. It is stated here only as an accompaniment to the nine cells that
now cover the cross, not as evidence on its own.

Of the 31 genuinely fixed, **2 were a compiler SIGSEGV (exit 139, zero output)**, **15 were an
UNLOCATED module-verifier dump** (the 7 new capturing-into-fat cells among them), and 14 were a
located but false "cannot store a pointer value into struct storage - a fixed array..." / "cannot
assign a struct value to a pointer variable" / "cannot cast an aggregate value" message. Of the 4
that changed message, 2 were an unlocated verifier dump and are now located.
**No LLVM fatal, verifier dump or compiler crash remains in the matrix.**

Ownership was re-checked on the nine new cells, not assumed: all nine run to `rc=0` with the
expected value, all nine report `0 leaks for 0 total leaked bytes` under `leaks --atExit`, the
reassign cell (the double-free-prone one) prints 21 and exits clean, and the `--out-lli` for the
store cell shows the old-slot dtor BEFORE the store and no dtor after it - the same shape the
spelled control emits. `Test/test_function_ptr.cb` as a whole is also leak-clean.

The one LOOSENING: `Box<function<int(int)>>.item = vp` on a `void*` was rejected on PRE and
compiles now. That PRE rejection was not a gate - it was the struct-storage message firing on the
`{ i8* }` backing - and the non-generic control `struct S { function<int(int)> f; }; s.f = vp;`
compiles on BOTH binaries. So the generic spelling now MATCHES its control rather than being
accidentally stricter, and the underlying hole (both spellings SIGBUS at the call, exit 138) is
filed as [[data-pointer-assigned-to-thin-function-value]] with the accept set the real gate needs.

Cells deliberately NOT closed, each filed:

- `(*ls.get(0))(2)` - inline deref of a call result - `Unable to dereference an object without a
  Storage.`, identical on both binaries and not closure-specific. Filed as
  [[inline-deref-of-container-call-result-has-no-storage]] (P3). The two-line form
  (`function<int(int)>* e0 = ls.get(0); (*e0)(2)`) works and is already covered in
  `Test/test_function_ptr.cb`.
- `s.f = vp` on a THIN `function<>` field OR a bare local - compiles clean and exits 138 (SIGBUS),
  identical on both binaries and NOT generic-specific. Filed as
  [[data-pointer-assigned-to-thin-function-value]] (P1/codegen). The generic thin field inherits
  exactly this and no more: closing it means gating the ASSIGNMENT leg for a thin destination,
  which is a separate accept set from the argument leg this commit reuses.
- `sizeof(Box<function<int(int)>>)` - compiler **SIGSEGV, exit 139, zero output**, identical on
  both binaries, and true of the `Lambda<>` and `list<T>` spellings too while
  `sizeof(function<int(int)>)` compiles and `sizeof(Box<int>)` gives a located `unknown type`.
  Found on the neighbour axis, not on any filed repro. Filed as
  [[sizeof-generic-over-closure-type-segfaults-compiler]] (P1/crash) - so this commit closes two
  rows of that bucket and opens one.

Neighbour axes probed and measured UNCHANGED on both binaries: a `list<function<>>` captured
BY REFERENCE in a lambda body (`R=8` both), `dictionary<string, function<int(int)>>` set/get
(`R=10` both), and `list<function<>>::copy()` (`R=10 1` both - the path whose `_data[i].copy()`
the postfix follow-ons had to keep resolving). All three are cells of the 59 above. One cell in that
set is a fix nobody asked for: a fixed-array field of the element type,
`struct Box<T> { T[3] items; }` with `b.items[0] = dbl`, went from the same false fixed-array
rejection to `R=10`.

### The `--init` cache is not involved, and that was checked rather than assumed

No field was added to `TypeAndValue` / `StructData` / `AnnotationValue`, so the round-trip rule
does not bite. The change does REMOVE a `dataStructures` entry (the thin encoded closure's `{ i8* }`
shell), and `dataStructures` IS cached - a cache written by an older binary could in principle
restore the shell. It cannot here: `encodedClosureTypes_` is never serialized, no `core/*.cb`
instantiates a generic over a thin closure type (grepped), and no cached artifact under
`.cflat/runtime/` contains a `__thinfn` name (grepped). Encoded closure types are rebuilt from user
source on every compile.

### Verification

- `./cmake_build.sh release`, `./test.sh Release`: **600 passed, 0 failed, 8 skipped**.
- `bash example_mac.sh Release`: **35 passed, 0 failed**.
- `./test_lsp.sh Release`: **152 passed, 0 failed**.
- All three re-run on the binary built AFTER the rebase onto `fix/chain-coalesce` (`47d3609`),
  not carried over from before it. Interaction with that landing checked directly rather than
  inferred: `take(z ?? y ?? a)` into an interface argument still returns 9, and
  `Test/test_move.cb` - the file that landing extended - passes 738/738.
- Differential corpus sweep, compile AND run, every `.cb` under `Test/` and `example/` - **447
  files**, PRE (`8c5a860`, built in a detached worktree) run TWICE first to identify nondeterminism,
  then POST. Harness and raw results in `scratch/gf_sweep.sh`, `scratch/sweep_PRE1/`,
  `scratch/sweep_PRE2/`, `scratch/sweep_POST/`.

  Honest composition of the 447 rows (PRE2): 290 compiled clean, 156 rejected, 1 crashed the
  compiler. Of the 156 rejections, 129 are `Test/errors/` fixtures where a rejection IS the pass
  condition and 22 are Windows-only sources whose headers do not exist on macOS - so only 5
  rejections are ordinary. Of the rows that produced a binary, 138 ran to `rc=0`, 15 exited nonzero
  by design, 6 hit the harness's 60 s cap (interactive/GUI examples), and 78 are error fixtures
  whose stub binary aborts. Because the sweep compiles with `-o`, the `Test/errors/` rows carry no
  signal beyond "the compiler still reaches the same place"; the real check for those is `test.sh`.

  PRE1 vs PRE2 differ on exactly **10** rows, all output-hash-only, all inherently nondeterministic
  (`test_c`, `test_hpc`, `test_time`, the four `example/hpc` benchmarks, `framework_link`,
  `sysinfo_mac`, `shell/pwd` - timings, addresses, uptime, cwd). That is the noise floor.

  PRE vs POST: **exactly one row changes status** - `Test/test_function_ptr.cb`, `C139` (compiler
  SIGSEGV, no output) -> `C0 R0`, i.e. the file carrying the new regression legs goes from crashing
  the PRE compiler to compiling and running green. The set of rows whose output hash differs is
  **identical to the 10-row noise floor**, so no program's behaviour changed. Every other textual
  diff is the PRE binary's own install path appearing inside `imported file not found` messages -
  a harness artifact of running PRE out of a separate directory, not a compiler difference.

  Re-run in full against the FINAL binary after review round 1 (`scratch/sweep_POST2/`): same
  single status change vs PRE, and vs the round-1 POST the only differing rows are the same 10
  noise-floor rows - the ownership re-keying and the message rewording disturbed nothing.

  Weighting: the sweep is the weaker half of the evidence here, because no corpus file other than
  the one I extended instantiates a generic over a closure type - the sweep structurally cannot see
  the new lowering. Its value is the negative result: the representation change did not disturb the
  446 files that do not use the shape. The strong half is the 70-cell matrix above.

### Regression tests

`Test/test_function_ptr.cb::testClosureByValueGenericArg` - 28 value-asserting legs. Every one was
measured failing on the PRE binary first, and each comment states which failure mode it pins
(hard error / unlocated verifier / SIGSEGV) so a later reader can tell a real leg from a vacuous
one. `cbv_thin_brace_init` is explicitly labelled a must-still-work leg: it passes on both
binaries and is there to protect the brace path, not to prove the fix. Seven of the 28 are the
`cbv_fat_capturing_*` legs added after review round 1 - store, reassign, pass-to-param, generic
class, nested, heap-pointer receiver, brace-init - each mutation-tested standalone and each
failing on PRE with the unlocated verifier dump. They are value-asserting on purpose: the
regression they pin compiled fine and returned the WRONG number, so a compile-only leg would have
been vacuous against it.

`Test/errors/err_data_pointer_to_closure_param.cb` - 3 new `expect_error` legs (fat container, fat
generic field, thin container). The field-store leg pins the reworded diagnostic `cannot store a
'void*' value into closure field 'gb.item'`: the gate is shared with the argument path, but a
field store is not a parameter pass and the message must not say it is (review round 1, LOW). `Test/errors/err_lambda_to_c_funcptr.cb` - 2 new legs (a capturing
lambda into a thin generic field, via `=` and via brace-init). All five were mutation-tested
individually against the PRE binary: each fails there, three with `expected error ... did not
occur` (PRE accepted the program or deferred the failure past the block) and two with a DIFFERENT
message (`cannot store a pointer value into struct storage`, `cannot cast an aggregate value`),
which is what proves the leg pins the new diagnostic rather than a pre-existing one.

### References left as written

`fix/widengate`'s landed record says the `list<Lambda<>>` container axis of its accept set "cannot
be exercised in either direction until that is fixed". It is fixed now, and the `add` direction is
exercised by `cbv_fat_list_named` (accept) and the new `expect_error` leg (reject). The record
itself is a dated snapshot and is left as written, per this file's standing convention.

---

## Landed: `fix/sizeof-closure` (2026-08-06) - a `sizeof` operand that looks like a generic type stays on the TYPE path

Closes `sizeof-generic-over-closure-type-segfaults-compiler` (P1/crash, deleted; that bucket is
now empty). The floor the issue file set - "reach the same `unknown type` arm `Box<int>` reaches" -
is what landed. The FEATURE question it separated out is filed as
[[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]].

### Root cause

`ParseUnaryExpression` (`cflat/MainListener.h:17712`) services a prefix `sizeof` by rebuilding the
operand's type from raw TEXT, and only takes the type path when a character test passes. That test
was a whitelist of `[A-Za-z0-9_.<>*]`. Every closure spelling carries `(` and `)` in its signature
(`function<int(int)>`), and every multi-argument instantiation carries `,` (`Pair<int,float>`), so
those texts failed the test and fell through to `ParsePostfixExpression`, which evaluated the
parenthesized text as an ordinary expression and returned a NamedVariable with a NULL `Primary`
and NO diagnostic. The `(int)` cast in the filed repro then dereferenced that null inside
`LLVMBackend::CreateCast` (confirmed under lldb: `EXC_BAD_ACCESS` at `+48`, the `Value->getType()`
load) - SIGSEGV, exit 139, zero output. Without a cast the same operand SILENTLY produced garbage:
`long long s = sizeof(Box<function<int(int)>>);` compiled, ran, and printed `8512798976`. The
issue file recorded the crash face only; the silent face is the same defect in a different
containment.

The filed framing ("specifically an instantiation whose type ARGUMENT is a closure type") is
NARROWER than the truth. The discriminator is the CHARACTER, not the closure: `sizeof(Pair<int,float>)`
and `sizeof(map<int,int>)` SIGSEGV identically with no closure anywhere.

### The fix

The character test now admits `(`, `)` and `,` when they occur at generic-bracket depth >= 1, and
requires the brackets to balance when it used that admission. Texts with parens at depth 0
(`sizeof(f(1))`, `sizeof(buf)`, `sizeof(x + 1)`) and unbalanced texts (`sizeof(a<b)`) keep their
existing classification exactly. One site: `likelyType` has no other copy (grepped), and the
neighbouring variable-name gate two blocks below is keyed on `find('<') == npos`, which every
newly-admitted text fails, so it is unreachable from the new admission by construction.

### Coverage matrix - measured pre (`f24fb18`) and post, `-o` compiles, never `--check`

| Cell | PRE | POST |
|---|---|---|
| `sizeof(Box<function<int(int)>>)` | SIGSEGV, no output | `unknown type 'Box<function<int(int)>>'` |
| `sizeof(Box<Lambda<int(int)>>)` | SIGSEGV | `unknown type 'Box<Lambda<int(int)>>'` |
| `sizeof(list<function<int(int)>>)` | SIGSEGV | `unknown type 'list<function<int(int)>>'` |
| `sizeof(Box<function<void()>>)` | SIGSEGV | located `unknown type` |
| `sizeof(Box<function<int(int)>*>)` | SIGSEGV | located `unknown type` |
| `sizeof(Box<Box<function<int(int)>>>)` | SIGSEGV | located `unknown type` |
| `sizeof(Pair<int,float>)` - NO closure | SIGSEGV | `unknown type 'Pair<int,float>'` |
| `sizeof(map<int,int>)` - NO closure | SIGSEGV | `unknown type 'map<int,int>'` |
| same, with the instantiation USED first | SIGSEGV | located `unknown type` |
| same, inside `if const` | SIGSEGV | located `unknown type` |
| `long long s = sizeof(Box<function<int(int)>>)` (no cast) | compiles, runs, prints `8512798976` | located `unknown type` |
| `sizeof(N.Box2<function<int(int)>>)` | `'Box2' does not name a value here` | `unknown type 'N.Box2<function<int(int)>>'` |
| `sizeof(Box<int>)`, `sizeof(Box<Box<int>>)`, `sizeof(list<int>)`, `sizeof(Box<Cb>)` | located `unknown type` | IDENTICAL |
| `sizeof(function<int(int)>)` / `<int(int,int)>` / `<void()>` / `*` | 8 | 8 |
| `sizeof(Lambda<int(int)>)` | 16 | 16 |
| `sizeof(int)` 4, `sizeof(S)` 8, `sizeof(S*)` 8, `sizeof(string)` 16 | as listed | IDENTICAL |
| `sizeof(buf)` on `char[128]` | 128 | 128 |
| `sizeof(f(1))` 1, `sizeof(x + 1)` 2, `sizeof(sizeof(int))` 4 | as listed | IDENTICAL (bogus on both - see below) |
| `sizeof(a<b)` | `unknown type 'a<b'` | IDENTICAL |
| `sizeof(Nope)` | `unknown type 'Nope'` | IDENTICAL |
| `sizeof(int[4])`, `sizeof(*p)` | pre-existing located cast errors | IDENTICAL |
| `alignof(Box<function<int(int)>>)` 8, `alignof(Box<int>)` 4, `alignof(Pair<int,float>)` 4 | as listed | IDENTICAL |

`alignof` is untouched by the diff and could not be otherwise: its operand text starts `alignof(`,
whose paren is at bracket depth 0, so the character test rejects it before and after. That it
nonetheless RESOLVES generic instantiations correctly is the asymmetry now filed as a P2.

`sizeof(f(1))` = 1 and `sizeof(x + 1)` = 2 are wrong on BOTH binaries. They are the expression
path's own answers, they predate this branch, and this fix deliberately does not move them - a
change there would move `sizeof` of a value expression, which is a much larger accept set.

### The axis the matrix MISSED, and what review round 1 found in it

The matrix above enumerated closure spellings, generic-argument shapes, containment (used
instantiation, `if const`, namespace-qualified), and expression operands with parens at depth 0.
It had **no comma-comparison / tuple cell at all** - no operand where the newly-admitted `,` sits
inside brackets that are a COMPARISON rather than a generic argument list. Review round 1 attacked
exactly that gap and found the one spelling the fix takes: `a<b,c>d`, which in cflat is a genuine
two-element tuple expression (`tuple__bool__bool`).

Measured cost, PRE `f24fb18` vs POST:

| Program | PRE | POST |
|---|---|---|
| `sizeof(a<b,c>d);` - value DISCARDED | rc 0, runs, prints `ok` | rc 1, `unknown type 'a<b,c>d'` |
| `i64 z = sizeof(a<b,c>d);` | rc 1, `cannot cast an aggregate value ...` | rc 1, `unknown type 'a<b,c>d'` |
| `i64 z = sizeof(a<b,c>d) + 0;` | rc 1, `no overload of 'operator+' ... [0] tuple__bool__bool` | rc 1, `unknown type 'a<b,c>d'` |
| `tuple<bool, bool> t = (a<b, c>d);` - no `sizeof` | rc 1, `Unknown identifier 'item0'` (the TUPLE on the line above is accepted) | IDENTICAL |

So the loss is one value-DISCARDED no-op statement; every consuming form was already rejected on
PRE with a different message, and the tuple outside a `sizeof` operand is untouched. Left as-is
deliberately and filed as [[sizeof-steals-discarded-tuple-comparison-spelling]] (P3), including
the note that the POST message calls a tuple expression a "type". The obvious repair - fall back
to the expression path when the type lookup fails - is precisely the fallthrough that reached
`CreateCast` with a null `Primary` and SIGSEGVed the compiler, so it is the one repair that must
not be made here; it becomes safe only once the operand is routed through the real `typeName` rule
(the P2's fix), where the PARSER decides instead of a character test.

The generalizable lesson is the one this file already carries about axis products: an operand
classifier's axes are the SHAPES OF TEXT it can see, and "a comma inside angle brackets that is
not a type-argument list" is a shape. Enumerating closure and generic spellings covered the
intended readings of the admitted characters and none of the unintended ones.

### Cells deliberately out of scope

- Making `sizeof` over an instantiation RESOLVE. Not trivially contained: it needs the operand
  re-parsed through the `typeName` rule or a grammar change to the `('sizeof')*` prefix loop, and
  the loop is what makes `sizeof(buf)` and `sizeof(expr)` work at all. Filed as a P2 with its
  accept set enumerated.
- `sizeof(T)` in a generic body where `T` substitutes to a closure spelling
  (`unknown type 'function<int(int)>'`, identical on both binaries) - same P2, same root: the
  prefix handler resolves by source spelling and `GetType` is keyed on the mangled name.
- The `alignof` operand never reaching the text handler at all. Correct as-is; it reaches the
  better resolver.

### Why no differential corpus sweep

The change can only alter the classification of a `sizeof`/`alignof` PREFIX operand, and only for
texts carrying `(`, `)` or `,` inside balanced generic brackets. `grep -rn "sizeof([^)]*<" Test/
example/ cflat/core/ performance/` returns **8 hits, all of them lines this branch added**. No
pre-existing corpus file performs the crossing, so a whole-corpus A/B is structurally incapable of
saying anything here (the standing lesson about zero-difference sweeps, applied in the honest
direction). The evidence is the targeted matrix above plus the two suites, which compile the whole
corpus anyway.

### Test legs

`Test/errors/err_types.cb` - 3 new `expect_error` legs: a thin closure argument, a fat `Lambda<>`
argument, and the closure-free two-argument `SizeofPair<int, float>` (which proves the
discriminator is the character, not the closure). Each was mutation-tested INDIVIDUALLY against
the PRE binary as a single-leg file: all three make PRE exit 139 with no output - i.e. each fails
there for exactly the filed reason, the compiler SIGSEGV - and each flips to `FAIL: expected
error` on the POST binary when its expected string is corrupted.

`Test/test_interface.cb::testSizeofClosureTypeSpellings` - 6 accept-set value legs (thin closure,
fat closure, two-parameter signature, `void()`, closure pointer, and `sizeof` of a named fixed
array). These pass on BOTH binaries by design: they are the frozen accept set for the predicate,
not regression legs, and they are what would have caught an over-broad widening of it.

---

## Landed: `fix/class-undef` (2026-08-06) - a `class` with no user-written constructor default-constructs to zero, not `undef`

Closes and deletes `internal/issue/p1/codegen/class-no-ctor-default-construct-returns-undef.md`.

### The issue file's root cause was WRONG, and the truth is simpler

The file hypothesised that a `struct` with no constructor "leaves `GetFunction` empty and falls
through to a real zero-init alloca", i.e. that struct and class take DIFFERENT paths. Measured,
they take the SAME path: `ParseStructDefinition` also synthesizes `_S_S__` and the declaration
site also calls it. The only difference is the seed of the aggregate the synthetic body builds:

- `MainListener_Aggregates.cpp:300` (struct/union) - `llvm::Constant::getNullValue(structType)`,
  carrying a comment that says exactly why ("so fields lacking an explicit initializer read as
  0/null ... instead of leaking stack garbage").
- `MainListener_Aggregates.cpp:2637` (class) - `llvm::UndefValue::get(structType)`, the
  un-updated twin. Fields WITH an initializer are `CreateInsertValue`d over the seed; fields
  without one are never written, so they kept the `undef`. With no defaulted field at all the
  whole return stayed `undef`: `define internal %C @_C_C__() { entry: ret %C undef }`.

Verified oracle: `struct S {int a; int b;}` emits `ret %S zeroinitializer` and
`struct SD {int a = 5; int b;}` emits `ret %SD { i32 5, i32 0 }` - so the struct reference is
correct on BOTH the zero axis and the field-default axis before being matched.

### Fix shape

Shape 2 from the issue file (give the synthesized ctor a real body), one token:
seed with `Constant::getNullValue` instead of `UndefValue::get`. Shape 1 (do not register the
ctor at all) was rejected on measurement: `struct Box<T> { T v; }` instantiated at `Box<CNoCtor>`
calls `_CNoCtor_CNoCtor__` from the CONTAINER's own default ctor, so the symbol is depended on.

Site audit for the seed: all four synthetic-ctor emitters in `MainListener_Aggregates.cpp` now
agree - struct/union (300), imported program (1813), program (2142), class (2639). The class one
was the sole outlier; no `UndefValue::get` remains in `MainListener_Aggregates.cpp`, the survivors
elsewhere in the listener are all closure/thin-fn-ptr seeds that are fully insertvalue'd before
use, none an aggregate ctor seed.

### Out of scope, measured pre AND post, identical on both

- Globals never run the ctor at all. `SD gs;` / `CD gc;` / `= default` all print `0 0` on both
  binaries for BOTH struct and class - field defaults are dropped at global scope. That is
  [[global-struct-no-initializer-ignores-field-defaults]], already filed, untouched here.
- `string s = "hi";` as a field default is dropped for struct AND class, both binaries
  (`s=[] n=0` post, `s=[]` pre). Pre-existing, shared, not this fix.
- Positional `C c = {1,2}` is a hard error for class and struct alike, unchanged.
- A `Test(...)` message string whose CONTENT contains `{}` retypes the literal as `string` and
  breaks overload resolution - hit while writing the legs; it is the already-filed
  [[string-literal-containing-braces-retyped-as-string]] (FIXED 2026-08-06 by `fix/brace-literal`).

### Verification

`./test.sh Release` 600 passed / 0 failed / 8 skipped. `example_mac.sh Release` 35 passed /
0 failed. Differential corpus sweep (compile+run of every `Test/*.cb` and `example/**/*.cb`,
184 entries, both binaries): the only non-nondeterministic difference is the intended new legs
in `test_initializer_list.cb`; the other 9 diffs are addresses, PIDs, timings and thread-race
counters. `leaks --atExit` on an owning-field probe: 186 nodes / 16 KB / 0 leaks on BOTH
binaries, destructor count 3 on both. Warm `--init-local` cache re-verified (the fix adds no
field, so nothing enters the serializer).

### fix/return-gate - the RETURN leg of the closure provenance gate, LANDED

Fixed and deleted [[data-pointer-returned-as-closure-not-gated]]. `CoerceToFuncPtrReturn` was the
one caller of `WidenBareOrThinToClosureFat` never routed through the `ce9858e` provenance gate -
`Lambda<int(int)> f() { void* p = &g; return p; }` (bare and `?:`/`??` join spellings, both fat
`Lambda<>` and thin `function<>` return types) widened a data pointer into the CODE slot and
called it (exit 138/139, no diagnostic). The THIN return arm (`return builder->CreateBitCast(val,
BuildThinFnPtrType(retTV), ...)`) had the identical hole and was not named in the issue file - it
shares the same function and is fixed by the same change.

Fix: a new `CheckClosureReturnProvenance(val, returnNV, thin)` reuses
`ArgumentIsProvablyDataPointer` - the SAME predicate the two argument-passing sites already share
- against the return expression's own `NamedVariable` (`returnNV`, already resolved by
`ParseAssignmentExpressionNamed` at the call site), so the accept set cannot drift between "pass"
and "return". Return-flavoured wording ("cannot return {} as a closure" / "... as a 'function<>'
value") distinguishes it from the argument-site diagnostics in a mutation-testable way.

Accept-set matrix (bare/local, named function, thin `function<>` value, fat `Lambda<>`
passthrough, lambda literal, `?:` join of two legal closures (named-fn and value arms), `??` join
of two legal thin values, explicit `(function<...>)value` cast escape hatch, closure read from a
struct field, closure read from a global, closure returned via a nested call, generic
`Box<Lambda<>>` field read) - 15 probes, every one measured BYTE-IDENTICAL (same compile exit,
same runtime output) on `x64/Release/cflat` before and after. Reject-set (bare `void*`/`int*`
return, `?:` join of two data pointers, `??` join of two data pointers, thin twins of all three,
a generic `Box<void*>` field returned as a closure) - 7 probes, every one silently miscompiled
(138/139) pre-fix and now diagnoses with the return-flavoured message post-fix.

The one user-visible tightening: a data-TYPED pointer that provably holds a code address,
returned as a closure, was accepted and ran correctly on master and now hard-errors. Measured:
`Lambda<int(int)> f() { void* vp = (void*)namedFn; return vp; }` ran `r=11` on the PRE binary; the
`i8*` spelling behaves identically. Both now diagnose post-fix. This is deliberate and matches
the argument-site behaviour on master, where the identical spellings already reject as arguments;
the documented escape hatch `(function<...>)value` still works at the return site (measured,
`r=11` post-fix).

Differential sweep: `--check` over all 447 `Test/` + `example/` `.cb` files, pre vs post - the
ONLY difference is `Test/errors/err_data_pointer_to_closure_param.cb`, the intended new legs.
macOS arm64 Release `test.sh` 600/0/8, `example_mac.sh` 35/0.

Confirmed OUT of scope, measured unchanged (still exit 138 on both binaries): the ASSIGNMENT-path
sibling [[data-pointer-assigned-to-thin-function-value]] (`function<int(int)> f = vp;` and
`s.f = vp;`) - a different site (`MainListener_Expressions.cpp`'s `operatorText == "="` block),
left open per that issue file. Also probed as a neighbour and confirmed still ungated: the plain
(non-generic) thin `f = vp;` reassignment and the thin arm of a generic-substituted closure field
assignment (`enc->IsThinFnPtr() && right->getType()->isPointerTy()`, `MainListener_Expressions.cpp`
around the generic-field conversion block) - both fall through every existing conversion arm
untouched, same root cause as the filed sibling, not fixed by this change.

Test legs: `Test/errors/err_data_pointer_to_closure_param.cb` gained 7 `expect_error` blocks - fat
bare (through a local), fat bare (address-of straight into the return), fat `?:` join, thin bare,
thin `?:` join, fat `??` coalesce join, and a generic `Box<void*>` field returned as a closure
(exercises the field-read arm of `ArgumentIsProvablyDataPointer` through a monomorphized
struct; a non-generic field twin rejects identically) - covering both return
flavours (fat `Lambda<>`, thin `function<>`), both bare/join spellings, and the coalesce/generic
shapes. Each leg was isolated into its own single-leg scratch file and mutation-tested there
individually: every one exits 1 on the PRE binary (merge-base `33b3ac4`) with "expected error ...
did not occur", and every one exits 0 with `PASS: expected error received` on the POST binary,
firing the return-flavoured wording - proving each leg is non-vacuous on its own, not merely as
the first failure in a longer file.

---

## Landed: `fix/field-brace` (2026-08-06) - a struct FIELD's own `= { ... }` default brace list is applied, not discarded

Closes and deletes `internal/issue/p1/codegen/struct-field-default-brace-list-discarded.md`.

### Root cause - the issue file's hypothesis was in the right area but named the wrong function

The file guessed `GenerateDefaultValue`. `GenerateDefaultValue` is never reached: the five
default-constructor emitters route a field's default expression themselves, and each one asks
only two questions of the field's `InitializerContext`:

```cpp
if (auto* ae = initializer->assignmentExpression()) ...
else if (initializer->Default()) ...
```

`CFlat.g4:521` gives `initializer` THREE alternatives - `assignmentExpression`,
`'{' initializerList? ','? '}'`, and `Default`. The brace-list alternative matched neither arm,
so `rvalue` stayed null and the field fell into the "no initializer" fallback: for a struct-typed
field that calls the field type's own default ctor (all-zero for `Inner {int x; int y;}`), for a
scalar it leaves the zero seed. Nothing dropped the list *late*; it was never read at all.

The scalar-default ORACLE was verified independently before being matched: `struct S {int a = 9;
int b;}` emits `ret %S { i32 9, i32 0 }` and prints `a=9 b=0` on the PRE binary. It is honoured
because `int a = 9` IS an `assignmentExpression`.

### The second brace SPELLING - the trap this family has now sprung twice

`CFlat.g4:286` `initDeclarator` has a third alternative, `declarator '{' initializerList? ','? '}'`,
so `Inner i { x = 1, y = 2 };` at FIELD position is grammatical and hangs its list on
`initDeclarator`, where `typeValue.Initializer` is null. Gating the fix on
`initializer->initializerList()` alone would have left that spelling reproducing the closed bug -
exactly the hole `fix/global-positional` shipped and had to re-open. Measured PRE: `= { x=1,y=2 }`
gives `x=0 y=0` and `{ x=1,y=2 }` gives `x=0 y=0`; POST: `1 2` and `3 4` respectively (leg
`field brace default bare spelling`, discriminator 3004).

`DeclTypeAndValue` therefore gained `BraceInitializer` (`LLVMBackend.h`), set from
`initDecl->initializerList()` in `MainListener_Declarations.cpp`, and `FieldDefaultBraceList()`
resolves the two spellings to one list.

### Fix shape

`MainListener::ParseFieldDefaultBraceInitializer` (`MainListener_Expressions.cpp`) mirrors the
LOCAL declarator, which is the working reference for the same construct:

1. Bail (return null, caller keeps its existing handling) when the field is a pointer, an
   `ElemPointer`, an array view or a fixed array.
2. Bail when `GetDataStructure(TypeName).StructType` is null - a primitive or an unsubstituted
   generic parameter.
3. Otherwise alloca the field type, SEED it with the field type's own default ctor result
   (`GetFunction` exact-key guard, `forceRoot`), falling back to `getNullValue`.
4. `TryEmitContainerInitializer` if the field is a `list`/`array`/`dictionary`, else
   `EmitFieldInitializer`.
5. Load the slot; the caller `CreateInsertValue`s it into the returned aggregate (or stores it
   through `this` at the user-ctor site).

Step 3 is what makes a PARTIAL list correct rather than merely non-zero: `struct FbSeed {int x=7;
int y=8; int w=9;}` under `FbSeed s = { x = 1 }` must read `1 8 9`, not `1 0 0` and not `7 8 9`.
The leg's discriminator `x*100 + y*10 + w == 189` is unreachable from either wrong answer.

### Site audit - all five field-default readers

| Site | What it emits | After the fix |
|---|---|---|
| `MainListener_Aggregates.cpp` struct/union ctor (~262) | insertvalue into a returned aggregate | honours brace lists (the UNION arm returns `getNullValue` and never reads field initializers at all - unchanged, `c3` identical on both binaries) |
| `MainListener_Aggregates.cpp` imported-`program` ctor (~1796) | same | unreachable: this emitter's `declList` is entirely synthetic (no user-declared fields), so `FieldDefaultBraceList` never has a field to fire on; wired for symmetry with the other four sites |
| `MainListener_Aggregates.cpp` `program` ctor (~2125) | same | honours brace lists |
| `MainListener_Aggregates.cpp` class ctor (~2617) | same | honours brace lists |
| `MainListener_Aggregates.cpp` user no-arg ctor (~3035) | GEP + store through `this` | honours brace lists (leg `field brace default user no-arg ctor`, 1009) |
| `GenerateDefaultValue` | field-independent default of a TYPE | unchanged - it has no field initializer to read; the `= default` arms that call it are untouched |
| Global emission (`MainListener_Declarations.cpp`) | compile-time `llvm::Constant` | UNCHANGED and still wrong; a global never runs a ctor. That is [[global-struct-no-initializer-ignores-field-defaults]], out of scope, frozen as a leg |
| Brace-init merge (`EmitFieldInitializer` at a declarator) | overrides on top of the default value | unchanged; it now runs on top of a correctly-seeded default |

Each aggregate site wraps the new call in `GlobalScopeGuard`, matching the `= default` arm next to
it: the file-scope struct walk leaves `global_scope` true, and the brace path emits real stores
and calls.

### Coverage matrix - every cell measured on BOTH binaries (PRE = verified `68c78fc` Release)

Probe corpus: `scratch/fb_*.cb`, tables `scratch/fb_pre.txt` / `scratch/fb_post.txt`.

CHANGED (all were silent wrong values):

| Cell | PRE | POST |
|---|---|---|
| struct, `=` spelling (`c1`) | `x=0 y=0 z=0` | `x=1 y=2 z=0` |
| struct, BARE spelling (`s8`) | `x=0 y=0` | `x=1 y=2` |
| class (`c2`) | `x=0 y=0 z=0` | `x=1 y=2 z=0` |
| nested one level down (`c4`) | `x=0 y=0 z=0 w=0` | `x=1 y=2 z=0 w=0` |
| generic container `Box<Outer>` (`c5`) | `x=0 y=0` | `x=1 y=2` |
| generic field `Box<T> { T v = {x=3,y=4} }` (`e4`) | `x=0 y=0` | `x=3 y=4` |
| user no-arg ctor (`c6`) | `x=0 y=0 z=7` | `x=1 y=2 z=7` |
| user parameterized ctor (`c7`) | `x=0 y=0 z=7` | `x=1 y=2 z=7` |
| PARTIAL list (`s4`) | `x=0 y=0` | `x=1 y=0` |
| partial over inner scalar defaults (`m3`) | `x=7 y=8 w=9` | `x=1 y=8 w=9` |
| brace default reached via `= default` of a default (`s5`) | `x=0 q=0` | `x=1 q=0` |
| namespaced (`e5`) | `x=0 y=0` | `x=1 y=2` |
| inner type with a destructor (`e6`) | `x=0 y=0` | `x=1 y=2` |
| two brace-defaulted fields + a scalar (`e8`) | `0 0 5` | `1002 3004 5` |
| interface-implementing class, through the fat slot (`m1`) | `0` | `1002` |
| container field `list<int> l = {1,2,3}` (`m7`,`e7`) | `n=0 s=0` | `n=3 s=6` |
| call in the list, run-once (`m4`) | `x=0 y=0 calls=0` | `x=1 y=100 calls=1` |
| instantiation: `O o;` / `= {}` / `o {}` / `= default` / `new O()` / `O[3]` (`i1`-`i5`,`i7`) | `0` each | `1002` each |
| container brace-init not naming the field (`i9`) | `0 z=5` | `1002 z=5` |
| non-braced sibling field of a braced one (`m5`) | `p=0 x=0` | `p=0 x=5` |
| POSITIONAL list at field position (`s2`) | compiles, silently `x=0 y=0` | HARD ERROR, the declarator's own wording |
| `program` config field brace default (`Test/test_program.cb` leg `program config field brace default`) | `exitCode=0` | `exitCode=1008` |

BYTE-IDENTICAL, measured in the exact spelling of the claim:

| Cell | PRE = POST |
|---|---|
| union field (`c3`) | `x=0 y=0` - the union arm returns a zeroed value and reads no field initializer |
| `= {}` over inner scalar defaults (`s3`) | `x=4 y=5` - empty list, no `initializerList`, still default-constructs |
| `= default` field default (`s6`) | `x=4 y=5` |
| scalar field default, the oracle (`s7`) | `a=9 b=0` |
| GLOBAL `Outer g;` (`i6`) | `0` - globals never run the ctor |
| explicit outer brace naming the field (`i10`) | `x=9 y=9 z=4` |
| non-braced generic field (`m6` second value) | `0` |
| POINTER field `Inner* p = {x=1}` (`m8`) | `0` (nullptr), silently |
| `int a = {5}` / `int a {5}` primitive field (`e1`,`e2`) | `a=0 b=0` |
| FIXED-ARRAY field `int[3] a = {1,2,3}` (`e3`) | `0 0 0 b=0` |
| `string s = "hi"` inside the braced inner struct (`m2` string half) | `s=[]` - the pre-existing string-field-default drop; the INT half of the same struct went `x=0` -> `x=3` |

Crosses deliberately NOT taken, and why: a brace-list value nested inside a brace list
(`{ a = { x = 2 } }`) and its declarator twin `Outer o = { i = { x = 9 } };` are PARSE ERRORS -
`CFlat.g4:531` `fieldInit` is `Identifier '=' assignmentExpression`, and a brace list is not an
assignmentExpression. Confirmed from the grammar and by measuring
(`no viable alternative at input 'a = {'`), not inferred. Nesting is therefore only reachable one
level at a time, through a field whose type itself carries a brace default - which IS covered
(`c4`, `s5`).

### The one new REJECTION, and its accept set

A POSITIONAL element in a field's default brace list (`Inner i = { 1, 2 };`) now gets
`EmitFieldInitializer`'s existing message, identical to the one the LOCAL declarator has always
given: `positional initializers are not supported for struct type 'Inner'; use 'field = value'`.
It cannot break a working program - PRE the same spelling compiled and produced all zeros, i.e.
the values were already discarded. The rejection is unreachable only for the containers
`TryEmitContainerInitializer` actually handles - `list`/`array`/`dictionary` (matched by mangled
prefix `list__`/`array__`/`dictionary__`), whose positional form is consumed there before
`EmitFieldInitializer` is called (`m7`/`e7` prove that path stays open). `hashset`/`stack`/`queue`
fields are NOT in that set: a positional list on one of those field types now hits the same
rejection the LOCAL declarator has always given, converting a PRE silently-empty container into a
diagnostic (measured: the identical positional spelling was already rejected at LOCAL declarator
scope on PRE, so this closes the same gap at field-default scope - no working program regresses).
The rejection is also unreachable for any field the helper bails on in steps 1-2 above - pointer,
view, fixed array, primitive, unsubstituted generic parameter - all measured byte-identical in the
table.

### Verification

- `./test.sh Release`: 600 passed / 0 failed / 8 skipped (baseline 600/0/8; the suite counts
  FILES, and both new legs went into existing files).
- `bash example_mac.sh Release`: 35 passed / 0 failed.
- Differential corpus sweep, compile AND run, 432 entries (`Test/*.cb`, `Test/errors/*.cb`,
  every `example/**/*.cb`), both binaries: 15 differing entries, of which 14 are addresses
  (`test_c`), timings (`test_time`, the hpc benchmarks, `framework_link`), PIDs
  (`sysinfo_mac`), cwd (`shell_pwd`), thread-race counters (`test_hpc`) and a `/private` prefix
  on the `/tmp` PRE worktree's core path (4 Windows-only UI files that fail to import
  `windows.h` on both). The ONE real difference is `Test/test_initializer_list.cb` - the
  intended new legs.
- `leaks --atExit` on the container-field-default probes (`fb_m7`, `fb_e7`, the newly-allocating
  cells): 0 leaks / 0 bytes on both.
- Warm `--init` cache: `--init-clear-local`, cold compile, `--init-local`, re-compile - the repro
  prints `x=1 y=2 z=0` on both, and `c4`/`c5`/`c6`/`m7` are unchanged warm. The new
  `BraceInitializer` field is a PARSE-TREE POINTER, the same class as the existing `Initializer`
  next to it, and like it is not serialized: the cache stores compiled core bitcode, so a cached
  type's synthesized ctor is already lowered and never re-reads a field initializer. No core
  `.cb` uses a brace field default (swept), so nothing enters the round-trip.

### Test legs

`Test/test_initializer_list.cb` - 20 new legs. 17 FAIL on the PRE binary with the exact expected
values recorded; 3 pass on both BY DESIGN and are labelled in the file:
`field brace default empty still default constructs` (789 - the frozen accept set for `{}`),
`field brace default at global scope still zero` (0 - the frozen
[[global-struct-no-initializer-ignores-field-defaults]] behaviour), and
`field brace default leaves other fields zero`. Discriminators are `x*1000+y` sums
(1002, 3004, 5006, 41002) and positional digit packs (189 vs 789 vs 000), so neither a zero fill
nor an ignored list can produce them.

`Test/errors/err_struct_positional_init.cb` - one new file-scope scoped `expect_error` block for
the positional-at-field-position rejection. It fires at `(7,40)`, a different site from the
pre-existing declarator leg at `(13,15)`; on the PRE binary the file exits 1 with
`FAIL: expected error ... did not occur`.

`Test/test_program.cb` - one new leg, `program config field brace default`, covering the
`program` default-ctor emitter (ParseProgramDefinition, ~2125) via a `FbConfig i = { x = 1 };`
config field read back through `.run()`/`exitCode`. FAILS on the PRE binary
(`exitCode=0`, expected `1008`).

### Filed by this round

[[fixed-array-field-brace-default-discarded]] (P2) - the FIXED-ARRAY sibling, same root cause,
different emitter (positional, needs an `[N x T]` aggregate builder), plus the POINTER-field cell
that still silently reads `nullptr`. Both measured identical on `68c78fc` and this branch.
### fix/assign-gate - the ASSIGNMENT leg of the closure provenance gate, LANDED

Fixed and deleted [[data-pointer-assigned-to-thin-function-value]]. The `ce9858e` argument gate
and yesterday's `fix/return-gate` (`CheckClosureReturnProvenance`) left the ASSIGNMENT direction
open: `f = vp;` and `s.f = vp;` on a thin `function<>` destination stored a DATA address straight
into the code slot with no diagnostic (exit 138, SIGBUS). A thin `function<>` slot is a bare
pointer, so the generic aggregate-store cast that accidentally catches the FAT `Lambda<>` twin
(a closure is a struct; "cannot store a pointer value into struct storage") never sees it - a
`Pointer -> Pointer` bitcast in `CreateCast` accepted the store silently.

Fix: a new `CheckThinFnPtrAssignProvenance(val, arg, destDesc)` reuses `ArgumentIsProvablyDataPointer`
- the SAME predicate the argument and return gates already share - so all three "pass", "return",
and "assign" accept sets cannot drift. Assignment-flavoured wording ("cannot assign {} to
'function<>' destination {}") distinguishes it from the argument ("... parameter") and return
("... as a ... value") wordings in a mutation-testable way. SEVEN separate lowering paths needed
the gate, since none of them share code with each other:

- `ParseAssignmentExpression` (`MainListener_Expressions.cpp`) - the plain `=` operator. Covers
  local, struct field, nested field, through-pointer, array element, and global destinations alike,
  since they all resolve to one destination-agnostic `NamedVariable` before this point - confirmed
  by probing all six spellings, not inferred. Also covers the generic-encoded thin element
  (`Box<function<>>.item = vp`), mirroring the existing FAT `WidenToClosureFatChecked` call in the
  same encoded-closure-field block.
- `ParseDeclaration`'s decl-init path (`MainListener_Declarations.cpp`) - `function<T> f = vp;` is
  a genuinely separate lowering, not a call into the `=` path. `rightNV` itself is out of scope by
  the check site (it lives inside an inner block whose other facts are hoisted into named locals
  the same way this file already does for a dozen other properties), so the gate's evidence is a
  reconstructed `NamedVariable` carrying only the three flags `ArgumentIsProvablyDataPointer` and
  `DescribeNonFunctionArgument` read (`IsFunctionPointer`, `Pointer`, `TypeName`).
- `EmitOneFieldInit` (`MainListener_Expressions.cpp`) - brace field-init (`S s = { f = vp };`), a
  THIRD lowering path. `rightNV` is a real parameter here, so no reconstruction was needed; added
  as the thin sibling of the existing FAT `WidenToClosureFatChecked` branch in the same block.
- `ParseFieldDefaultInitializer` (`MainListener_Expressions.cpp`) - a struct FIELD DEFAULT
  (`struct S { function<T> f = gvp; };`), a FOURTH lowering path, reached only when a `default`
  expression constructs the struct. Found by review, not in the original round.
- `EmitPositionalFixedArrayInit` (`MainListener_Expressions.cpp`) - a positional FIXED-array
  brace-init (`function<T>[N] arr = { vp, vp };`), a FIFTH lowering path. Found by review.
- `EmitArrayViewInferredInit` (`MainListener_Expressions.cpp`) - an array-VIEW brace-init
  (`function<T>[] arr = { vp };`), the array-view twin of the fixed-array path, a SIXTH lowering
  path. Found by review. `EmitGlobalFixedArrayInit` (the GLOBAL form of the fixed-array spelling)
  is not a gap - a global array initializer is already required to be a compile-time constant, so
  it is rejected earlier by that unrelated diagnostic, before this gate would ever see it.
- `GenerateDefaultParamOverloads` (`MainListener_Statements.cpp`) - a PARAMETER-DEFAULT value
  (`int f(function<T> cb = vp)`), a SEVENTH lowering path. Found in round-2 review. The wrapper's
  forwarded default is built directly at this site (`defaultVal` from `LoadNamedVariable(defNV)`),
  so no reconstruction was needed - the gate sits right after the existing opposite-direction
  `RejectCodeValueIntoDataSlot` call, reusing the same `defNV`. The argument gate at the call site
  inside the generated wrapper never sees this: the forwarding `NamedVariable` that reaches that
  call is rebuilt with the DESTINATION's `TypeName` (`MainListener_Statements.cpp` around line
  2378), which launders provenance before `CheckThinFnPtrArgProvenance` would run - so this gap
  can only be closed at the default-value site itself, not the call site. Covers both free
  functions and struct methods, which share this one emitter.

Guard polarity bug found and fixed mid-round by `./test.sh`: the first two sites' new branches were
missing the `!typeAndValue.Pointer` guard the pre-existing FAT/encoded-thin branches next to them
already carry, so `function<int(int)>* p = b.item;` (a POINTER to a thin slot, not the slot itself -
`Test/test_function_ptr.cb`'s `tgp_box_load` cell) false-rejected. `EmitOneFieldInit` was already
safe - its whole closure-conversion block is wrapped in `!fieldType.Pointer` upstream.

Accept-set matrix (named function, `function<>` value read from a local/field, `?:` join of two
`function<>` values, `nullptr`, explicit `(function<...>)value` cast escape hatch, `function<>`
parameter, call returning `function<>`, all six spellings above) - one combined probe file,
byte-identical compile output and runtime values on `x64/Release/cflat` before and after (worktree
`fix/assign-gate` vs a clean build of its base `68c78fc` in a scratch worktree). Reject-set (bare
`void*`/`char*`/`int*`, `?:` join of two data pointers) x (local, field, nested field,
through-pointer, array element, global, generic-encoded field, decl-init, brace-init) - 12 probes,
every one measured silently accepted (compile exit 0, run exit 138) on the PRE binary and now
diagnoses with the assignment-flavoured message post-fix. The FAT twin (`Lambda<>`/`Box<Lambda<>>`)
was reconfirmed rejected, unchanged, by the pre-existing accidental struct-storage message on both
binaries at the plain `=`, decl-init, brace field-init, and fixed-array/array-view brace-init
spellings - not this gate. Round-3 review found the fat twin is NOT rejected at the two
default-value sites (field default `Lambda<> f = gvp;` SIGSEGVs, parameter default
`f(Lambda<> cb = gvp)` SIGBUSes) - pre-existing on the base binary, filed as
`internal/issue/p1/codegen/data-pointer-into-fat-closure-default-not-gated.md` and fixed by
`fix/fat-default` (see its landed record below).

Differential sweep: `--check`/compile over all 444 `Test/` + `example/` `.cb` files (`Test/errors/`
run as whole-file compiles, not `--check`), pre vs post in a scratch worktree at the merge-base -
the only differences are the intended new test legs and per-worktree absolute paths embedded in
unrelated Windows-only "imported file not found" messages (`windows.h` et al., confirmed by diff to
be path text only). macOS arm64 Release `test.sh` 600/0/8, `example_mac.sh` 35/0.

Amend round (review): the three additional lowering paths above (field default, positional
fixed-array brace-init, array-view brace-init) were each confirmed compiling clean and SIGBUSing
(exit 138) on a `68c78fc` PRE binary, then confirmed diagnosing with the assignment-flavoured
message post-fix; the array-shape accept legs (`function<T>[N] arr = { f, g };` and the `[]` twin,
named functions) and the field-default accept leg were reconfirmed unchanged (correct runtime
values, not just exit 0). Also fixed in the same round: a cosmetic empty-`destDesc` bug where a
bare GLOBAL thin destination (`gf = vp;`) rendered `'function<>' destination ` with empty quotes,
because the global spelling leaves `NamedVariable::CallerName` unset; the two `ParseAssignmentExpression`
call sites now fall back to `TypeAndValue.VariableName`, then to the literal "the destination",
same as the existing `CallerName`-fallback convention used elsewhere in this file. `./test.sh`
600/0/8 and `example_mac.sh` 35/0 held after the amend.

Found and filed, NOT fixed here (different mechanism, wider blast radius): the `??=` coalesce-assign
spelling on a thin `function<>` destination is a STILL-OPEN instance of the same hole -
`f ??= vp;` compiles and SIGBUSes, because `??=` evaluates its RHS through a value-only path that
discards the `NamedVariable` this gate reads, for reasons already documented for the
opposite-direction code-value gate. Added as a new dated section to the pre-existing
[[coalesce-assign-skips-store-bookkeeping]] (its general "`??=` skips the shared post-store tail"
root cause covers this too). The FAT `Lambda<>` twin is not open by this mechanism - `??=`'s
scalar-condition requirement rejects it first, for an unrelated reason.

Test legs: `Test/errors/err_data_pointer_to_closure_param.cb` gained 11 `expect_error` blocks -
bare local, struct field, generic-encoded field, decl-init, brace field-init, an `int*` source (to
pin that the message differentiates by source TYPE, not just a generic phrase), a `?:` join of
two data pointers (exercises `JoinDeliversDataValue`, the same per-value ledger fallback the
return-gate join legs use), the amend round's field default, positional fixed-array brace-init,
and array-view brace-init legs, plus a second amend round's parameter-default leg (covers both
free-function and struct-method defaults, since they share one emitter). Each leg was isolated
into its own single-leg scratch file and mutation-tested there individually against a clean build
of the merge-base `68c78fc`: every one fails with "... did not occur" pre-fix and passes with the
assignment-flavoured wording post-fix - the parameter-default leg's PRE binary was also confirmed
to compile the un-wrapped repro clean and exit 138 (SIGBUS) at runtime, matching the other legs'
PRE behaviour.

Second amend round (round-2 review): the parameter-DEFAULT path above was the sole blocker - the
call-site argument gate cannot see it because `GenerateDefaultParamOverloads` rebuilds the
forwarding `NamedVariable` with the destination's type before the call, laundering the source's
provenance. Gated at the default-value site instead, immediately after the existing opposite-
direction `RejectCodeValueIntoDataSlot` call. `./test.sh` 600/0/8 and `example_mac.sh` 35/0 held
after this amend. `??=` on a thin `function<>` destination was the last still-open THIN spelling of
this defect class; it is CLOSED as of `fix/coalesce-tail` (see its landed record below), which
routes `??=` through the shared store tail so it reaches this gate with a real `NamedVariable` -
no THIN spelling of any assignment-direction gate remains open. The two FAT default-value spellings found by round-3
review were filed separately and are now closed by `fix/fat-default` (see its landed record
below) - no THIN or FAT spelling of the default-value gate remains open.

### fix/fat-default - the FAT twin of the default-value closure provenance gate, LANDED

Fixed and deleted
[[data-pointer-into-fat-closure-default-not-gated]]. `fix/assign-gate`'s `??=`-adjacent amend
closed the THIN default-value gap at both `ParseFieldDefaultInitializer` and
`GenerateDefaultParamOverloads`, but neither site had an `else` branch for a FAT `Lambda<>`
destination - unlike every other fat-destination site (`=`, decl-init, brace field-init,
fixed-array/array-view brace-init), where a closure is a struct and the generic aggregate-store
cast rejects a pointer source by accident. The two default-value sites store the raw value
directly with no cast in between, so nothing objected: `struct D { Lambda<int(int)> f = gvp; };
D d = default; d.f(1);` SIGSEGVed (exit 139), and `int f(Lambda<int(int)> cb = gvp) { return
cb(1); }` SIGBUSed (exit 138), both with no diagnostic.

Fix: a new check-only `CheckFatClosureAssignProvenance(val, arg, destDesc)`
(`LLVMBackend.h`/`LLVMBackend_WinRT.cpp`), the fat sibling of `CheckThinFnPtrAssignProvenance`,
sharing `ArgumentIsProvablyDataPointer` so the accept set cannot drift from the thin gate or the
argument/return gates. Unlike `WidenToClosureFatChecked` (the fat gate used at call-argument and
brace-init sites), it does NOT widen - it only rejects what is provably data, mirroring
`CheckClosureReturnProvenance`'s fat arm, which is also check-only. Wired in at both sites as an
`else` branch alongside the existing thin check:

- `ParseFieldDefaultInitializer` (`MainListener_Expressions.cpp` ~5845) - `clo->IsThinFnPtr()`
  now branches to `CheckThinFnPtrAssignProvenance` (unchanged) or
  `CheckFatClosureAssignProvenance` (new).
- `GenerateDefaultParamOverloads` (`MainListener_Statements.cpp` ~2316) - same branch, added as an
  `else if (thinDest)` after the existing thin check. Already proven (by the thin round) to serve
  both free-function and struct-method defaults through one emitter, so no separate method-twin leg
  was needed.

Why check-only, not widen: measured first, per the skill's guard-polarity rule. A LEGAL fat
default (a named function assigned to a `Lambda<>` field default, `struct D { Lambda<int(int)> f
= addOne; }; D d = default; d.f(1);`) was measured and found to ALREADY crash (SIGSEGV, exit 139)
on the PRE binary at the FIELD-default site (review round 1 measured the same crash for a thin
`function<>` VALUE source, so the bug is not named-fn-specific; filed as
[[fat-field-default-legal-source-not-widened]]) - the raw function pointer is stored straight into the
two-word fat struct with no `{code, null}` wrapping, so the runtime call ABI mismatches. This is a
separate, pre-existing bug in `ParseFieldDefaultInitializer`, unrelated to data-pointer provenance,
and out of scope for this fix; implementing widening here would have silently "fixed" it without
measuring the blast radius, which the skill forbids. The PARAMETER-default site does NOT have this
problem - a named-function parameter default is forwarded as a call argument to the wrapper's
inner call, which already widens correctly through the existing `LowerByValueArg` /
`WidenToClosureFatChecked` path at the call site, confirmed unchanged pre/post (compiles clean,
returns the correct value both before and after this fix). `CheckFatClosureAssignProvenance`
itself still only rejects - it never widens on its own - but the FIELD-default call site now
follows it with an explicit widen call, closing [[fat-field-default-legal-source-not-widened]];
see `fix/fat-widen`'s landed record below for the follow-up and its measured matrix.

Accept-set matrix, measured on both the PRE and POST binaries (identical unless noted): named-fn
field default (SIGSEGV both, pre-existing bug, confirmed unchanged, out of scope), named-fn
parameter default (compiles, returns correct value, both), `nullptr` field/parameter default
(compiles, both), `Lambda<>`-value global field/parameter default (compiles, returns correct
value, both), explicit `(Lambda<...>)value` cast escape hatch field/parameter default (rejected
with the pre-existing cast-diagnostic, both, unchanged), capturing-lambda-literal FIELD default
(module verification failure, both, unchanged - a pre-existing, separately-filed bug), thin
`function<>` sibling field/parameter default (rejected with the existing thin wording, both,
byte-identical). The known-broken lambda-literal PARAMETER default
([[lambda-literal-param-default-invalid-ir]]) was re-measured and is byte-identical pre/post
("Module verification failed: Found return instr that returns non-void in Function of void return
type") - this fix does not touch, mask, or worsen it. The two reject-set cells (field default,
parameter default, both fed a `void*` provably-data source) went from silent accept (compile exit
0, run exit 139/138) to a compile-time reject with the new closure-flavoured wording ("cannot
assign a 'void*' value to closure destination ...").

Test legs: `Test/errors/err_data_pointer_to_closure_param.cb` gained 2 `expect_error` blocks (field
default, parameter default), appended after the existing thin-sibling legs. Each was proven to
fail on the PRE binary (`28ef745`, built in a throwaway `git worktree`, removed after use): the
field-default leg compiled clean with no diagnostic (matching the SIGSEGV repro); the
parameter-default leg also compiled clean and then exited 138 (SIGBUS) at runtime. Both pass with
the new wording post-fix.

`./cmake_build.sh release && bash test.sh Release` - 600 passed, 0 failed, 8 skipped.
`bash example_mac.sh Release` - 35 passed, 0 failed. One commit, `git rev-list --count
28ef745..HEAD` = 1.

### fix/iface-global - a brace list with values on a NON-AGGREGATE global REJECTED, matching local (LANDED)

Closes the P1 [[interface-typed-global-brace-init-discarded]] (file deleted). Branch
`fix/iface-global`, one commit on `0a45763`.

#### Root cause, as measured (the filed hypothesis was right, and understated the scope)

The global declarator's brace-list reject added by `fix/global-positional` fires only when
`compiler->GetDataStructure(scalarTypeName).StructType != nullptr`. An interface name is not in
the struct table at all (interfaces live in `interfaceTable`), so `I gi = { a = 1 };` fell
through unguarded into the pre-existing discard: `right` stays the type's zero default and the
global lands as a Constant built from it. Measured with `--out-lli` as the issue file demanded
rather than assumed: `@gi = global %__iface_fat_ptr zeroinitializer`, BYTE-IDENTICAL to the
`I gi = default;` spelling, so `gi` reads back null (probe `gi == nullptr ? 5 : 6` exits 5 on
`0a45763`). The interface-boxing machinery never runs, exactly as the issue file guessed: a
brace list is an `initializerList`, not an `assignmentExpression`.

What the issue file did NOT say is that the same hole swallows every other non-aggregate type.
The predicate asks "is this a registered struct", and the answer is also no for primitives,
`char*`, `function<>` and `simd` - all of which the LOCAL declarator has always rejected. So
this was never an interface bug; it was a global-scope hole with an interface-shaped repro.

#### Oracle, verified INDEPENDENTLY before being used as one

The fix is specified as "make global agree with local", so the local path was measured on its
own first rather than assumed correct: local rejects ALL of `I li = { a = 1 }`, `I li { a = 1 }`,
`I li = { 1 }`, `int x = {5}`, `int x {5}`, `char* p = {1}`, `function<int(int)> f = {add}` and
`simd<int,4> v = {1,2,3,4}` with one message
(`brace initializer with values is not supported on 'X' - 'T' is not a struct/union/class or a
recognized container; assign it after declaration instead`). The oracle has no hole on this
axis - unlike `fix/global-positional`, where copying the local gating condition carried the
bare-brace hole across with it. That earlier lesson is why the bare spelling is its own matrix
row and its own test leg here.

#### Fix shape

`MainListener::LogNonAggregateBraceInitReject(ctx, name, typeName)` - a new helper next to
`LogPointerBraceInitReject` in `MainListener_Expressions.cpp`, holding the one message text. The
LOCAL site (`MainListener_Declarations.cpp`, the `!localIsContainer && StructType == nullptr` arm)
now calls it instead of formatting inline, so the two scopes cannot drift. The GLOBAL guard hoists
`isContainerType` and the `StructType != nullptr` test out of the `if`, keeps the three existing
messages unchanged in the aggregate arm, and adds one `else if (!scalarElements.empty() &&
!isContainerType)` arm that calls the same helper. Both global brace spellings reach it: the
`scalarInitList` it reads was already `initializer->initializerList()` OR `barebraceList`.

Deliberately NOT widened: an empty `{}` (no `fieldInit` elements - the grammar yields no
`initializerList` at all, so both scopes accept it and `I gi = { };` is unchanged); containers,
which keep their own global-scope message; and a container name whose `StructType` is somehow
null, which still falls through silently as before (accept-on-doubt - the `!isContainerType` in
the new arm). Fixed arrays and array views cannot reach the arm at all: both `continue` out
several hundred lines earlier (`MainListener_Declarations.cpp` ~2758 / ~2773), which is why
`int[3] ga = {7,8,9};` is untouched and still exits 9.

#### Coverage matrix - 49 cells, every one run on `0a45763` and on this branch

23 cells CHANGE (silent discard -> hard error; the last five rows were measured by review
round 1, same PRE/POST method). Each "value" below is the probe's exit code, and
each probe distinguishes discarded from applied (`gi == nullptr ? 5 : 6`, or the value itself):

| Cell | PRE `0a45763` | POST |
|---|---|---|
| `I gi = { a = 1 };` (the filed repro) | compiles, exit 5 (gi null) | REJECT |
| `I gi { a = 1 };` (bare-brace spelling) | compiles, exit 5 | REJECT |
| `I gi = { 1 };` (positional) | compiles, exit 5 | REJECT |
| `I gi { 1 };` (bare positional) | compiles, exit 5 | REJECT |
| `namespace N { I gi = { a = 1 }; }` | compiles, exit 5 | REJECT (`'N.gi'`) |
| `namespace N { I gi { a = 1 }; }` | compiles, exit 5 | REJECT (`'N.gi'`) |
| `const I gi = { a = 1 };` | compiles, exit 5 | REJECT |
| `static I gi = { a = 1 };` | compiles, exit 5 | REJECT |
| `unique I gi = { a = 1 };` | compiles, exit 5 | REJECT |
| `IB<int> gb = { b = 1 };` (generic interface) | compiles, exit 5 | REJECT (`'IB__i32'`) |
| `int gx = {5};` | compiles, exit 0 (should be 5) | REJECT |
| `int gx {5};` | compiles, exit 0 | REJECT |
| `bool gb = {true};` | compiles, exit 0 | REJECT |
| `float gf = {2.0};` | compiles, exit 0 | REJECT |
| `thread_local int gx = {5};` | compiles, exit 0 | REJECT |
| `char* gp = {1};` | compiles, exit 5 (null) | REJECT (`'char'`) |
| `function<int(int)> gf = {add};` | compiles, exit 5 (null) | REJECT (`'__c_fn_ptr'`) |
| `simd<int,4> gv = {1,2,3,4};` | compiles, exit 0 (lane 2 zero) | REJECT (message names the ELEMENT type `'int'` - pre-existing local wording, kept for scope consistency; `DescribePointerDeclType` could spell the vector out, but that edit changes two local-scope messages and belongs to its own round) |
| `using MyI = I; MyI gi = { a = 1 };` (alias) | compiles, exit 5 | REJECT (names the underlying `'I'`, not `'MyI'`) |
| `int ga = 5, gb = { 6 };` (multi-declarator) | compiles, exit 5 (gb dropped) | REJECT (`'gb'`) |
| `extern int gx = { 5 };` (initializer defeats extern-only) | compiles, exit 0 | REJECT |
| `if const (__MACOS__) { int gx = { 5 }; }` (file-scope const block) | compiles, discard | REJECT |
| `namespace N { namespace M { I gi = { a = 1 }; } }` | compiles, exit 5 | REJECT (`'N.M.gi'`) |

**Frozen ACCEPT SET - enumerated and measured BEFORE the guard was written, all byte-identical
PRE and POST** (17 cells): `I gi = default;` (5), `I gi = { };` (5), `I gi { };` (5),
`I gi = nullptr;` (5), `S gs = default; I gi = gs;` (6 - global boxing from another global still
WORKS), `extern I gi;` (3), `I[2] gi = { };` (5), local boxing `S s; s.a=7; I li = s; li.foo()`
(7), `int[3] ga = {7,8,9};` (9), `int[3] ga {7,8,9};` (9), `int gx = 5;` (5),
`int gx = default;` (4), `simd<int,4> gv = default;` (3), `string gs = "hi";` (2), local
`P p = { x = 3, y = 4 };` (7), local `S s = { a = 9 };` (9), local `list<int> l = {1,2,3};` (2).

**Pre-existing rejections that keep their OWN message** (9 cells, verbatim-identical both
binaries): global `P gp = { x = 1 };` and `C gc = { a = 1 };` (field-initializers-at-global-scope),
global `P gp = { 1 };` (positional-for-struct-type), global `P* gp = { x = 1 };` (pointer to
struct type), global `list<int> gl = {1,2,3};` (container), global `int[] gv = {1,2,3};`
(array-view-at-global-scope), global `P[2] gp = { x = 3 };` (global array initializer must be
positional), local `I li = { a = 1 };` and local `int x = {5};` (the oracle).

Grammar-impossible cells, named and not probed: a `struct`/`union` cannot implement an interface
(`CFlat.g4` gives the base-clause to `classDefinition` only), and `I*` is rejected earlier
(`pointer '*' is not allowed on interface type 'I'`).

#### No working program breaks

Every cell the guard newly rejects compiled PRE with its brace values ALREADY DISCARDED - the
table above measures each one and none carried a value through. There is no program that was
getting the values and now errors. Two whole-corpus differential sweeps back this: 447
`Test/**` + `example/**` files compiled with both binaries produced exactly ONE difference (the
intended new test legs), and the 88 `core/**.cb` files - each compiled as a root against its own
binary's core tree, so the comparison is like-for-like - produced ZERO.

(Method note for the next round: a first attempt swept core with a COPIED PRE binary whose
`.cflat` cache still pointed at the other tree, and produced 25 bogus "diffs" -
circular-import vs. redeclaration messages that had nothing to do with the change. A copied exe
is not a PRE binary; build a detached worktree at the merge base, which is what the numbers
above come from.)

#### Per-site audit - every site asking the same question

`grep '\.StructType == nullptr'` finds six `== nullptr` sites; adding the `!= nullptr` site
fixed here (`scalarIsAggregate`, `MainListener_Declarations.cpp:3463`) makes seven in total. The
grep does not reach `EmitGlobalFixedArrayInit`, the GLOBAL sibling of the 2699/2719 element
sites - it is what actually keeps `int[3] ga = {7,8,9};` working, and `I[2] gi = {gs1,gs1};`
gives the same "must be compile-time constants" message on both binaries (review-verified).
Of the seven, two are scalar-declarator brace-init routing:

- `MainListener_Declarations.cpp:3888` (LOCAL scalar declarator) - same question, already
  correct, now routed through the shared helper. This is the oracle.
- `MainListener_Declarations.cpp:3463` (GLOBAL scalar declarator) - the defect; fixed.
The other five:

- `MainListener_Declarations.cpp:2699` / `:2719` (fixed-array ELEMENT brace init) - same
  question, no defect: 2699 zero-inits an empty `{}` on a non-struct element (correct), and
  2719 already rejects a non-empty list on a non-struct element type
  (`array value-initializer '= {}' requires a struct element type`).
- `MainListener_Expressions.cpp:4921` (winmd generic instantiation on demand in a cast),
  `:6171` (`EmitFixedArrayDefaultInit` early-out), `MainListener_PostfixExpression.cpp:200`
  (`operator->` walk) - not brace-init routing, unaffected.

The FIELD-default path (`ParseFieldDefaultBraceInitializer`) asks the same question with a
silent `return nullptr` and IS defective - see "Found, not fixed" below.

#### Test legs

`Test/errors/err_primitive_brace_init_with_values.cb` - 9 new `expect_error` blocks (8 at file
scope for the globals, 1 local). Each of the 8 global legs was proven individually: put alone in
a file it exits 1 on the `0a45763` binary (`FAIL: expected error ... did not occur`) and 0 on
this branch. They pin the interface repro, the bare-brace spelling, the positional spelling, the
generic instantiation `IB__i32`, the namespace-qualified name `N.gi`, both primitive spellings,
and `char*`. The 9th (local `I li = { a = 1 };`) passes on BOTH binaries by design and is
labelled as the oracle leg: it is the tripwire for the shared helper, so a wording drift fails
both scopes' legs together. The file's header comment, which previously recorded the global gap
as "pre-existing... unaffected by this fix", is rewritten with the measured PRE values.

No new value legs: every cell this change touches goes from compiling to erroring, so a runtime
leg could only assert the accept set, and each accept-set cell is byte-identical on both
binaries - such a leg would pass with the fix reverted, which is the leg-that-cannot-fail
pattern this file has banned. The accept set is frozen in the table above instead.

#### Verification

- `./test.sh Release`: 600 passed / 0 failed / 8 skipped (baseline 600/0/8 - the suite counts
  FILES and the legs went into an existing one).
- `bash example_mac.sh Release`: 35 passed / 0 failed.
- Differential sweeps: 447 `Test`+`example` files, 1 intended difference; 88 `core` files, 0.
- No new `TypeAndValue` / `StructData` / `AnnotationValue` field, so no `--init` cache
  round-trip change is owed.

#### Found, not fixed - filed

The FIELD position has the identical hole and is NOT closed here: `ParseFieldDefaultBraceInitializer`
bails with `if (fieldType == nullptr) return nullptr;`, so `struct H { I h = { a = 1 }; };` leaves
`h.h` null and `struct H2 { int x = { 5 }; };` leaves `x` zero - measured identical on `0a45763`
and on this branch (exit 5 / exit 0 both sides). Appended as a new neighbouring-cell section to
[[fixed-array-field-brace-default-discarded]] (P2), which already owns the field-default family,
rather than opening a fourth file for one emitter. Its fix is the same helper.

### fix/fat-widen - the FIELD-default site now widens a legal fat closure source, LANDED

Fixed and deleted [[fat-field-default-legal-source-not-widened]]. `fix/fat-default` deliberately
left the FIELD-default site's legal accept set broken (named fn / thin `function<>` value into a
`Lambda<>` field default compiled clean and SIGSEGVed, exit 139) because implementing the widen
without measuring its blast radius would have been exactly the kind of unmeasured fix the skill
forbids - so it was filed instead. This round does the widen.

Fix: `MainListener::ParseFieldDefaultInitializer` (`MainListener_Expressions.cpp` ~5845-5858) -
the fat `else` branch (sibling of the existing thin `CheckThinFnPtrAssignProvenance` branch) now
reads:

```cpp
std::string destDesc = std::format("'{}.{}'", structName, field.VariableName);
compiler->CheckFatClosureAssignProvenance(val, nv, destDesc);
val = compiler->WidenBareOrThinToClosureFat(val);
```

`CheckFatClosureAssignProvenance` is unchanged and still runs FIRST: it `LogError`s (which
throws / exits, per the "LogError is a control-flow edge" lesson) on a provable data pointer, so
`WidenBareOrThinToClosureFat` never executes for a rejected source - the reject and the widen
share the ordering the call-site path (`WidenToClosureFatChecked`) already encodes, just spelled
as two calls instead of one so the existing reject wording (and its `err_data_pointer_to_closure_param.cb`
legs) stays byte-identical. `WidenBareOrThinToClosureFat` is the same helper the call-site argument
path already uses: a named function becomes `{shim, null}`, a thin `function<>` value becomes
`{code, null}`, and an already-fat value or non-pointer passes through untouched - so a
member-access source that reads an already-fat field needs no wrapping and is a no-op here, matched
by measurement (see below). The returned value is what every caller already stores into the field
(`MainListener_Aggregates.cpp` field-default and brace/positional-aggregate emitters), so no caller
changed.

Accept-set matrix, measured PRE (`a7bdc31`, throwaway worktree, removed after use) vs POST:
- named-fn struct field default: PRE compiles clean, SIGSEGV (exit 139, no output before flush);
  POST compiles clean, returns 31 for `s.f(30)`.
- thin `function<>` VALUE struct field default: PRE SIGSEGV, POST returns 31. Confirms the bug was
  not named-fn-specific, per the issue file.
- `Lambda<>` VALUE struct field default (regression guard): PRE and POST both return 31,
  byte-identical - already fat, `WidenBareOrThinToClosureFat` is a no-op on it.
- `nullptr` field default (regression guard): PRE and POST both compile and read back null.
- named-fn CLASS field default (separate default-ctor emitter): PRE SIGSEGV, POST returns 31.
- named-fn NESTED struct field default (`Outer.inn.f`): PRE SIGSEGV, POST returns 31.
- named-fn field default via `S s;` (no `= default`) vs `S s = default;`: both spellings PRE
  SIGSEGV, POST returns 31 - identical to the `= default` spelling.
- named-fn field default on a HEAP instance (`new S()`): PRE SIGSEGV, POST returns 31.
- thin `function<>` FIELD default with a named-fn source (already thin, no widen needed): PRE and
  POST both return 31, byte-identical - unaffected, confirms the thin sibling branch was never
  broken.
- explicit `(function<...>)value` cast escape hatch as a field default source: PRE and POST both
  compile clean; the reject exemption (`IsDataValueCodeCast`) is unchanged, but the emitted IR is
  NOT identical - PRE silently DROPPED the asserted address (`ret %D zeroinitializer`), POST emits
  the real `insertvalue` pair, so the escape hatch now does what its diagnostic advertises
  (review round 1 measured the IR delta; a strict improvement, recorded per the equivalence rule).
- `?:` join of two `Lambda<>` VALUES as a field default source: PRE and POST both compile and
  return the correct arm's value (31) - already fat on both arms, so nothing to widen. The
  NAMED-FUNCTION-arm join (`k > 0 ? addOne : addTwo` as the default) was 139 PRE and 31 POST -
  a cell this fix genuinely repairs (review round 1 measured it), not a never-broken one.
- generic-encoded fat field default fed a bare named function (`struct G<T> { T item = fwAddOne;
  }; G<Lambda<int(int)>> g = default;`) - the axis the issue file flagged as "if expressible": PRE
  SIGSEGV, POST returns 31. `GetEncodedClosureType` routes the encoded element through the same
  branch, so the generic axis needed no separate code path.
- member-access source (`gh.g` reading an already-initialized `Lambda<>` field) as a field default:
  PRE and POST both compile and return the correct value (31) - `val` arrives already struct-typed
  (fat), so the `!val->getType()->isStructTy()` guard on the widen branch never fires; this axis
  was never in the bug's blast radius. (A GLOBAL-scope struct instance's OWN field defaults do not
  run at all - `S g = default;` zero-initializes by design, per the compiler's own diagnostic on a
  global brace-init attempt - so this leg reads the source through a struct whose field was set by
  an explicit assignment before the read, not through a global's own unrun field-default chain;
  that zero-init-only behavior is pre-existing, applies identically to a plain `int` field, and is
  unrelated to and unchanged by this fix. Review round 1 found the FIXED-ARRAY spelling shares it:
  `S[2] a = default;` also skips every field initializer - silent zeros for an `int` field, and a
  SIGSEGV when the skipped field is a closure (139 PRE and POST, unchanged) - while `new S[2]`
  DOES run them and IS fixed by this commit, so the two array spellings diverge; filed as
  `internal/issue/p2/fixed-array-default-skips-field-initializers.md`.)
- capturing-lambda-literal FIELD default: PRE and POST both fail identically with "Module
  verification failed: Found return instr that returns non-void in Function of void return type" -
  unchanged, per [[lambda-literal-param-default-invalid-ir]] (still open, out of scope here, as the
  issue file required).
- data-pointer (`void*`) field default (provenance reject, must keep rejecting): PRE and POST both
  reject with the byte-identical `CheckFatClosureAssignProvenance` wording ("cannot assign a
  'void*' value to closure destination ..."), confirmed by re-running
  `Test/errors/err_data_pointer_to_closure_param.cb` (all existing `expect_error` blocks, including
  its two default-value legs from `fix/fat-default`, still PASS on POST).
- union closure member field default (out of scope, [[union-closure-member-call-crashes-compiler]]):
  PRE and POST both SIGSEGV the COMPILER identically (exit 139, no diagnostic) on the fat repro from
  that issue file - untouched, as that file requires.
- PARAMETER-default site (both source kinds, must stay unchanged - this fix does not touch
  `GenerateDefaultParamOverloads`): PRE and POST both compile and return the correct value (31),
  byte-identical.

Test legs: `Test/test_function_ptr.cb` gained `testFatFieldDefaultWidensLegalSource` (named-fn
struct field default, thin-value field default, `Lambda<>`-value regression guard, `nullptr`
regression guard, named-fn class field default, named-fn generic-encoded field default - 6
`Test()` value assertions), registered in `main`. Proven to fail on the PRE binary (`a7bdc31`,
throwaway worktree, removed after use): compiling `Test/test_function_ptr.cb` with the new legs
against PRE compiles clean and the whole suite binary SIGSEGVs (exit 139) at the first new leg,
before any output flushes (stdout is fully buffered when not a tty) - matching the isolated
single-leg repros above. Passes 73/73 on POST.

`./cmake_build.sh release && bash test.sh Release` - 600 passed, 0 failed, 8 skipped.
`bash example_mac.sh Release` - 35 passed, 0 failed. One commit, `git rev-list --count
a7bdc31..HEAD` = 1.

## Landed: `fix/extern-array` (2026-08-06) - a bodyless PROTOTYPE no longer drops a fixed-array return size

Closes [[extern-decl-drops-fixed-array-return-size]] (P1, silent wrong ABI), the last spelling
of the by-value fixed-array-return axis whose DEFINITION half landed with `fix/array-storage`.

### The filed repro, re-measured

`extern char[8] extmk();` + a `main` that never calls it compiles rc 0 on `6a13b1a`, exactly as
filed. The severity claim was verified rather than assumed, from the `--out-lli` IR of a CALLING
variant:

```
  %0 = call i8 @extmk()
  declare i8 @extmk()
```

So the ABI is genuinely wrong (one byte where the callee writes eight), not merely under-typed.
The no-body case alone is harmless in practice - nothing defines `extmk`, so the CALLING probe
dies at link with `ld64.lld: error: undefined symbol: _extmk` (rc 1). The real hazard is exactly
the one the issue names: a `.c`/asm object elsewhere that DOES define an 8-byte-returning
`extmk`, which links fine and reads one byte. The `declare i8` above is the proof; the link
failure is an artifact of the probe having no definition, not a mitigation.

### Root cause - a SECOND registration site, not a missed branch

`CFlat.g4`'s `functionDefinition` ALWAYS requires a `compoundStatement` - there is no
optional-body alternative. A bodyless prototype is therefore not a `functionDefinition` at all:
it parses as a plain `declaration` whose `initDeclarator` has a function-shaped
`directDeclarator`. It never enters `ParseFunctionDefinition`, so the existing reject at
`MainListener_Declarations.cpp:1897` structurally cannot see it. Prototypes are registered at
one other place entirely: `MainListener::ParseDeclaration(DeclarationSpecifiersContext*,
InitDeclaratorListContext*, ...)`, in the `paramTypeList != nullptr || hasParens` arm.
`CreateFunctionDeclaration` reads only `TypeName` / `Pointer` to pick the LLVM return type and
never looks at `ArraySize` / `AliasArraySize`, so the size is dropped there silently.

**Only ONE pass registers prototypes.** `ForwardRefScanner::ScanExternalDeclaration`
(`ForwardRefScanner.cpp:1684`) dispatches on `annotationDefinition` / `namespaceDefinition` /
`functionDefinition` / `structDefinition` / `classDefinition` / `interfaceDefinition` /
`usingDeclaration` / `programDefinition` / `importDeclaration` / `lockFieldGroup` /
`expectErrorDeclaration` / `ifConstDeclaration` - there is no `declaration()` arm. So the
pre-pass never sees a prototype and the "both copies must move together" pattern does not apply
here: a single gate at the single registration site is correct, and a second guard in
`ForwardRefScanner` would be dead code.

### What changed

`cflat/MainListener_Declarations.cpp:2499` (one new guard, immediately before the
`CreateFunctionDeclaration` at line 2515). Predicate and message text are copied
verbatim from the definition-path reject at `:1897` so the two sites cannot drift:

```cpp
if ((typeAndValue.ArraySize != nullptr || typeAndValue.AliasArraySize > 0)
    && !typeAndValue.IsArrayView && !typeAndValue.Pointer)
```

anchored on `direct` (the declarator) so the caret lands on the function name. `IsSimd` is used
only to spell the element back out as `simd<T,N>`, never as a carve-out - the same shape as
`:1897`, and the reason cell 7 below is rejected.

### Phase A coverage matrix - every cell run on `6a13b1a` and on this branch

| # | Cell | PRE `6a13b1a` | POST |
|---|---|---|---|
| 1 | `extern char[8] extmk();` (filed repro, uncalled) | compiles rc 0, links, runs rc 0 | REJECT `'extmk' ... 'char[N]'` |
| 2 | same + a CALL using the result | `call i8 @extmk()` emitted; rc 1 at link (`undefined symbol: _extmk`) | REJECT at compile, before link |
| 3 | `using RetBuf = char[8]; extern RetBuf extmk2();` (alias -> `AliasArraySize`) | compiles rc 0 | REJECT `'char[N]'` |
| 4 | `extern int[2][3] extmk3();` (2-D) | compiles rc 0 | REJECT `'int[N]'` |
| 5 | `extern char[] extmkview();` (array VIEW) | compiles rc 0 | rc 0 - unchanged (ACCEPT SET) |
| 6 | `extern simd<float,4> extmkvec();` (bare simd) | compiles rc 0 | rc 0 - unchanged (ACCEPT SET) |
| 7 | `extern simd<float,4>[2] extmkvecarr();` (simd ARRAY) | compiles rc 0 | REJECT `'simd<float,4>[N]'` |
| 8 | `extern char* extmkptr();` (pointer) | compiles rc 0 | rc 0 - unchanged (ACCEPT SET) |
| 9 | `struct Buf { char[8] b = default; }; extern Buf extmkbuf();` | compiles rc 0 | rc 0 - unchanged (ACCEPT SET) |
| 10 | the same prototype inside an IMPORTED `.cb`, not the root file | compiles rc 0 | REJECT, located in the IMPORTED file |
| 11 | generic prototype `extern T[8] extmkgen<T>();` | not expressible - `mismatched input ';' expecting {'lock','where','{'}`; the grammar requires a body on a generic | out of scope, grammar-impossible |
| 12 | C-interop auto-extern (`import "ea_12_util.c";`, three ordinary C fns incl. `void ea_fill(char out[8])`) | compiles rc 0, runs `5 hi abcdefg` | identical - rc 0, `5 hi abcdefg` (ACCEPT SET) |
| 13 | `namespace Nsx { extern char[8] extmkns(); }` | compiles rc 0 | REJECT, names the QUALIFIED `'Nsx.extmkns'` |
| 14 | prototype declared inside a FUNCTION BODY (statement scope) | compiles rc 0 | REJECT |
| 15 | `char[8] protomk();` - prototype with NO `extern` keyword | compiles rc 0 | REJECT |

Cells 13/14/15 are the neighbour axis and are why the guard is NOT keyed on
`typeAndValue.external`: the namespace-scope, statement-scope and plain (non-`extern`) prototype
spellings all reach the same `ParseDeclaration` arm and all dropped the size identically.

**Frozen ACCEPT SET** - cells 5, 6, 8, 9, 12, enumerated and measured BEFORE the guard was
written, all rc 0 both binaries, and cell 12 additionally value-identical at runtime
(`5 hi abcdefg`).

### The C-interop caveat - CONFIRMED structurally unreachable

The issue file's mandatory pre-check. Two independent reasons, both read from the source rather
than inferred from a passing probe:

1. **The auto-extern path does not go through the gated code at all.** `--c-include` /
   `import "x.c"` / `import package "x.h"` all land in `LLVMBackend::RegisterCSignatures`, which
   calls `CreateFunctionDeclaration(regName, e.ret, ...)` directly at
   `LLVMBackend_CInterop.cpp:783`. The new guard is in `MainListener::ParseDeclaration`, which
   no C-interop registration ever enters.
2. **`e.ret` cannot carry array-ness even in principle.** It is built solely by
   `MapCTypeToTypeAndValue` -> `MapCTypeToTypeAndValueImpl`
   (`LLVMBackend_CInterop.cpp:617-751`), which writes exactly `TypeName`, `Pointer`,
   `ElemPointer` (plus the `IsFunctionPointer` / `FuncPtrReturn*` set on the `(*)`  branch at
   `:471-560`). It never touches `ArraySize` or `AliasArraySize` - and it could not:
   `ArraySize` is an ANTLR parse-tree pointer with no string source available on this path, and
   the impl's first act on seeing a `[` is to DECAY it (`:633-637`, `ptr++; ctype = ctype.substr(0, br);`).
   `grep -n 'ArraySize' LLVMBackend_CInterop.cpp` returns three hits, all `ConstArraySize` on
   struct FIELDS (`:1578`, `:1658`, `:1659`), none on a return type. C's own declarator grammar
   cannot express an array return either, so clang's JSON AST has nothing to hand over.

Empirically confirmed too (cell 12): a `.c` with `int ea_add(int,int)`, `const char* ea_greet(void)`
and `void ea_fill(char out[8])` compiles and runs byte-identically on both binaries. The array
PARAMETER in `ea_fill` is the closest real C gets to the shape, and it decays to `char*`.

### Per-site audit - every read of `ArraySize` / `AliasArraySize` for a function RETURN

`grep -n 'AliasArraySize' cflat/*.cpp cflat/*.h` gives 10 sites; the `ArraySize` grep adds the
declarator/global paths. Per site:

- `ForwardRefScanner.cpp:245` and `MainListener_Declarations.cpp:662` - the two
  `ParseDeclarationSpecifiers` copies SETTING `AliasArraySize` from an array alias. Already
  symmetric, unchanged; the fix is a registration-time value check, not a type-parsing change,
  so the two-copy rule is not engaged.
- `MainListener_Declarations.cpp:741` - `AliasArraySize > 0 && hasExplicitPointer` diagnostic on
  a VARIABLE declarator. Different question (alias + `*`), no defect.
- `MainListener_Declarations.cpp:1897` - the DEFINITION-path reject. The oracle; unchanged.
- `MainListener_Declarations.cpp:2499` - the new PROTOTYPE reject. The defect; fixed.
- `MainListener_Declarations.cpp:2163/2166` and `:2592/2596` - folding `AliasArraySize` into
  `ConstArraySize` for a VARIABLE declarator (local and global). Legal and load-bearing
  (`RetBuf b = default;` must keep working, and does - it is exercised by leg `mk3`). No change.
- `ForwardRefScanner.cpp:436` - the definition-path `CreateFunctionDeclaration`. Reached only
  for real `functionDefinition` nodes, all of which hit `:1897` in the main walk. No second
  guard needed; a prototype never reaches this line (verified: `ScanExternalDeclaration` has no
  `declaration()` arm).
- `MainListener_Generics.cpp:481/513`, `MainListener_Aggregates.cpp:420/2756`,
  `MainListener.h:1537/1573/1589`, `ForwardRefScanner.cpp:1252-1662`,
  `MainListener_PostfixExpression.cpp:5061` - other `CreateFunctionDeclaration` callers
  (ctor/dtor wrappers, `program` run wrappers, generic INSTANTIATION, on-demand generic decls).
  None is a user-written prototype path; a size there would be a compiler invariant bug, not a
  user error, which is why the gate was NOT put inside `CreateFunctionDeclaration` itself - that
  layer cannot tell a user prototype from an internal wrapper registration.

### Test legs

`Test/errors/err_fixed_array_byval_return.cb`, +2 `expect_error` blocks (existing file, no new
test file). Both proven fail-on-PRE against a detached worktree built at `6a13b1a`, isolated one
per file:

- `extern char[8] extmk1();` - PRE: `FAIL: expected error 'function 'extmk1' cannot return the
  fixed array 'char[N]' by value' did not occur`, rc 1. POST: `PASS: expected error received`.
  Discriminator: reaches the `ParseDeclaration` site through `ArraySize` (the prototype twin of
  leg `mk1`).
- `extern RetBuf extmk2();` (reusing the file's existing `using RetBuf = char[8];`) - PRE: same
  `FAIL ... did not occur`, rc 1. POST: PASS. Discriminator: reaches the SAME site through
  `AliasArraySize` instead - the `mk3` x `extmk1` cross-product.

The file's header comment previously said the reject was "at the definition"; it is rewritten to
name both registration sites and to say why a bodyless prototype is not a `functionDefinition`.

No new value legs. Every cell the guard touches goes from compiling to erroring, so a runtime leg
could only assert the accept set - and each accept-set cell is byte-identical on both binaries, so
such a leg would pass with the fix reverted (the leg-that-cannot-fail pattern this file bans). The
accept set is frozen in the table above instead. The guard adds no new carve-out logic: predicate
and message are verbatim from `:1897`, already covered by legs `mk1`-`mk4` and `mkVec`.

### Verification

- `./cmake_build.sh release`: clean.
- `bash test.sh Release`: 600 passed / 0 failed / 8 skipped (baseline 600/0/8 - the suite counts
  FILES and both legs went into an existing one).
- `bash example_mac.sh Release`: 35 passed / 0 failed.
- No whole-corpus differential sweep was run: the change adds a rejection reachable only from a
  bodyless function-shaped declarator, and `test.sh` (600 files) plus `example_mac.sh` (35, which
  is where the C-interop and header-binding spellings actually live) both compile green, so no
  corpus file performs the newly-rejected crossing. The strong evidence here is the targeted
  PRE/POST matrix above, not a sweep.
- No new `TypeAndValue` / `StructData` / `AnnotationValue` field, so no `--init` cache round-trip
  change is owed.

### Found, not fixed

Cell 11 (a generic prototype) is grammar-impossible rather than a gap, and cells 13/14/15 - which
WOULD have been residue if the guard had been keyed on `external` - are closed here by the same
predicate.

Review round 1 found a THIRD, still-open registration path: an interface method contract
(`interface IBuf { char[8] get(); }`) and a struct-member method prototype
(`struct S { char[8] get(); }`) both still accept a fixed-array return silently. Neither is a
wrong-ABI hazard like the closed issue - any implementor of `IBuf.get()` is already rejected by
the definition-path guard, so the interface can never be implemented or called through, and a
bodyless struct-member prototype registers no callable symbol. Filed as P3 (diagnostic quality,
not correctness) in
[[interface-and-struct-member-fixed-array-return-not-rejected]] rather than folded into this
commit, since it is a different registration path (interface/struct-member contract, in
`MainListener_Aggregates.cpp`) that this fix's scope did not cover.
---

### fix/brace-literal - an EMPTY brace pair is not an interpolation, and a `string` reaching a `char*` parameter is diagnosed (LANDED)

Closes `string-literal-containing-braces-retyped-as-string` (filed P2 for its false-rejection face,
re-ranked P1 for the miscompile face); its file is deleted in this commit. Branched from `56ebc52`.

**Root cause, measured (the filed file said "not diagnosed").** `HasInterpolation`
(`cflat/MainListener_PostfixExpression.cpp`) returned true on the first unescaped `{`, so ANY brace
pair sent the literal down `ParseFormatString`, which returns a `string`. `ParseFormatString` itself
already disagreed: it treats an empty `{}`, an unmatched `{`, and JSON-ish content as LITERAL TEXT
and emits them verbatim. So for `"a = {} b"` the format path produced one literal segment and
wrapped it in a `string` - the retype had no interpolation behind it at all. Two predicates asking
the same question, one of them wrong; the fix makes them one function (`ClassifyBrace`) that both
call.

**Face 2 was NOT where the issue file said it was.** The file blamed the variadic guard
(`cannot pass 'string' to the variadic '...'`). Measured: for `printf("{x}\n")` the interpolated
string is argument 0, which binds printf's DECLARED `char* fmt` parameter and is therefore not in
the variadic range - the variadic guard never sees it. The pointer-parameter arm then materialized
the `{ptr,len}` struct on the stack and passed its ADDRESS, which is the binary garbage. A variadic
candidate is taken without per-argument scoring (`LLVMBackend_Overloads.cpp:72-81`), so the overload
scoring that rejects `f(char*)` for a user function never runs for `printf`. The guard was added at
the pointer-parameter arm, keyed on the REPRESENTATION (the named `string` struct type), not on
`TypeAndValue.TypeName` - an interpolated literal carries no `string` spelling. The variadic-range
guard was re-keyed the same way in the same change (it was `TypeName == "string" && isStructTy`).

**Design decision: only the EMPTY pair and the unmatched `{` stop triggering interpolation.** The
first cut also moved JSON-ish content (matched braces starting with `"` or `\`) onto the plain
literal path and BROKE `Test/test_reflect.cb` (`toJson_nested FAILED (expected ...12345} got
...12345}}`)`. The two paths do not fold braces the same way - but NOT in the way this record first
claimed: `ParseFormatString` ALSO folds `}}` to `}` at top level (review round 1 measured it), so
neither path is byte-for-byte. The real divergence is at the BOUNDARY: the format path consumed an
empty pair's `}` as part of the pair, leaving a following `}` standing alone, while the plain path
pairs that `}` with the next one and folds. So `string s = "a {}} b"` read `a {}} b` PRE and reads
`a {} b` POST - a narrow, deliberate value change (empty pair followed by an odd run of `}`),
pinned by the `interp_empty_pair_brace_run` leg and consistent with the documented `}}` escape
rule. Every other neighbouring spelling (`"{}}}"`, `"{}{}"`, `"a {} }} b"`, `"{{}"`, ...) was
measured identical PRE/POST. JSON-ish matched-pair content is different: its interior region IS
copied differently enough that moving it broke `test_reflect.cb`, so it keeps its routing. JSON-ish content therefore keeps its pre-fix routing
(`BraceKind::Verbatim`), and the remaining false rejection is filed as
[[json-ish-brace-literal-still-typed-string]]. Whitespace-only `{ }` is deliberately unchanged too:
it already produced the located "the text between the braces (\" \") is not a single valid
expression" diagnostic, which cannot be distinguished from a typo of `{ x }` and is more useful
than silently accepting it.

No escape syntax was invented; `{{` / `}}` / `\{` are unchanged.

**Measured matrix** (PRE = `56ebc52` Release in a throwaway worktree, removed after use; POST = this
commit; every cell compiled with `-o` and RUN):

| Cell | PRE | POST |
|---|---|---|
| `printf("a = {} b\n")` | rc 0, runs, prints address bytes (the filed repro A) | rc 0, prints `a = {} b` |
| `printf("a {} b\n")` / `printf("{}\n")` | rc 0, garbage | rc 0, prints the literal |
| `Test("a = {} b", 1, 1)` (label with `{}`) | `no overload of 'Test' matches` (repro B) | rc 0, binds `char* name` |
| `char* p = "a {} b";` | `cannot initialize pointer 'p' with a value of type 'char'` | rc 0, prints `a {} b` |
| `char* u = "unmatched { brace";` | same false rejection | rc 0 |
| `printf("unmatched { brace\n")` | rc 0, garbage | rc 0, prints the text |
| `string s = "a {} b";` | prints `a {} b` | prints `a {} b` (unchanged) |
| `"a {} b {x}"` (empty pair inside a REAL interpolation) | `a {} b 7` | `a {} b 7` (unchanged) |
| `"{x}"`, `"n={n}"`, `"{a} and {b}"`, IString `{p}` | correct values | byte-identical |
| `"{{escaped}}"` | `{escaped}` | `{escaped}` (unchanged) |
| `"{ }"` (whitespace only) | located "not a single valid expression" | identical |
| `"{x}"` with no `x` in scope | `Undefined variable x.` | identical |
| `string s = "{\"key\": 1}"` (JSON) | prints `{"key": 1}` | identical |
| `char* j = "{\"k\":1}";` | false rejection | STILL rejected - [[json-ish-brace-literal-still-typed-string]] |
| `printf("{x}\n")` (real interpolation into printf) | rc 0, garbage, NO diagnostic | rc 1, `cannot pass 'string' to the 'char*' parameter 'fmt' of 'printf'` |
| `printf("{\"k\":1}\n")` | rc 0, garbage | rc 1, same diagnostic |
| `string t = "zz"; printf(t);` | rc 0, garbage, no diagnostic | rc 1, same diagnostic |
| `string t = "zz"; printf("%s\n", t);` | variadic guard fires | fires, wording unchanged |
| `printf("%s\n", s.data())` | prints `hi` | unchanged |
| `int f(char* p); f(s)` (non-variadic) | `no overload of 'f' matches` | unchanged (scoring, not the new guard) |
| `function<int(char*)> fp; fp(s)` | `Module verification failed:` | identical - filed as [[indirect-call-string-to-charptr-fails-in-verifier]] |

**Accept set frozen before the guard was written**: every real-interpolation spelling in
`Test/test_core.cb::testStringInterpolation` (simple, two-var, sandwich, int, IString, mixed, i64,
double, bool), `s.data()` into printf, string-typed method receivers, and `test_reflect`'s
`toJson_nested` verbatim-JSON literal. All measured on PRE and re-measured on POST; all identical.

**Per-site audit of the changed predicate** (a `string` value reaching a pointer parameter):
- `LLVMBackend_Overloads.cpp` pointer-parameter arm - the defect; guard added.
- `LLVMBackend_Overloads.cpp` variadic-range arm - had the guard, but spelling-keyed; re-keyed on
  the struct type. Wording unchanged, so `err_string_vararg.cb`'s existing leg still pins it.
- `LLVMBackend_WinRT.cpp` interface-dispatch arm (`param.Pointer`) - same shape, NOT changed: an
  interface method cannot be variadic (`err_iface_variadic_method.cb`), so overload scoring always
  runs and rejects `string` -> `char*` before this arm.
- `LLVMBackend_ControlFlowAndFunctions.cpp` indirect-call arms (C fn ptr, closure invoker) - same
  shape, reached today, and they fail in the verifier instead of diagnosing. Left alone and filed:
  [[indirect-call-string-to-charptr-fails-in-verifier]].
- `HasInterpolation` had exactly one caller; `ParseFormatString`'s brace scan was the only other
  copy of the predicate, and it is now the same function.

**Differential corpus sweep**: every `.cb` under `Test/` and `example/` (447 files), compiled with
`-o` and RUN on both binaries. After normalizing the two binaries' own paths, exactly two files
differ - `Test/test_core.cb` and `Test/errors/err_string_vararg.cb`, the two files this commit
edits. Everything else differs only in addresses, timings, thread counts and pids (`test_c`,
`test_hpc`, `test_time`, the four hpc examples, `macos_framework_link`, `sysinfo_mac`), or in the
PRE binary's own `runtime core '...'` path inside an unrelated `imported file not found` message
(the four Win32-only UI examples).

**Test legs.** `Test/test_core.cb::testStringInterpolation` (+3, total bumped 6 -> 9): an empty pair
bound to a `char*` AND used in the `Test` label itself (fails on PRE with
`cannot initialize pointer 'emptyPair' ...`; the label alone, isolated, fails with
`no overload of 'Test' matches` - the filed repro B), an unmatched `{` bound to a `char*` (same PRE
rejection), and an accept-set freeze leg for an empty pair inside a real interpolation (passes on
both binaries by design, and its comment says so). `Test/errors/err_string_vararg.cb` gained a
scoped-block leg for `printf("n={n}\n")`; on PRE that file prints
`FAIL: expected error ... did not occur` and exits 1. Both legs in that file were mutation-checked
individually and each flips the file to exit 1 on its own.

`./cmake_build.sh release && bash test.sh Release` - 600 passed, 0 failed, 8 skipped.
`bash example_mac.sh Release` - 35 passed, 0 failed. One commit,
`git rev-list --count 56ebc52..HEAD` = 1.

### fix/generic-shell - an unresolved generic name no longer gets an opaque shell, LANDED

Closes the P1 [[unresolved-generic-preregisters-opaque-shell]] (filed 2026-08-05, file
deleted). Branch `fix/generic-shell`, one commit on `b18ae7f`.

**Root cause, confirmed.** `ForwardRefScanner` pre-declared an opaque struct shell for every
syntactic `Name<Args>` in a BARE type position without checking that `Name` names a generic type
template. The filed citation was right about the site and right that the qualified spelling one
line below IS gated on `IsGenericTemplateKey`. It was INCOMPLETE about the copies: gating
`tryPreDeclare` alone left the headline repro - `int use(ZZZ<int> v)` with `ZZZ` declared nowhere -
still compiling, linking and running clean. A SECOND ungated copy of the same shell creation lives
in `ForwardRefScanner::ParseDeclarationSpecifiers` (`ForwardRefScanner.cpp:194`), which is what
resolves a function SIGNATURE, and it re-created the shell the first gate had just refused. The
main-pass twin of that branch was already gated (`isKnownTemplate`), so this really was the
both-copies divergence CLAUDE.md warns about - in two places, not one.

**Copies audited.** Shell-creating sites for a generic instantiation, per site:
`ScanGenericTypeUses::tryPreDeclare` (bare + qualified) - GATED here; `ForwardRefScanner::
ParseDeclarationSpecifiers` - GATED here; `MainListener::QueueGenericInstantiation` and
`MainListener::ParseDeclarationSpecifiers` - already gated, unchanged; the `using` alias path
(`ForwardRefScanner.cpp:1428`) - already rejects with its own message (`using alias 'BQ' =
'QQQ<int>': 'QQQ' is not a generic type`), measured, unchanged; the tuple sites - a different
construct whose template is always present, unchanged.

**Gate shape.** One new predicate, `LLVMBackend::AnyGenericTypeTemplateNamed`. Accept-on-doubt:
it refuses only a name with NO evidence anywhere, because the sole consequence of refusing is that
the use falls through to the existing `unknown type '...'`. It accepts the template key space
(`IsGenericTemplateKey`, before and after namespace resolution), a `using GB = Box;` base alias, an
imported winmd generic, a template seen where the scan is not `certain`, and - load-bearing - any
key whose LAST DOTTED SEGMENT matches the spelling. Generic FUNCTION templates are deliberately
NOT consulted; that is what lets a zero-argument `mk<int>()` stop being shelled.

**Trap 1 (`certain=false` regions), measured.** Interfaces are collected regardless of `certain`;
only the struct/class half is skipped, so the exposure is a generic CLASS/STRUCT template inside an
unfoldable `if const` arm or an expect_error block. Collection was widened into a new set
`gts.scannedGenericStructNamesUncertain`, read by `AnyGenericTypeTemplateNamed` and nothing else -
deliberately OUT of the key space, since an invented key is a false rejection. It is NOT optional:
built without it, `Test/errors/err_lambda_array_view.cb` and
`Test/errors/err_data_pointer_to_closure_param.cb` both go red (`unknown type
'LavOuter____fatfn_1_3_i32_3_i32'`, `unknown type 'SigBoxE__double'`) - each declares a generic
struct inside an expect_error block and then names it in a function SIGNATURE. Note the
discriminator: a plain local declaration of such a type needs no shell at all (the main pass
registers and instantiates the template itself), which is why the first three probes of this shape
passed without the set and nearly certified it as dead code. The signature is the shape that needs
it, and `Test/test_generics.cb`'s `gs_ifconst_uncertain_template_use` leg was rebuilt around a
signature for exactly that reason (verified: it reports `unknown type 'GsUncertainBox__i32'` on a
build with the set removed).

**Trap 2 (ratified messages), measured.** The three `Test/errors/err_namespaced_generic_iface_*.cb`
pin `Unknown identifier 'Width'.` / `'Tag'.`, confirmed correct by the maintainer 2026-08-05. Their
whole mechanism is the shell for a BARE `IV<int>` that names the namespaced key `NS.IV`. The
last-dotted-segment clause of the gate exists to keep them: with it, all three files are
BYTE-IDENTICAL to the pre-fix binary (diffed, rc 0 both sides); built without it, all three report
`unknown type 'IV__i32'` instead. Nothing about those tests changed.

**One existing message DID change, deliberately.**
`Test/errors/err_if_const_generic_interface_dead_branch.cb` moved from `has an incomplete layout (a
field type C interop could not import)` to `unknown type 'GiDeadOnly__i32'`. A generic interface
declared only in a DECIDABLY-DEAD `if const` arm genuinely does not exist in this build, so the new
wording is true where the old one blamed C interop on a file with no C. The test's stated intent -
"rejected at the first use, not routed to a fat pointer and run" - still holds; its expectation and
comment were updated in this commit.

**Three-face matrix, PRE (b18ae7f) -> POST.**

| Shape | PRE | POST |
|---|---|---|
| `int use(ZZZ<int> v) { return 1; }`, never called | compiles, links, RUNS clean (exit 0) | `unknown type 'ZZZ__i32'` |
| `ZZZ<int> z;` local | `type 'ZZZ__i32' has an incomplete layout (a field type C interop could not import)` | `unknown type 'ZZZ__i32'` |
| `int mk<T>() { return 5; }` then `mk<int>()` | `cannot cast an aggregate value - a fixed array decays to a pointer` | RUNS, prints 5 |
| `ZZZ<int>` as return type / global / struct field / pointer param | incomplete-layout, or silently accepted (pointer param, global) | `unknown type 'ZZZ__i32'` |
| `NS.ZZZ<int> z;` (qualified, unknown) | `unknown type 'NS.ZZZ__i32'` | unchanged |
| unknown NON-generic `WWW` / `WWW*` | `unknown type 'WWW'` | unchanged - the generic spelling now matches its non-generic twin |

**Accept set, frozen BEFORE the gate was written and re-measured after - every cell identical
PRE and POST:** declared bare template (`Box<int>` = 7); n-argument generic function
(`mk2<int>(1)` = 6); namespaced zero-arg generic function (`N.mk<int>()` = 6); qualified known
template (8); instantiation alias `using B4 = Box4<int>` (9); base alias `using GB5 = Box5` - BOTH spellings
measured, local declaration (11) and function SIGNATURE (`int f(GB5<int> b)`, and the
alias-of-alias and namespaced-base variants of it), all identical PRE and POST only after the
round-1 punch fix below; the first cut measured the local spelling ALONE and shipped a regression
where the signature spelling reported `unknown type 'GB5__i32'` (`ForwardRefScanner::
ScanUsingDeclaration` gated base-alias registration on the MAIN-PASS template maps, empty during
the scan for a template declared in the same file; it now also consults `IsGenericTemplateKey` and
the uncertain set, and `scratch/rv_s_alias_base.cb` / `rv_s3_alias_after_use.cb` /
`rv_w_alias_of_alias_sig.cb` / `rv_u_alias_ns_base_sig.cb` print 4 / 4 / 3 / 6 on b18ae7f and
again now);
generic interface template + boxing (12); nested type argument (`NBox<NBox<int>>`); imported
user template (31); core `list<int>` / `dictionary<string,int>` (41, 42) on BOTH a warm and a
cold cache; generic class in an unfoldable `if const` arm (21) and its interface twin (23);
foldable arm (22); generic class declared and used inside an expect_error block; bare use of a
namespaced generic interface (`Unknown identifier 'Width'.`); use-before-template of a real
template (`Unknown identifier 'v'.`); unknown type ARGUMENT (`VBox<QQQ>` -> `unknown type 'QQQ'`).

**Claims that were checked rather than inherited.** The filed issue said this bug is "the CAUSE of
two of the three causes" in [[incomplete-layout-message-blames-c-interop]] and that fixing it
"removes most of that P2's reach". Measured: FALSE. All three causes that P2 lists - an abandoned
C-imported record, use-before-declaration (generic AND non-generic), and a type whose declaration
failed inside an expect_error block - still produce the message, byte-identical on both binaries.
The shell this commit gates was a FOURTH, unlisted funnel. That P2 file was updated with the
measurement rather than deleted, and it keeps its severity.

**Coverage.** `Test/errors/err_unknown_type_arg_qualifier.cb` gained two `expect_error` legs -
the unknown generic in a local declaration and the unknown generic in an UNUSED signature (the
silent-accept face). `Test/test_generics.cb` gained `testGnUnresolvedGenericShellGate()`: the
zero-argument generic function call (5), its one-argument control (6), and the `if const`
accept-set leg (21). Each leg was isolated and run against a b18ae7f binary: the two negative legs
report `FAIL: expected error ... did not occur`, and the zero-arg call fails with `cannot cast an
aggregate value`. The round-1 punch added three more legs to the same function -
`gs_same_tu_base_alias_in_signature` (4), `gs_alias_of_alias_in_signature` (3) and
`gs_namespaced_base_alias_in_signature` (6) - each isolated and run against a binary built from the
first cut of this commit, where each reports `unknown type '<alias>__i32'`.

**Verification.** `./test.sh Release` 600 passed / 0 failed / 8 skipped; `bash example_mac.sh
Release` 35 passed / 0 failed. No new `TypeAndValue` / `StructData` / `AnnotationValue` field, so
no `--init` round-trip change is owed; the new set sits beside `scannedGenericStructNames`, which
that file already documents as deliberately not cached (it is rebuilt by every forward-ref scan,
and the cold-cache probe above confirms it).

### Found, not fixed

- A generic name used BEFORE its template is declared still fails, with `Unknown identifier 'v'.`
  rather than a message about ordering. Unchanged by this commit and not generics-specific (the
  non-generic spelling fails the same way) - it is cause 2 of
  [[incomplete-layout-message-blames-c-interop]].
- The remaining three funnels into the incomplete-layout message, per the paragraph above.
- The silent-accept face is NOT fully closed. `AnyGenericTypeTemplateNamed`'s last-dotted-segment
  clause still admits a BARE unknown name that merely matches some namespaced template's last
  segment, with no diagnostic at all: `scratch/rv_b_ns_collision_sig.cb` (`namespace N { class
  Box<T> ... }` plus a top-level `int useIt(Box<int> b)`, where top-level `Box` is declared
  nowhere) and `scratch/rv_m_cross_ns_collision.cb` (a cross-namespace `Tag<T>` collision) both
  compile, link and run clean printing `ok`, measured identical on the b18ae7f binary and on this
  one - so it is NOT a regression. Narrowing the clause would move the three ratified
  `Test/errors/err_namespaced_generic_iface_*.cb` messages, which is why it is deliberately not
  fixed here. Filed as [[last-segment-collision-still-shells-unknown-generic]].
### fix/shape-arg - a SELECTED shape-mismatched funcptr argument now rejects instead of lowering

Closed `shape-mismatched-funcptr-arg-binds-silently` (file deleted).

**Subsumption first.** The filed repro `only(&g)` and the issue file's own "variable spelling"
`only(p)` (a `function<T>*` VARIABLE, not just `&expr`) are BOTH already rejected on `bdd6869`
with the pre-existing "cannot pass a non-function pointer value ... a data pointer would be
called as code" message - `closure-param-accepts-data-pointer`'s provenance gate landed first and
subsumed the filed spelling. Verified directly (not inferred): both compile-reject with that exact
wording on the pre-`fix/shape-arg` binary. What remained live was the residual shape cross-product
the provenance gate cannot see, because the argument genuinely IS a function pointer there - just
the wrong INDIRECTION SHAPE.

**Matrix (argument shape x parameter shape, `function<int(int)>`), PRE -> POST:**

| Argument | Param VALUE (shape 0) | Param POINTER `T*` (shape 1) | Param VIEW `T[]` (shape 2) |
|---|---|---|---|
| VALUE (a `function<T>` variable) | ok (shape0->shape0) | **SIGSEGV 139 -> rejected** | **SIGSEGV 139 -> rejected** |
| NAMED FUNCTION (bare `dbl`, no variable) | ok | **SIGSEGV 139 -> rejected** | **SIGSEGV 139 -> rejected** |
| POINTER `&var` / a `T*` variable (`p`) | already rejected (provenance gate, message frozen) | ok (shape1->shape1) | rejected (separate "raw pointer as array-view" gate, frozen, unrelated to this fix) |
| FIXED ARRAY `T[N]` (the whole array) | **SIGBUS 138 -> rejected** | ok (array decays to its own address = a valid slot address; frozen accept, `shape_array_into_pointer_decay`) | ok (array decays to a view; pre-existing, `cvs_fnptr_array_view`) |
| ARRAY ELEMENT `arr[i]` | ok (shape0->shape0; frozen accept, `shape_array_element_into_value`) | n/a (address-of an element into `T*` is `&arr[i]`, a POINTER arg, covered above) | n/a |
| `nullptr` | SIGSEGV 139, uninvestigated - filed separately as `nullptr-into-thin-funcptr-value-calls-null` (P3), likely a DIFFERENT bug (null-deref-at-call, not a shape confusion; `FunctionPointerShapeOf` sees shape0==shape0, no disagreement to reject) | ok, unaffected (compared, never called; frozen accept, `shape_nullptr_into_pointer`) | ok, unaffected (frozen accept, `shape_nullptr_into_view`) |

Two-arm overload ranking (`pickFnPtrShape`/`pickFnPtrShapeRev`, the `4000fa1` behaviour) is
unaffected by construction: the gate only fires on the SELECTED candidate's argument, and ranking
already prefers the shape-matching arm, so a correctly-resolving two-arm call never reaches a
mismatch. Frozen as accept-set legs (`shape_overload_picks_array_arm`,
`shape_overload_picks_pointer_arm`).

Note the matrix has THREE live faces, not the two the issue file described: the issue's repro and
root cause covered VALUE->POINTER only. Phase A's cross-product also found VALUE->VIEW and
ARRAY->VALUE live (both SIGSEGV/SIGBUS, no diagnostic) and a named-function spelling of the first
two (bare `dbl`, not through a variable) - all fixed by the same two gates below, since a named
function scores `FunctionPointerShapeOf` shape 0 exactly like a variable.

**The gate** is one shared function, `LLVMBackend::RejectFuncPtrShapeMismatch`
(`cflat/LLVMBackend_Overloads.cpp`), judging the (argument, parameter) pair and reporting; it runs
after the candidate is already SELECTED, so multi-arm ranking is untouched. Two faces:
- Parameter shape 1 or 2 (`function<T>*` / `function<T>[]`, both `Pointer=true`) with argument
  shape 0 - a bare VALUE reaching a pointer/view slot. Exempts a literal `nullptr`
  (`ConstantPointerNull`) explicitly, matching `ArgumentIsProvablyDataPointer`'s own escape hatch.
- Parameter shape 0 (the plain-VALUE param) with argument shape 2 (array/view) only, NOT `!= 0` -
  a shape-1 (pointer) argument is already caught by `ArgumentIsProvablyDataPointer`, with its OWN
  message; widening to `!= 0` would preempt that check and change its wording (would have broken
  the `only(&g)`/`only(p)` freeze).

**Gate INVENTORY - three doors, not one.** A first cut applied the two checks inline at the two
lowering arms of `CreateOverloadedFunctionCall`, which left two live faces behind (both measured on
`bdd6869` AND on that first cut: compiles clean, exit 139, no diagnostic):
- **Virtual dispatch.** `LLVMBackend::CallInterfaceMethod` (`cflat/LLVMBackend_WinRT.cpp`) lowers
  its own argument list and never enters `CreateOverloadedFunctionCall`, so ARRAY->VALUE and
  VALUE->POINTER through an interface method were untouched. It now calls the shared gate at the
  head of its per-argument loop (skipping an interface parameter, whose own arm owns that pair).
- **Encoded (monomorphized generic) closure parameters.** `list<function<int(int)>>::add`'s
  `T value` is not `IsFunctionPointer`, so a gate keyed on that flag alone never saw it -
  `L.add(arr)` with a `function<int(int)>[2]` bound silently and SIGSEGV'd at `L[0](3)`. The gate's
  parameter predicate now mirrors the SCORER's, `IsFunctionPointer || IsEncodedClosureType(...)`,
  which has always treated the two as one family.

**Message wording** (`LLVMBackend::FuncPtrShapeWord`, now taking the `TypeAndValue` + optional
`NamedVariable` instead of a bare shape int):
- The old version hard-coded `a 'function<>[]' view` for EVERY shape-2 operand and `function<>` for
  every family, so a fixed `function<T>[N]` and a `Lambda<T>[N]` were both described as a
  `function<>[]` view - a spelling the source never wrote.
- The family is now derived: `__c_fn_ptr` -> `function<>`, `__closure_fat_ptr` -> `Lambda<>`, an
  encoded closure via its registered flavour, and a bare `llvm::Function` argument -> `function<>`
  (a named function is a thin code address). When none of those answers - the common case on the
  ARGUMENT side, since the call-argument loops copy a stored closure's SIGNATURE but deliberately
  not its TypeName - the word is the generic "closure value" / "closure array" / "closure view"
  rather than an asserted spelling. The PARAMETER word is always exact (shape and family are part
  of its declaration), so the message still names what the slot wants.
- The trailing ADVICE branches on the parameter's shape: "Take the address with `&`" for a pointer
  param, "Pass a `function<>[N]` array or another view" for a VIEW param. The old text advised `&`
  for both, which is false for a view - `&g` is a pointer, and a raw pointer into a `T[]` slot is
  refused by the array-view gate, i.e. the advice sent the user into a second error.

**Guard polarity.** Both gates reject only a PROVABLE shape disagreement between two CLASSIFIED
shapes (`FunctionPointerShapeOf` on the PARAMETER is always fully reliable - shape is part of its
static declaration; on the ARGUMENT it is reliable once `Pointer`/`IsArrayView`/`ConstArraySize`
are correctly threaded through). `0 == 0` (both value-shaped, the common case) is never touched.

**A real defect surfaced mid-fix, not in the gate polarity but in the SIGNAL the gate reads.** The
ARRAY->VALUE direction (`only(arr)`) does not lower through a shape computed at the gate site at
all - by the time an argument reaches `CreateOverloadedFunctionCall`, a by-value read of a fixed
array has already been decayed to its element-0 address by `LoadNamedVariable`
(`MainListener_Expressions.cpp`), and neither METHOD call-argument-assembly loop
(`MainListener_PostfixExpression.cpp`, the INTERFACE-method-call loop and the STRUCT-method-call
loop - not the free-function-call loop, which never needed it) propagated `ConstArraySize` onto the
argument `NamedVariable` the way it already propagates `IsArrayView` (with an explicit comment
stating why: "otherwise ... looks like a thin `T*`"). Both sites now propagate `ConstArraySize`
alongside `IsArrayView`, for the identical reason.

**Which spellings the propagation actually decides** (measured against a build of this commit with
the two `ConstArraySize` lines removed, not inferred): a LOCAL fixed array through a FREE call
(`only(arr)`), through a STRUCT method, and a `Lambda<T>[N]` array all still reject WITHOUT it -
those carry their array-ness some other way. The two spellings that need it are a struct FIELD
holding a fixed array (`only(b.cbs)`, `scratch/rv_d3.cb`: compiles clean and exits 139 without the
propagation) and a FILE-SCOPE fixed array (the `shapeArrayIntoValue` leg's spelling: silently
compiles and links without it). Both are pinned by legs, so the two lines are not dead weight. A first
attempt fell back to re-deriving the argument's shape from its declared symbol-table entry by
`CallerName` when the direct read gave shape 0 - this FALSE-REJECTED an array ELEMENT access
(`arr[i]`, correctly shape 0) because `CallerName` names the base array for BOTH the whole-array
and the per-element spelling, and the fallback could not tell them apart; found via the accept-set
leg `shape_array_element_into_value` before it shipped, then removed in favour of the direct
`ConstArraySize` propagation (which distinguishes them correctly, since indexing already clears
`ConstArraySize` to 0 on the element's `NamedVariable`).

**Accept set** (`Test/test_function_ptr.cb::testFuncPtrShapeGateAccepts`, frozen before the reject
was written, re-run after - every leg also passed on the pre-fix binary): array decaying to a
pointer parameter, an array element into a value parameter, `nullptr` into a pointer parameter,
`nullptr` into a view parameter, and both arms of the two-arm shape-ranking overload set. Plus the
accept halves of the two extra doors: value -> value through a VTABLE slot (variable and bare-name
spellings) and through an ENCODED closure parameter (`list<function<int(int)>>::add`, then calling
the stored element). Plus the full existing `Test/test_function_ptr.cb` (`74/74`, was `73/73` - one
new test function added) and `Test/errors/err_data_pointer_to_closure_param.cb` (`127/127`
`expect_error` legs, all PASS, including the 5 pre-existing legs that pin the frozen
provenance-gate wording).

**Legs** (`Test/errors/err_data_pointer_to_closure_param.cb`, new `expect_error` blocks, each
proven fail-on-PRE against a build at `bdd6869`): VALUE->POINTER (`shapeValueIntoPointer`),
VALUE->VIEW (`shapeValueIntoView`), ARRAY->VALUE (`shapeArrayIntoValue`), the named-function
spelling of the first two (`shapeNamedFunctionIntoPointer`, `shapeNamedFunctionIntoView` - the
latter pins the corrected VIEW advice rather than the message head), the two VIRTUAL faces
(`shapeArrayIntoValueVirtual`, `shapeValueIntoPointerVirtual`, both exit 139 pre-fix), the ENCODED
closure parameter (`shapeArrayIntoEncodedClosureValue`, exit 139 pre-fix), the FAT family of
ARRAY->VALUE (`shapeLambdaArrayIntoValue`, `Lambda<int(int)>[2]` into a `Lambda<int(int)>` value,
exit 139 pre-fix - every other leg passes a thin `function<>`), and the view-VARIABLE face
(`shapeViewVariableIntoValue`, see the message change below) - 10 legs.

**A message CHANGE, recorded deliberately.** A `function<T>[]` VIEW VARIABLE passed into a VALUE
parameter (`function<int(int)>[] v = arr; only(v);`, `scratch/rv_e3.cb`) was ALREADY rejected pre-fix
by the closure-PROVENANCE gate, with "cannot pass a non-function pointer value to `function<>`
parameter `f`". The shape gate now runs first and reports the SHAPES instead. This is a deliberate
preemption, not a regression: the provenance wording asserted the value was not a function pointer,
which is false for a view OF function pointers, while the shape wording names the real defect and
the fix ("index an element"). Pinned by `shapeViewVariableIntoValue` so it cannot silently revert.
The `only(&g)` / `only(p)` POINTER spellings keep the provenance wording untouched (shape 1 into a
value parameter is still left to that gate on purpose).

**Found, filed, not fixed here**: `nullptr` into a plain `function<T>` VALUE parameter compiles
clean and SIGSEGVs when called - filed as `nullptr-into-thin-funcptr-value-calls-null` (P3),
flagged as an open DESIGN question (intentional null-pointer-call UB, matching C, vs. a missed
null-safety gap) rather than fixed, since `FunctionPointerShapeOf` sees no shape disagreement
there and forcing it into this gate would conflate two different questions.

`./cmake_build.sh release && bash test.sh Release` - 600 passed, 0 failed, 8 skipped.
`bash example_mac.sh Release` - 35 passed, 0 failed. One commit,
`git rev-list --count bdd6869..HEAD` = 1.

### fix/coalesce-tail - `??=` routed through the shared store tail, LANDED

Fixed and deleted `p1/codegen/coalesce-assign-skips-store-bookkeeping.md`. The `??=` handler in
`ParseAssignmentExpression` emitted its own compare/branch/store and RETURNED, so every check the
plain-`=` path runs afterwards was skipped for that one spelling. It now emits ONLY the null test
and the branch, rewrites `operatorText` to `"="`, and falls through into the shared tail with the
builder positioned in the assign arm; a `finishStore` lambda closes the arm and yields the
destination's value at each of the tail's 29 return sites. The RHS is therefore parsed by
`ParseAssignmentExpressionNamed` (a real `NamedVariable`, which is what the provenance gate reads)
and still evaluated inside the assign arm, so `??=` keeps short-circuiting.

**The oracle.** `??=` is defined as `if (x == null) x = rhs;`, so that desugared spelling - not the
bare `=` - is the reference, and it was measured independently per subsystem. It says the tail's
compile-time bookkeeping is FLOW-INSENSITIVE: an `=` inside an `if` applies its facts to the
enclosing binding unconditionally. That is right for facts that RESTRICT later use and wrong for
facts that RETIRE a restriction, which is the one place this fix deliberately diverges from the
desugaring (below).

**Per-subsystem oracle table.** PRE = `152728c`; `=` and desugared columns measured on PRE.

| # | subsystem | probe | `=` / desugared oracle | `??=` PRE | `??=` POST |
|---|-----------|-------|------------------------|-----------|------------|
| 1 | `TransferPointerOwnershipOnStore` | `R* p = new R(); h.f ??= p;` then read `p` (`h.f` is `unique R*`) | rejects: `use of moved variable 'p'` | compiles, rc 133 (double free at scope exit) | rejects, same wording |
| 2 | `TransferMoveStringOwnershipOnStore` | move-string RHS into a `??=` destination | n/a | UNREACHABLE | UNREACHABLE - the null test needs a scalar; a `string` destination dies first with `condition must be a scalar ... not 'string'` |
| 3 | `MarkVariableUnmoved` / `...FieldUnmoved` / `...NotExplicitlyMovedNull` | `R* b = move a; a ??= new R(); a->v` | `a=9 b=3` rc 0 | rejects: `dereference of moved variable 'a'` (false rejection) | `a=9 b=3` rc 0 |
| 4 | `ClearOwnMoveOrigin` | leg 3 compiled `--sanitize=ownership` | `a=9 b=3` rc 0 | same false rejection | `a=9 b=3` rc 0 |
| 5 | `ClearVariableBond` | `int* view = Borrow(&a); view ??= &b; a = 6;` | `v=5 v2=5 a=6` rc 0 (bond cleared) | rejects `a = 6` (bond survives) | rejects `a = 6` - DELIBERATE, see below |
| 6 | `SetVariableBorrowsOwnedString` | `string s; s ??= b.name;` | n/a | UNREACHABLE (same scalar rule as 2) | UNREACHABLE |
| 7 | `SetVariableBorrowsOwnedElement` | `R* g = l.get(0); g ??= new R(); delete g;` | accepts, `mid dtors=1 end dtors=2`, and the DESUGARED spelling then rc 133 | rejects the `delete` | rejects the `delete` - DELIBERATE, see below |
| 8 | `CheckThinFnPtrAssignProvenance` | `function<int(int)> f; f ??= vp;` | rejects: `cannot assign a 'void*' value to 'function<>' destination 'f'` | compiles, rc 138 (SIGBUS) | rejects, same wording |

**Conditional-store decisions.** Rows 1, 3, 4 and 8 apply UNCONDITIONALLY, matching the desugaring.
For 1 and 8 that is trivially sound: marking a source moved and rejecting a data pointer both
RESTRICT, and a conservative restriction under a may-not-happen store is still sound. For 3 and 4
there is a stronger argument - a moved-from pointer is NULL (CFlat's `move` nulls its source), so
the `??=` null test is always true for one and the store always happens; reviving it can never
un-restrict a live binding. Rows 5 and 7 (and 6, were it reachable) RETIRE a restriction, and there
the desugaring is an unsound oracle: measured, `if (g == nullptr) { g = new R(); }` clears the
container-element taint even though `g` is non-null at run time, and the following raw `delete g`
double-frees (rc 133). Those three therefore take the JOIN under `??=` - the fact is kept unless the
new RHS carries it too - which is byte-identical to what master does for `??=` and keeps the raw
delete guard exactly where the interface-boxing record left it. Recorded as a deliberate divergence
from `=`, not an oversight; see "Found, not fixed".

Two NEW rejections come from the routing, both the `=` oracle's behaviour: `a ??= 6;` while a bond
to `a` is live (`cannot reassign 'a' while 'view' holds a bonded reference`), and `a ??= p;` of a
raw pointer into an array view. Neither one caught a live bug - on PRE both COMPILED and RAN
correctly, because the destination was non-null and the store therefore never happened. They are a
deliberate TIGHTENING to the flow-insensitive rule `=` already applies, and both oracle twins (the
plain `=` and the desugared `if` spellings) reject identically on both binaries. Accept twins were
frozen first: a view-source `w ??= v` (leg in `err_arrayview_bind_reassign.cb`), and the `ct_accept`
corpus - the assignment expression's own value (`int r = (x ??= 5)`), short-circuiting (the RHS call
runs 0 times when the LHS is set, 1 when it is not), `double`, a named function into `function<>`,
and `??=` in a loop. Every one of those cells is byte-identical on both binaries. The corpus's
remaining cell is NOT, and is excluded from that claim: the lambda RHS, which PRE rejects and POST
compiles - see "Widening found in passing" below.

**CoalesceRebound disposition: RETAINED, still load-bearing.** Because row 7 takes the JOIN rather
than clearing, the DECLARATION's element fact still survives a `??=`, so the interface-boxing proof
still needs `MarkPointerRebound`'s `coalesceJoin` flag to suppress the element clause. The
lhs-owner / rhs-owner JOIN rule moved with it into the tail's `MarkPointerRebound` call. Deriving
the RHS owner from `DescribeAssignedSourceOwner(rightNV)` alone was NOT equivalent to the
pre-restructure derivation off the loaded value: it bails unless the RHS binding's `Storage` is an
alloca, and a CAST erases that. Measured for `p ??= (CircleB*)q;` between two borrowed parameters,
then boxed and deleted: PRE rejects it (`cannot delete interface ... it boxes an object that 'p' or
'q' already frees`), the first cut of this branch ACCEPTED it and ran memory-unsafe (double free,
rc 133; the read-back-after-free shape aborts rc 139), and it is now fixed by falling back to the
value-based derivation - coalesce path only, and only when the binding-based one comes back empty -
with the spelling pinned by the new `sCoalesceCast` leg in
`Test/errors/err_delete_borrowed_interface_box.cb` and its accept twin
`delete_box_coalesce_cast_*` in `Test/test_move.cb` (an owning-local cast source under an LHS that
proves nothing, which must stay accepted). The parenthesized and container-element cast spellings
were measured too and reject identically on PRE and on the fixed binary.
`Test/test_move.cb`'s three `coalesce*Box` legs and `err_delete_borrowed_interface_box.cb`'s two
other `??=` legs are unchanged and green.

**One non-tail change was required.** `derefLoad()` bypasses `LoadNamedVariable`, which is where
the null dataflow's guard-read event is logged, so with the store alone routed the pass still could
not see that `??=` tests its destination and kept reporting row 3 as a moved-variable dereference.
The handler now calls `RecordNullRead` for the destination, which is what makes `??=` and its
desugaring agree. It costs one diagnostic: `R* b = move a; a ??= nullptr; a->v` was rejected on PRE
with `dereference of moved variable 'a'`, and now compiles and SIGSEGVs (rc 139) - converging on the
desugared-`=` oracle, which is rc 139 too, so the lost diagnostic is deliberate drift toward the
oracle rather than a regression against it.

**Destination-spelling matrix** (`scratch/ct_dest.cb`), all six PASS identically on PRE and POST:
local, struct field, through-pointer (`*pp`), fixed-array element, global, generic-encoded field
(`Box<R*>.t`). RHS spellings measured: plain value, `move` (unchanged - the move expression nulls
its own source), owning-temp field (rows above), `?:` join, data pointer into thin `function<>`,
interface-boxing sources, lambda literal.

**Widening found in passing.** `f2 ??= (int a) => a + 1;` was rejected on PRE with `cannot assign a
struct value to a pointer variable` - the expected-type threading and the fat-to-thin coercion both
live in the tail. It now compiles and runs; leg `cvj_nullassign_fn_lambda` in
`Test/test_function_ptr.cb`.

**Test legs.** Six error legs, each measured failing on a `152728c` binary with `expected error ...
did not occur` (the join leg in `err_unique_borrow_into_field.cb` was isolated by deleting the bare
leg, since the bare one fires first): `err_unique_borrow_into_field.cb` (the two memory-unsafe
repros - bare and `?:`-join owning temp field), `err_data_pointer_to_closure_param.cb` (the SIGBUS),
`err_move.cb` (row 1), `err_bond_source_reassign.cb` and `err_arrayview_bind_reassign.cb` (the two
new rejections). Three value legs: `coalesce_revives_moved_local_value` / `..._freed_twice` in
`Test/test_move.cb` (fails on PRE with `dereference of moved variable 'a'`),
`cvj_nullassign_fn_lambda` in `Test/test_function_ptr.cb` (fails on PRE with `cannot assign a struct
value to a pointer variable`), and the three short-circuit accept-set legs in `Test/test_basic.cb`,
which are identical on both binaries by design - they guard the property a fall-through restructure
could silently drop. The punch round added one error leg (`sCoalesceCast` in
`err_delete_borrowed_interface_box.cb`) and two value legs (`delete_box_coalesce_cast_*` in
`Test/test_move.cb`); those three are vacuous vs `152728c` (it also rejects the cast spelling) and
instead discriminate against the branch's first cut `6bc1b10`, which accepted it.

**Verification.** `./test.sh Release` 600 passed / 0 failed / 8 skipped; `bash example_mac.sh
Release` 35 passed / 0 failed. A `--check` differential sweep over all 447 `.cb` in `Test/` and
`example/` reports exactly 7 differences, every one a file this commit edits; the other 29 flagged
files differ only in the runtime-core PATH printed inside an `imported file not found` message
(harness noise from two binaries in two directories). No new `TypeAndValue` / `StructData` /
`AnnotationValue` field, so no `--init` round-trip change is owed.

### Found, not fixed

- **`ClearVariableBond` and `SetVariableBorrowsOwnedElement` do NOT converge on `=` for `??=`**
  (rows 5 and 7 above). This is a decision, not a gap: the desugaring oracle retires those facts
  unconditionally and that is measurably unsound under a store that may not happen
  (`if (g == nullptr) { g = new R(); }` then `delete g` is rc 133 on both binaries). Converging
  would need a flow-sensitive fact, which is a different feature. Filed as
  [[conditional-store-retires-borrow-facts-unconditionally]] with both measured oracle pairs -
  note the defect there is in the DESUGARED `=` spelling, not in `??=`.
- `TransferMoveStringOwnershipOnStore` and `SetVariableBorrowsOwnedString` (rows 2 and 6) have no
  live cell at all: `??=` requires a scalar destination and `string` is a two-word struct, so both
  are unreachable by construction. Not filed - the reject face (`condition must be a scalar ...
  not 'string'`) is the accept set's boundary and is measured unchanged, as is the FAT `Lambda<>`
  twin (`g ??= vp;` -> `condition must be a scalar ... not '__closure_fat_ptr'`).

### fix/iface-uninit - a bare interface local with no initializer, genuinely uninitialised (LANDED)

Closed `null-interface-access-remaining-storage-kinds` (file deleted; face 1, the global sub-object
case, was already fixed 2026-08-03 - this closes the last open item, face 2). Read
[`internal/plan/null-interface-access-widening.md`](../plan/null-interface-access-widening.md)
section 7 for the original "explicitly out of scope" note this supersedes for the local-declaration
half; the plan file itself is unchanged except for a one-line pointer to this record.

**Root cause / discriminator.** `PLive lv; lv.Get();` has NO null store and NO constructor call to
reason through - unlike `PLive lv = default;` (a store of a null fat pointer, already rejected by
the existing proof) or `PHolder h;` (a struct field, defaulted via a synthesized ctor call that
returns a zero aggregate, also already rejected). Confirmed on the `--no-opt` IR: `%lv = alloca
%__iface_fat_ptr` has ZERO `StoreInst` users anywhere in the function, only loads - the discriminator
is exactly "this (Base, empty Path) location has no covering store anywhere in F", which is a
whole-function existential fact, not a per-block or per-path proof. That is also why the crash
signal differs: SIGTRAP (133, an LLVM-inserted trap for a load of literally undef/poison memory
feeding a call) rather than the SIGSEGV (139) every proven-null shape produces - verified by
`--no-opt` IR inspection, not inferred from the exit code alone.

**Check site.** `cflat/LLVMBackend_MoveDataflow.cpp`, `RunNullIfaceDispatchCheck` - beside the
existing null-interface proof, reusing its `InterfaceSlotIsFrameLocal` gate (so an escaped, `this`,
heap, or through-pointer base is never even considered - the new check inherits that safety by
construction, not by a repeated check) and its `CollectNullIfaceLocFacts` per-function pass (now
computed unconditionally rather than only when the cross-block hatch is on, since the new check
needs it too). For each live record that neither the same-block nor cross-block null proof
resolves: if `rec.Path.empty()` (the WHOLE receiver, not a sub-object - a sub-object's null-ness
always comes through a synthesized constructor call, which is a real store, so it can never reach
this branch) and `facts[i].ByBlock.empty()` (zero stores anywhere in F touch this location), report
a NEW diagnostic (`ReportNullIfaceUninitAccess`) rather than the existing "last set to null" one,
which would be factually false here. New env hatch `CFLAT_NULL_IFACE_UNINIT_OFF`, independent of the
existing two, since it answers a different question.

**Matrix, PRE (`08328cd`, the branch point) vs POST.** PRE numbers were measured against an
`08328cd` Release build; the main checkout has since advanced past the branch point, so re-running
the scratch harness against `x64/Release/cflat` there measures a LATER commit - rebuild `08328cd`
in a detached worktree instead. Round-2 review independently rebuilt a verified `08328cd` and
reproduced every sampled cell:

| Shape | PRE | POST |
|---|---|---|
| bare local, method call (`lv.Get()`) | compile 0, run 133 | **compile 1**, "uninitialized interface value" |
| bare local, field read (`lv.tag`) | compile 0, run 133 | **compile 1**, "uninitialized interface value" |
| assigned before use, no initializer (`lv; lv = s; lv.Get()`) | compile 0, run 0, value correct | unchanged - a store exists, `ByBlock` non-empty |
| maybe-assigned in a branch, no initializer | compile 0, run 0, value correct | unchanged, same reason |
| back-edge: access precedes assignment on iteration 1, follows on iteration 2 | compile 0, run 0 | unchanged - false negative by design, matching the family's existing loop-carried precedent |
| `PLive lv = default;` (already-null twin) | compile 1 (existing proof) | unchanged, same message, same site - the two diagnostics never collide since a store disqualifies the new check |
| `PHolder h;` (struct field, ctor-zeroed) | compile 1 (existing proof) | unchanged - Path is `[0]`, not empty, so the new check never even evaluates it |
| `PHolder2 h;` (interface field declared WITHOUT `= default`) | compile 1 (existing proof) | unchanged - measured directly: the language still synthesizes a zeroing ctor call even without an explicit field initializer, so this was never actually reachable as a new axis |
| `int x;` (non-interface bare local) | compile 0, run garbage value | unchanged - `pendingNullIfaceDispatch_` is never populated for a non-interface type, so this is out of scope by construction, not by a new guard |
| bare local, access under a branch that is false at run time (`if (cond) { r = lv.Get(); }`) | compile 0, run 0 | **fixed to compile 0, run 0** - round-1 review found this rejected (regression vs PRE); the check now requires `cfg.Cd[accessBlock]` empty (control-dependent on nothing) before reporting, reusing the null proof's `ComputeControlDependence`/`EnsureNullIfaceBlocks` machinery, so a guarded access with no covering store anywhere accepts same as PRE |

**Two conservatism axes, both inherited from the family, neither new in kind.** (1) Any-path-store
disqualifier: `facts[i].ByBlock` non-empty on ANY path (even an unreached one, even one AFTER the
access) turns the check off - this is the pre-existing MUST-uninit design, not new. (2)
Control-dependence containment: the access must be reached unconditionally (its CD set is empty,
the same emptiness the entry-block declaration witness has) - this is the fix landed in round 1 of
review, and it reuses the null proof's own `NullIfaceCfgInfo`/`ComputeControlDependence` rather than
adding a second CD implementation.

**Address-of escape axis - could not be constructed, and does not need to be.** CFlat rejects
`PLive*` outright ("pointer '*' is not allowed on interface type"), so there is no syntactic way to
take the address of a bare interface local and hand it to a callee. This is not a gap: the new
check's precondition is the SAME `InterfaceSlotIsFrameLocal` walk the existing proof already
requires, and that walk already returns false (accept) for anything that is not a load, a
store-into-it, a constant GEP, or a debug/lifetime marker - so even if some future spelling did
expose the address, it would already be excluded before the new check runs, by the same discipline
the plan's do-not-widen list documents for `this->field.method()`.

**Disqualifier false negative - store AFTER the access, still accepted.** `PLive lv; int r =
lv.Get(); lv = s;` compiles 0 on both PRE and POST and crashes 133 at runtime on both: the
any-path-store disqualifier is path-blind about ORDER, not just about reachability, so a store that
lexically follows the access still empties `facts[i].ByBlock` and turns the check off. Honest,
recorded false negative - not a regression, since PRE already accepted this shape; the check narrows
toward the two shapes it can prove (no store anywhere, reached unconditionally), not toward every
uninitialised use.

**Two round-2 observations, recorded for the next reader.** (1) The check infers "declared with no
initializer" from the ABSENCE of stores, and an interface PARAMETER's slot is structurally
identical - it stays accepted only because parameter lowering emits an entry store of the incoming
argument (removing the `ByBlock` disqualifier rejects `core/string.cb`'s `operator string(IString)`
immediately). If parameter lowering ever stops materialising that store, or the check moves after a
mem2reg-class pass, every interface parameter becomes a false rejection. (2) `lv?.Get()` on a
never-initialised local is silently accepted on both binaries and reads garbage - there is no null
to test, which is exactly why the uninit diagnostic deliberately omits the `?.` advice the
proven-null diagnostic gives. Pre-existing shape, safe direction, out of this change's scope.

**Accept-set legs** (`Test/test_interface.cb`, `testNullIfaceDispatchAcceptSet`, legs 52-55 -
`niu_assigned_before_use`, `niu_maybe_assigned_branch`, `niu_loop_carried_accept`,
`niu_guarded_never_taken`): legs 52-55 declare with NO initializer (distinct from legs 1-3, which
start from `= default` and therefore never reach the new check at all) and assert the dispatched
VALUE. Legs 52-53 reach the store disqualifier by construction (axis 1: a store exists on some
path, so `facts[i].ByBlock` is non-empty, and the access is unconditional). Leg 54's access sits
under `if (i == 1)` inside the loop body, so it is accepted by the CD gate REGARDLESS of any store
- it does not pin axis 1 (round-2 review, witnessed by the identical store-free loop-body shape
compiling on POST); it stands as a loop-carried VALUE leg only. Leg 55, added in round-1 review,
pins axis 2: NO store anywhere, but the access sits under a branch that is false at run time, so it
must clear the control-dependence containment test to accept rather than the store disqualifier
(mutation-verified: forcing the CD gate true rejects leg 55's `u4`).

**Reject legs** (`Test/errors/err_iface_field_missing.cb`, SCOPED `expect_error` form - the check
resolves at end-of-body inside `RunNullIfaceDispatchCheck`, the same hook as every other leg in
that file, so a scoped block closes after the diagnostic fires, unlike the module-end global
check): `uvNeverInitCall` (method call) and `fvNeverInitFieldRead` (field read), both proven
fail-on-PRE against a verified `08328cd` Release build (compile 0, run 133) before being added to
the tracked file, and both confirmed in round-2 review to go unmet under
`CFLAT_NULL_IFACE_UNINIT_OFF=1` - pinned to the new check, not a neighbouring guard.

**Sweep (no-regression result, not safety evidence for the guard).** All 535 `.cb` files under
`cflat/core`, `example`, `Test` compiled with `--check`: exactly two hits, both the intentional
error-test legs above; zero in `cflat/core` or `example`. The corpus contains no bare-interface-local
access guarded by a branch, so it cannot exercise the control-dependence containment path added in
round 1 - it shows the new check does not fire spuriously on existing code, nothing more; the
guarded-shape probes and leg 55 above are what actually cover the containment logic.

**Verification.** `./cmake_build.sh release && bash test.sh Release` - 600 passed, 0 failed, 8
skipped. `bash example_mac.sh Release` - 35 passed, 0 failed. One commit,
`git rev-list --count 08328cd..HEAD` = 1. Round 1 review fix (control-dependence suppression) applied
and re-verified against the same two commands with the same result.
