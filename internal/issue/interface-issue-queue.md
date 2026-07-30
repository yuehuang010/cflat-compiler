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

State on 2026-07-30: **59 open issues** (13 P1 / 23 P2 / 16 P3 / 7 UI). Ten files merged into three on their shared root
(consolidation record at the bottom), the generic namespace key space fixed and its file
deleted, its corpus deleted, `archive/` folded into this file, and one new issue filed.
## Resume point

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
- **`fix/iface-ifconst` is SHELVED, not pending.** Branch `23418c2`, worktree
  `../cflat-fix-iface-ifconst`, with its ~60-file repro corpus in `scratch/` - the most valuable
  artifact of that attempt. Read [[iface-ifconst-blame-attempt-shelved]] before touching it.
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
| **P1** | [`p1/`](p1/) | The compiler produces a WRONG PROGRAM, or dies with no usable diagnostic. Silent wrong values, miscompiles, SIGSEGV/abort, verifier failures, missed lifetime errors. | 13 |
| **P2** | [`p2/`](p2/) | Legal code is REJECTED, a feature is unavailable, or an ownership guard has a hole that does not (yet) produce a wrong value. The program does not run, but nothing lies to you. | 23 |
| **P3** | [`p3/`](p3/) | Diagnostic quality, latent/no-repro, deliberate deferrals, and shelved attempts. Real, filed, and not blocking anyone. | 16 |
| **UI** | [`ui/`](ui/) | Separate track - UI / Win32 / WinRT parity. Gates no compiler work; not priority-ranked against the compiler buckets. | 7 |

### P1 - wrong programs and crashes (`p1/`)

| Issue | Severity |
|---|---|
| [[interface-boxing-keyed-on-source-binding]] | Double free (exit 134) via parens / `?:`; verifier failure via `??`; two un-routed boxing sites. Merged 2026-07-30. |
| [[iface-call-does-no-argument-type-matching]] | Silent miscompile then SIGBUS. An `int` reaches a closure slot; the direct path rejects it. |
| [[function-array-body-silently-truncated]] | Silent miscompile, exit 133. `--check` reports PASS. NOT interface-related. |
| [[auto-binding-of-fixed-array-loses-shape]] | Silent miscompile (`auto` on an array is not indexable), and it defeats the primitive-array guard. Fix the deduction, NOT the guard. |
| [[interface-type-alias-not-resolved-in-is-as-target]] | Wrong answer + false rejection: `ia is AliasIB` rejected while `ia is IB` works. Fix with one resolving accessor over the ~12 direct `interfaceTable.find/count` sites. |
| [[ftell-fseek-long-width-on-windows]] | Silent wrong value on Windows: core binds C `long` as pointer-sized, so `ftell`/`fseek` read garbage under LLP64. Not a UI issue despite being Windows-only. |
| [[closure-param-accepts-data-pointer]] | SIGSEGV, no diagnostic. DIRECT-path residue; the virtual path is now guarded. |
| [[interface-method-call-on-null-value-segfaults]] | SIGSEGV (139), no guard. Fires on a PLAIN non-generic interface too. Pre-existing and language-wide. |
| [[unique-ptr-field-stack-address-aborts-silently]] | Silent abort (exit 134), **no diagnostic at all**. Per CLAUDE.md's LLVM-assert convention this should become a proper error. |
| [[return-dangle-missed-when-slot-has-extra-user]] | Missed dangle, no diagnostic. Residue of `2bcc5a0`; NOT to be fixed by widening the whitelist. |
| [[fixed-array-copy-invalid-bitcast]] | Verifier failure, no diagnostic. NOT interface-related. |
| [[ifconst-const-global-condition-corrupts-ir]] | Missing block terminator in an unrelated already-emitted function. Identical on master. |
| [[null-conditional-args-eval-order]] | `?.` call arguments evaluate before the null-guard branch - the guard does not guard them. Filed as latent; it is a wrong-order semantics bug. |

### P2 - false rejections, unavailable features, ownership holes (`p2/`)

| Issue | Family | Severity |
|---|---|---|
| [[array-view-params-unconditionally-noalias]] | latent miscompile | Latent `-O2` miscompile hazard - UB handed to LLVM. P1 the moment a witness exists. |
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
| [[iface-ifconst-base-clause-implementor]] | false reject | Implementor inside a non-taken `if const` -> "no class implements it". |
| [[macos-header-import-and-framework-link]] | false reject | Two gaps block first-class Apple-API binding: header import hard-codes a Linux triple on Darwin (`objc/runtime.h` registers 1 of ~80 functions), and there is no `-framework` / `-F` link channel. The macOS demos work around both with dlopen + typed `objc_msgSend` casts. |
| [[unique-assign-syntactic-owned-rhs-leaks]] | ownership | Owning value laundered through a BORROW-returning call still leaks. |
| [[alias-borrow-local-launder-gaps]] | ownership | An `IsAliasBorrow` owning-struct local launders its borrow through `=` and through `move`. |
| [[delete-borrow-via-named-local]] | ownership | Opt-in spelling closes it; the bare case is still open. |
| [[deref-of-moved-pointer-guard-inside-callee]] | ownership | False positive: guarded only by a conditionally-terminating callee. |
| [[owning-temp-ledgers-should-be-split]] | ownership | `ownedReturnTemps_` fails UNSAFE, `ownedNewTemps_` fails SAFE. |
| [[detection-ledgers-not-discarded-on-aborted-arm]] | ownership | Detection-only ledgers survive an aborted `?:` arm. |

### P3 - diagnostics, latent, deliberate deferrals (`p3/`)

| Issue | Family | Severity |
|---|---|---|
| [[generic-function-call-diagnostics-are-misleading]] | diagnostic | Three defects on one path: a PHANTOM candidate invented for an undeclared generic function, wrong type-arg arity reported as "unknown function 'D3.id__int__float'", and a mangled name leaking into user-facing text. Pre-existing, identical before `e2a23d5`; filed 2026-07-30 out of the layer-4 review. |
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
| [[iface-arg-lambda-fnptr-type-not-propagated]] | latent | No failing shape found; recorded with what was tried. |
| [[nondeterministic-ir-switch-case-order]] | methodology | No miscompile - a METHODOLOGY hazard. Read it before using "0 IR diffs" as proof. |
| [[iface-namespace-follow-ups]] | follow-up | Items 2-6 of the round-1 review of `c9acb6c`. Item 1 is RESOLVED (`853cb87`); items 4 and 5 were fixed by `15809e0`. Item 5's remainder (annotation/template key split) is reachable only on the Windows `[uuid]` / `[winrt]` path. |
| [[iface-ifconst-blame-attempt-shelved]] | shelved | READ BEFORE attempting the `if const` blame diagnostic again. A serious attempt shelved after eight review rounds / nine defects. |

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
  `BoxConcreteIntoInterface` (`MainListener.h:9969`) now carries every guard for two of them;
  the rest is [[interface-boxing-keyed-on-source-binding]].
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

Suite trajectory across the whole sequence: 522 -> 530 -> 536.

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
  `auto` intermediate does, and that is [[auto-binding-of-fixed-array-loses-shape]] rather than a
  widening of the guard.
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
| [[interface-boxing-keyed-on-source-binding]] | `interface-boxing-guards-are-binding-dependent`, `null-coalesce-join-into-interface-not-boxed`, `interface-boxing-sites-not-fully-consolidated` | Boxing keys off the source `NamedVariable`, so parens / `?:` / `??` fall through. |
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
- The fixed-array trio ([[fixed-array-copy-invalid-bitcast]],
  [[fixed-array-parameter-not-callable]], [[auto-binding-of-fixed-array-loses-shape]]) has a
  plausible shared root - the array SHAPE is dropped to a bare `T` - but it is UNPROBED.
  Grouping them in the index is honest; merging them on an unverified hypothesis is not.
