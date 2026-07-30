# Generic templates have a namespace-blind key space, so a generic declared in a namespace is unusable

Filed 2026-07-29, consolidating three separately-filed issues that are all one root cause.
Supersedes and replaces:

- `generic-interface-namespace-scope-limit.md` (filed 2026-07-27, the declared scope limit of `c9acb6c`)
- `namespaced-generic-interface-in-signature.md` (filed 2026-07-29)
- `generic-interface-name-vetoed-by-core-template.md` (filed 2026-07-29)

All three were framed as interface problems. They are not: **generic STRUCT, CLASS and INTERFACE
templates are all affected**, and the interface case is only the one that was being looked at.
Probes below were run this session on `x64/Release/cflat` with the generic-interface fix applied;
all are PRE-EXISTING (they fail identically on master).

> **SCOPE CORRECTION (round 3).** This issue originally claimed "every generic template kind". That
> is false: generic **FUNCTION** templates were never in the probe table, the corpus, or the fix
> direction, and they remain bare-keyed - a namespaced `T ident<T>` is silently discarded, identical
> on both binaries (`scratch/rev/p15_genfunc.cb`). Filed separately as
> [[generic-function-templates-are-bare-keyed]]. What shipped here covers struct, class and
> interface templates only.

Severity: **a whole feature is unusable, with misleading diagnostics** - not a miscompile.
See "Severity correction" below; the predecessor issue claimed a silent miscompile that this
session could not reach.

## The root cause: three disagreeing key conventions

| Site | Key it uses | Code |
|---|---|---|
| Generic STRUCT template registration | `ns.Base` (qualified) | `MainListener.h:24091` |
| Generic CLASS template registration | `ns.Base` (qualified) | `MainListener.h:26488` |
| Generic INTERFACE template registration | `Base` (**bare** - `name` is deliberately shadowed back to `baseName`) | `MainListener.h:4111-4113` |
| Use-site mangling | the **spelled** base, verbatim | `MangledGenericName`, `MainListener.h:23847` |
| Scanner claim/veto sets | the **bare** `directDeclarator()->getText()` | `MainListener.h:2459`, `2478` |

`MangledGenericName` just concatenates: `MangledGenericName("NS.Box", {"int"})` is
`"NS.Box__int"`. Nothing reconciles the spelled base against the registered key, and
`ResolveQualifiedName` is never applied on this path.

Consequences, one per spelling:

- **Qualified use** (`NS.Box<int>`): base `NS.Box` matches the struct/class key, mangles to
  `NS.Box__int`, and that name never becomes a real type -> `unknown type`.
- **Bare use from inside the namespace** (`Box<int>` inside `namespace NS`): mangles to
  `Box__int`, misses the `NS.Box` key entirely, and lands on the unconditional opaque shell that
  the forward scanner pre-declares -> `incomplete layout`. This is the same opaque-struct family
  as `generic-interface-registered-as-opaque-struct.md`, reached by a different route.
- **Interfaces are bare-keyed at declaration**, so two namespaces declaring the same generic
  interface name collapse onto one key, silently, with no diagnostic at either declaration.

## Probes (originally under `scratch/nsgi/`, now deleted)

> Paths in the table below are historical: `scratch/nsgi/` was removed when the corpus merged into
> `Test/`. The round-3 review's probes, which are the live ones, are in `scratch/rev/`.

| # | Shape | File | Result |
|---|---|---|---|
| 1 | `NS.Box<int>` generic STRUCT, plain local | `e_struct.cb` | `unknown type 'NS.Box__int'` |
| 2 | `NS.S<int>` generic CLASS, plain local | `b_local.cb` | `unknown type 'NS.S__int'` |
| 3 | `NS.S<int>` generic class as a PARAMETER | `a_class.cb` | `Unknown identifier 'Get'.` |
| 4 | Bare `Box<int>` used from INSIDE `namespace NS` | `f_bare.cb` | `type 'Box__int' has an incomplete layout (a field type C interop could not import); it can only be used through a pointer` - nonsense diagnostic, no C interop involved |
| 5 | Generic class + param, both INSIDE the namespace | `d_inside.cb` | `Unknown identifier 'Get'.` |
| 6 | `NS.C<int>` generic INTERFACE as a parameter | (predecessor issue's repro) | `Unknown identifier 'Get'.` |
| 7 | Two namespaces each declaring `interface IV<T>`, IDENTICAL contracts, bare use at file scope | `h_collide2.cb` | **Compiles and prints `width=32`** - the two templates collapsed onto the bare key `IV` with no diagnostic |
| 8 | Same, but the two contracts DIFFER (`Tag()` vs `Other()`) | `i_collide3.cb` | `unknown function 'Tag'` - clean rejection, no wrong value |
| 9 | Two namespaces, qualified spelling, to reach the collision directly | `g_collide.cb` | Blocked earlier by #2: `unknown type 'A.CA__int'` |

Probe 2 also corrects the record: `namespaced-generic-interface-in-signature.md` asserted that the
`NS.S<int>` local in its repro worked and only the interface parameter failed. A bare generic-class
local in a namespace does **not** work.

`using namespace NS;` is not a workaround - the grammar has no such form
(`extraneous input 'namespace'`).

## Severity correction

`generic-interface-namespace-scope-limit.md` recorded severity as "SILENT MISCOMPILE, same shape as
the bug `c9acb6c` fixed", on the strength of a repro *direction* that was never built. This session
built it (probes 7-9) and **could not reach a wrong value**:

- Differing contracts reject cleanly (probe 8), because a method call resolves through
  `interfaceTable` and a missing entry is a compile error.
- Identical contracts compile (probe 7), but two structurally identical interfaces dispatch the same
  way, so there is no observable difference to be wrong about.
- The qualified spelling that would let two namespaces be distinguished at the use site errors out
  before the collision matters (probe 9).

So the honest severity is **false rejection plus a silent name collapse**, not a miscompile. It is
still worth fixing - the collapse is an identity bug waiting for a shape that does observe it, and
the feature is simply unavailable today - but nobody should be told there is a live wrong-value bug
here on the basis of these probes.

## The core-template veto is the same key space

`LLVMBackend::IsGenericInterfaceTemplateName` (`LLVMBackend.h:9395-9400`) resolves an
interface/struct name collision in favour of the struct, globally:

```cpp
if (gts.genericStructTemplates.count(name) != 0
    || gts.genericClassTemplates.count(name) != 0
    || gts.scannedGenericStructNames.count(name) != 0)
    return false;
```

That tie-break is *required* - `Test/test_generics.cb` legitimately declares `Container<T>` as both
a generic struct (line 21) and a generic interface (line 204) and needs `Container__int` to be a
real struct type - but it is applied per bare name across the whole compile rather than per
declaring scope. So a core template vetoes an unrelated user interface of the same name:

```cflat
interface list<T> { T Get(); };            // 'list' is a generic CLASS in core/list.cb
class L<T> : list<T> { T d = default; T Get() { return d; } };
int use(list<int> l) { return l.Get(); }   // Unknown identifier 'Get'.
```

Master-parity - the name never becomes a fat pointer, it just keeps the pre-fix behaviour. Reachable
names, from a sweep of `cflat/core/`: `block_pool`, `arena_channel`, `array`, `channel`,
`dictionary`, `HResult`, `ComPtr`, `hashset`, `list`, `page_arena`, `Pair`, `queue`, `span`, `stack`,
`spsc_queue`, `TaskResult`, `tuple`, `view`. Of these `Pair`, `array`, `list`, `span`, `queue`,
`stack` and `view` are plausible user interface names, so the set is not theoretical.

This cannot be fixed by qualifying namespaces alone: core's `list<T>` and a user's
`interface list<T>` are both at global scope, so they are genuinely the same key. It needs the
tie-break keyed on the **declaring file/module**, which is why it belongs in this issue rather than
being solved incidentally by it.

> **CORRECTION (round 2): this section is WRONG about the remedy, and the work is re-filed.**
> Keying the tie-break on the declaring module does not fix it - it moves the false rejection onto
> core's own containers. See the note under step 4 of the fix direction, and
> [[generic-interface-name-vetoed-by-core-template]], which now owns this. Everything else in this
> issue (steps 1, 2, 3, 5) is done and shipped; this section is the only part that is not.

## Fix direction

One deliberate change to the whole key space, not a local patch. In order:

1. **Qualify generic INTERFACE template registration** the way struct/class already are
   (`MainListener.h:4111` - delete the `std::string name = baseName;` shadow), covering
   `genericInterfaceTemplates`, `genericInterfaceTypeParams`, `genericInterfacePackIndex`, and
   `MaterializeGenericInterface`. This alone also fixes finding 5 of
   [[iface-namespace-follow-ups]] (`SetTypeAnnotations` already uses the qualified `name`, so the
   annotation key and the template key currently disagree).
2. **Resolve the use-site base before mangling.** Every `MangledGenericName(baseName, ...)` caller
   must first resolve `baseName` against the template maps through the enclosing-namespace chain,
   innermost first, then fall back to the bare tail after the last `.`. That is what makes both the
   qualified and the bare-from-inside spellings land on the same key. Note the ordering hazard
   recorded in [[bare-interface-name-resolves-outward-before-namespace]]: inner scope must win over
   outer, and today outer wins for non-generic interfaces.
3. **Mangled symbol form for a dotted base: DECIDED - keep the dot, `NS.Box__int`.** The predecessor
   issue called this "the hard part" and finding 4 of [[iface-namespace-follow-ups]] treated
   `ga.IBox__int` as evidence the form was wrong. It is not. A NON-generic namespaced struct already
   registers and lowers under its dotted name today, verified on `scratch/nsgi/j_nongeneric.cb`:

   ```llvm
   %NS.Plain = type { i32 }
   define internal %NS.Plain @_NS.Plain_NS.Plain__() #0 {
   ```

   So a dot is already legal in both LLVM type names and function symbols here, and the generic form
   should match the non-generic convention rather than invent a separate escaping scheme. The failure
   today is NOT that `NS.Box__int` is an illegal or unusable name - it is that nothing ever creates
   it. That makes step 3 a non-decision, and removes the reason this issue was thought to need a
   design call before implementation.
4. **Key the struct-wins tie-break on the declaring module**, not the bare name, so a core template
   cannot veto a user interface. Keep the tie-break for the genuine same-scope collision that
   `Test/test_generics.cb` depends on.

   > **NOT DONE - step 4 was implemented, then REVERTED, and is now re-filed on its own.**
   > Steps 1, 2, 3 and 5 landed and are independent of this one. Two findings, in order:
   >
   > 1. "Different module -> interface wins" is directly contradicted by an existing ratified
   >    assertion: `Test/test_interface.cb` legs 16/17/19 ("Cross-file bare-name collisions - the
   >    struct role owns the mangled name") pin a user `interface GiCollideRev<T>` LOSING to a
   >    `struct GiCollideRev<T>` imported from `Test/library/gi_collide_struct.cb` - the SAME shape
   >    as this issue's `list` veto leg, with the opposite expected outcome. Implementing step 4
   >    literally gives `529 passed, 1 failed`.
   > 2. Narrowing it to **core-vs-user** (the interface takes the key only when the sole struct
   >    claimant is a core template) keeps the suite green but TRADES ONE FALSE REJECTION FOR
   >    ANOTHER, and for the more common shape. `scratch/nsgi/t3_clash.cb` declares
   >    `interface list<T>` and then uses core's `list<int>` container: pre-fix it compiles and
   >    prints `core count=1`; with the core-vs-user tie-break it fails with
   >    `no overload of 'add' matches the given arguments`. That is a REGRESSION, not a tightening.
   >
   > The root obstruction, which this issue's text does not state: the two shapes are **mutually
   > exclusive**. Both spell a bare `list<int>` at GLOBAL scope, and one needs it to mean the
   > interface while the other needs it to mean core's class. No deterministic rule separates them,
   > and `global::` - the only scope-escape qualifier in the grammar - cannot distinguish two roles
   > that both live at root scope. So step 4 is not a tie-break problem at all: it needs either a
   > new disambiguating spelling or a diagnostic that rejects the collision outright. Re-filed as
   > [[generic-interface-name-vetoed-by-core-template]] with the evidence.
5. **Scanner sets** (`scannedGenericStructNames`, `scannedGenericInterfaceNames`,
   `MainListener.h:2459` and `2478`) must use the same qualified key, or step 1 just moves the
   disagreement.

Per CLAUDE.md's load-bearing `--init` rule, every key change here MUST be applied to the cache
round-trip in `LLVMBackend.cpp` in the same change, or it is silently dropped on a warm cache and
the tests stop firing.

## What the fix changed for programs that already compiled

Re-derived from scratch in round 3 by sweeping every probe in `scratch/rev/` against a pre-fix
binary (`09f1d56`, worktree `../cflat-prefix-check`) and the fixed one. The witnesses cited here are
live paths under `scratch/rev/`; the round-2 list cited `scratch/nsgi/`, which has since been
deleted when the corpus was merged into `Test/`.

Only these six shapes behave differently. Everything else that compiled before compiles the same,
and every remaining difference is "pre-fix errored, now works" or "pre-fix errored, now errors
better" (e.g. `scratch/rev/p10_iface_launder.cb`, which now names the interface the class fails to
implement instead of failing earlier on an unknown one).

**T1 - TIGHTENING. A generic INTERFACE declared in a namespace is no longer reachable by a BARE
spelling from outside that namespace.** Step 1 keys it qualified, so the bare key it used to answer
to is gone. Witness `scratch/rev/w_t1_iface_bare_outside.cb` - a SINGLE namespace, no collision,
everything else spelled bare at file scope: pre-fix `t1=7`, now `Unknown identifier 'Width'.` The
fix is to spell the use `NS.IV<int>`.

> Ratified on consistency grounds: the NON-generic analogs behave this way already, on BOTH
> binaries. `scratch/rev/w_t1b_nongeneric_control.cb` -> `unknown type 'P'`, identical pre and post.
> So the generic interface's old bare-from-file-scope reachability was an ANOMALY produced by the
> bare-key bug, not a feature being removed. Pinned by
> `Test/errors/err_namespaced_generic_iface_bare_single_ns.cb`.

**T2 - SILENT MEANING CHANGE. Inside a namespace, a bare generic name now binds to the
namespace-local template instead of a same-named GLOBAL one.** The program compiles both ways and
returns a different answer, which is why it is pinned as a test (`testGnNsInnerScopeWins`) rather
than left in scratch. Ratified: inner scope must win. Three witnesses, all of which COMPILED
pre-fix:

- `scratch/rev/p1_depth.cb` - nested namespaces `A`, `A.B`, `A.C` over a global template:
  pre-fix `inB=1 inC=1 inA=1` (always the global one), now `inB=3 inC=2 inA=2`. Confirms the walk is
  innermost-first and falls outward correctly (`A.C` has no local template, so it reaches `A`'s).
- `scratch/rev/p8b_ifconst_taken.cb` - the namespace-local template is declared inside a TAKEN
  `if const (__MACOS__)`: pre-fix `1`, now `2`.
- `scratch/rev/p12c_forward_global.cb` / `t2` shape as pinned in `Test/test_generics.cb`.

**T2b - SILENT MEANING CHANGE, same walk. A bare generic name used inside `namespace Outer` now
finds a template nested in a same-named `struct Outer`.** Witness
`scratch/rev/p6_struct_key_hijack.cb`: pre-fix `inNsOuter=1` (the global template), now
`inNsOuter=5` (the struct-nested one).

> Ratified on the same consistency grounds as T1: the NON-generic control
> `scratch/rev/p6b_struct_key_hijack_ng.cb` prints `ng_inNsOuter=5` on BOTH binaries. So 5 is the
> compiler's own answer for this shape and the generic form has converged onto it; pre-fix's 1 was
> the anomaly. Note this is the read direction of the same struct-vs-namespace key ambiguity that
> round 3 had to fix in the write direction (see "The declaring scope must be RECORDED" below) -
> the difference is that here the answer the ambiguity produces happens to be the correct one.

**T3 - LOOSENING. A generic struct/class declared in a NAMESPACE no longer vetoes a same-named
GLOBAL generic interface.** `scannedGenericStructNames` was over-inclusive (a namespaced declaration
contributed its bare name); step 5 keys it qualified. Witness
`scratch/rev/w_t3_ns_struct_no_veto.cb`: pre-fix `Unknown identifier 'Width'.`, now `t3=11`.

**T4 - LOOSENING (BONUS FIX). A generic template nested inside a struct now works.** Witness
`scratch/rev/p5_nested_struct.cb`: pre-fix `unknown type 'Outer.Inner__int'`, now correct. Not a
target of this issue; it fell out of the same key-space repair.

> Round 2 recorded T4 as simply "now works". **It shipped a WRONG VALUE**, caught by the round-3
> review: see below. It now returns `9`, matching its non-generic control
> `scratch/rev/p5b_nested_nongeneric.cb` (`9` on both binaries), and is pinned by
> `testGnNsNestedInStructNotNamespace`.

**T5 - TIGHTENING. A bare generic name used BEFORE a same-named namespace-local template is
declared now fails.** Witness `scratch/rev/p12_forward.cb` - `NS.f()` uses `Box<int>` on the line
above `namespace NS`'s own `struct Box<T>`: pre-fix `forward=1` (it silently bound outward to the
global template), now
`type 'NS.Box__int' has an incomplete layout (a field type C interop could not import); it can only
be used through a pointer`.

> This is T2 meeting a PRE-EXISTING gap rather than a new one: use-before-generic-declaration fails
> identically at global scope on both binaries (`scratch/rev/p12b_forward_only.cb`). The fix moved
> which template the name resolves to, and the resolved one is not yet declared at that point.
>
> **The diagnostic is misleading and worth its own fix**: it blames C interop ("a field type C
> interop could not import") for a file that imports no C at all. It is the generic opaque-shell
> message reused for an incomplete layout of any cause. Any user hitting T5 - or the pre-existing
> global-scope version - is sent looking in entirely the wrong place.

**Untested cross-platform risk (Windows only).** `ScanGenericTypeUses`,
`QueueInstantiateGenericType` and `ScanAndQueueGenericTypeUses` now also see the dotted
`qualifiedGenericIdentifier` spelling, so a WinRT base such as
`Windows.Foundation.IReference<int>` can reach `pendingInstantiations` from two sites it previously
could not. It falls through to the existing idempotent `InstantiateWinrtGenericInterface`, and the
new qualified branch in `ScanGenericTypeUses` is gated on `IsGenericTemplateKey`, which a winmd base
never satisfies, so no opaque shell is created. This is the one path a macOS host cannot exercise.

## The declaring scope must be RECORDED, never derived from the key

Round 3's most important lesson, and the cause of two separate silent wrong values that shipped in
round 2. **Struct nesting and namespace nesting share one dotted key space.** A template in
`namespace Outer` and a template nested in `struct Outer` are BOTH keyed `Outer.Box`
(`MainListener.h` passes the outer struct's name as the `namespaceName` argument for a nested
definition). So:

- Recovering the declaring namespace with `rfind('.')` on the key made a struct-nested template's
  body resolve its bare names against a same-named NAMESPACE and silently return that namespace's
  type. `scratch/rev/p5_nested_struct.cb` returned `5` (the namespace's `Helper`) where the
  non-generic control returns `9`.
- The fix is a parallel map, `GenericTemplateState::genericTemplateNamespace`, written at
  registration from `GetCurrentNamespace()` - which is correct regardless of struct nesting - and
  read by `TemplateNamespaceScope`. It is serialized as `decl_ns` in the `--init` round-trip per
  CLAUDE.md's load-bearing rule; every core template is at global scope today, so omitting it would
  have been harmless right now, which is exactly how it would have gone unnoticed later.

The second wrong value was the mirror of it in the ALIAS path: piping a `using` generic-base alias's
TARGET through the namespace walk re-resolved an explicit, already-qualified target at the USE site,
so a global `using GBox = Box;` silently named `NS.Box` inside `namespace NS`
(`scratch/rev/p11_alias_hijack.cb`, `2` where it must be `1`). An alias hit now short-circuits the
walk. Both are pinned by `testGnNsNestedInStructNotNamespace` and
`testGnNsGenericBaseAliasKeepsDeclSiteMeaning` in `Test/test_generics.cb`.

**The `decl_ns` serialization is correct but currently UNEXERCISED - do not mistake a green suite for
coverage of it.** Verified directly: a `--init` cache written by the fixed binary contains **zero**
occurrences of `decl_ns`, because every core template is at global scope and the field is omitted
when empty. So the write path is real, the read path is real, and neither runs today. Two things
make that acceptable rather than alarming:

- The cache is a **named-key JSON map**, not a positional record, so an absent `decl_ns` is an
  unambiguous absence and cannot desync later fields. Confirmed empirically: a cache written by the
  PRE binary (which has no such field) and read by the fixed binary gives byte-identical results to
  cold. There is no old-cache hazard here.
- `GenericTemplateState::Clear()` is `*this = GenericTemplateState{}`, a whole-struct reset, so the
  new map cannot go stale across an LSP re-analysis and could not have been forgotten in the reset.

It becomes live the moment any `cflat/core/*.cb` declares a generic template inside a namespace. A
change that does so should re-run the cold-vs-warm comparison above, because that is the point at
which a silent drop would start producing a warm-cache-only wrong value.

## `currentNamespace_` must not survive a reset

`LogError` THROWS on the batch (`--check`) and LSP paths, unwinding past any hand-rolled
save/restore of `currentNamespace_`. Before this fix a stale namespace was mostly cosmetic; now it
steers the generic template key space, so a file that errors inside a namespace caused FALSE
REJECTIONS in every later file of a batched `--check`:

```
cflat --check scratch/rev/p7_f1_leak.cb scratch/rev/p7_f2_user.cb
  before: Checked 2 file(s), 2 failed.   (second file passes on its own)
  after:  Checked 2 file(s), 1 failed.
```

Two halves, both required: `ResetForReanalysis` now clears `currentNamespace_` (the CLAUDE.md
reset-hygiene rule), and every save/restore site is now RAII via `LLVMBackend::NamespaceScope`.
`test.sh` runs one file per process, so **the suite cannot express this regression**; `test.bat` is
the batching consumer, and the same reset path backs LSP re-analysis.

## Polarity warning

This is the recurring failure mode of this queue, and it has already bitten once here. Widening the
routing predicate `IsGenericInterfaceTemplateName` without widening every validation predicate
keyed on the narrow one turns a compiling program into an LLVM verifier failure - that was round 1,
defect 1 of the generic-interface fix. Prefer record-then-resolve (decide once
`interfaceTable` is complete) over an at-site check, for the reason documented at
`LLVMBackend.h:16301`: "in `genericInterfaceInstances`, not yet in `interfaceTable`" is a
legitimately transient state, so no at-site check can be sound.

## Test coverage today

**None.** There is no namespaced-generic use anywhere in `Test/` or `cflat/core/` (verified by
sweep), which is why a whole unusable feature went unnoticed. Any fix needs positive legs added to
`Test/test_generics.cb` (generic struct/class in a namespace, qualified and bare-from-inside) and
`Test/test_interface.cb` (generic interface in a namespace, as local/param/field/return), plus a
`Test/errors/` leg pinning whatever the two-namespace same-name collision is decided to be.

## Test plan - BUILD THE TEST FIRST

Zero coverage exists, and the accept set here is wide enough that it must be pinned before the fix
is written. Same order as `generic-interface-registered-as-opaque-struct.md` used, which worked.

**Step 1 - a standalone runnable corpus**, before touching the compiler. The corpus is **embedded in
this issue file** (see "The corpus" below) and that copy is the tracked source of truth: `scratch/`
is gitignored, so a corpus living only there is lost on a fresh clone, exactly the reason
`internal/skill/` exists per CLAUDE.md. Work on it at `scratch/nsgi/test_namespaced_generic.cb` and
keep the embedded copy in sync; it MUST NOT live under `Test/` while the fix is in progress, because
`test.sh` / `test.bat` glob `Test/*.*` and a half-red file there breaks the suite for everyone.
Compile and run it as

```bash
x64/Release/cflat scratch/nsgi/test_namespaced_generic.cb -i Test -i Test/library --run
```

Note the `-i Test -i Test/library` pair - `test_helper.cb` lives in `Test/`, not `Test/library/`.

Every leg must assert a VALUE, not merely that the program compiled: the failure family here
includes binding to the wrong template, which links cleanly. Legs, at minimum - one per row of the
probe table above, plus:

| Shape | Why |
|---|---|
| Generic struct/class in a namespace: local, param, return, struct field | the four positions that broke for the unqualified interface case |
| Same, spelled bare from inside the namespace | the `Box__int` opaque-shell route |
| Generic interface in a namespace, same four positions | the predecessor issue's shape |
| Two namespaces, same generic name, DIFFERENT contracts, each used qualified | must dispatch to its own template - this is the identity leg |
| Nested namespaces (`A.B.Box<int>`) | the dotted-mangling decision of step 3 |
| A namespaced generic taking a namespaced generic as a type argument | mangling composition |
| `interface list<T>` at global scope alongside core's `list<T>` class | the veto leg; needs step 4 |
| A namespaced generic instantiated ONLY from another namespace | resolution is use-site, not declaration-site |
| Controls that pass today: every one of the above with the namespace REMOVED | must not regress |

Record each leg's verbatim pre-fix behaviour in a header comment block. That is the non-vacuity
evidence: a leg that passes on both the before and after binaries is testing nothing and must be
replaced.

**Step 2 - fix the compiler**, per the fix direction above, verifying against the corpus the whole
way. Not done until every leg passes AND `./test.sh Release` is green on this host. Run the corpus
against a `--init` warm cache too - step 1 of the fix changes cache keys, and the `--init` rule is
load-bearing.

**Step 3 - merge into the existing suite, only once step 2 is green.** Positive legs fold into
`Test/test_generics.cb` (struct/class) and `Test/test_interface.cb` (interface) as
`bool testNamespacedGeneric*()` functions called from the existing `extern int main()`. Do NOT add a
new file under `Test/`. Anything rejected by design goes to `Test/errors/` as an `expect_error` leg
pinning the message substring only, never a path. Delete `scratch/nsgi/` after the merge, and strip
the embedded corpus from this file at the same time - once the assertions live in the suite, the
embedded copy is a stale duplicate, and this whole file goes away with the fix anyway.

## The corpus

The standalone corpus (37 legs) has been merged into the suite, step 3 of the test plan above.
`scratch/nsgi/` is deleted; the assertions now live in:

- `Test/test_generics.cb` (prefix `testGnNs*`, 23 legs: generic STRUCT/CLASS in a namespace,
  qualified and bare, all four positions, identity across two namespaces, nested namespaces,
  namespaced-generic-as-type-argument composition, cross-namespace use, multiple live
  instantiations, and leg 40 `testGnNsInnerScopeWins`, the ratified inner-scope-wins meaning
  change).
- `Test/test_interface.cb` (prefix `testGiNs*`, 5 legs: generic INTERFACE in a namespace, all
  four positions, plus the qualified two-namespace collision leg).
- `Test/errors/err_namespaced_generic_iface_collide_identical.cb`,
  `Test/errors/err_namespaced_generic_iface_collide_differing.cb`, and
  `Test/errors/err_namespaced_generic_iface_bare_single_ns.cb` (the three by-design rejections:
  former corpus legs 20, 21, and the single-namespace bare-spelling tightening).

Round 3 added two more legs to `Test/test_generics.cb`, both regression tests for silent wrong
values that shipped in round 2 and were caught by review (see "The declaring scope must be
RECORDED" above):

- `testGnNsNestedInStructNotNamespace` - a generic template nested in `struct Outer` must resolve
  its body lexically, not against a same-named `namespace Outer`. Asserts 9, alongside the
  non-generic control in the same struct which also asserts 9. The defective binary returned 5.
- `testGnNsGenericBaseAliasKeepsDeclSiteMeaning` - a `using` alias to a generic BASE keeps its
  declaration-site target. Asserts 1. The defective binary returned 2.

Former corpus leg 25 (`testVetoListInterface`, the core-template veto) is NOT merged anywhere -
it still fails, and is tracked separately in
`internal/issue/generic-interface-name-vetoed-by-core-template.md`.


## Not consolidated here, but adjacent

- [[duplicate-generic-template-name-silently-accepted]] - the undiagnosed "struct wins" tie-break
  between a generic struct and a generic interface of the same name *in the same scope*. Its probes
  5 and 6 are this issue's root cause; its core accept-set question is separate, and it carries a
  stated blocker (`Test/test_generics.cb` depends on the collision).
- [[bare-interface-name-resolves-outward-before-namespace]] - resolution ORDER for non-generic
  interface names. Same inner-vs-outer walk that step 2 needs; fix them with one convention.
- [[iface-namespace-follow-ups]] - findings 4 and 5 are this root cause. That file's findings 2, 3
  and 6 are unrelated, so it stays.
- [[generic-type-arguments-not-key-space-resolved]] - the same bug one level down: the template BASE
  is now namespace-resolved, the type ARGUMENTS are not, so `Box<Item>` at global scope and inside
  `namespace A` (where `A.Item` exists) collapse onto one instantiation. Wrong on both binaries;
  filed in round 3 because this fix FLIPS which caller gets the wrong answer.
- [[generic-function-templates-are-bare-keyed]] - the kind this issue's title over-claimed. Generic
  FUNCTION templates keep the bare key, so a namespaced one is silently discarded.
- **UNFILED, found in round 3 and not yet root-caused**: a `using` generic-BASE alias used in the
  same namespace as a BARE use of its target fails on BOTH binaries. Repro
  `scratch/rev/z_alias.cb`; pre-fix `type 'GnAliasBoxG__int' has an incomplete layout...`, post-fix
  `cannot cast an aggregate value...`. Only the message moved, so it is not a regression, but the
  shape had to be avoided when writing `testGnNsGenericBaseAliasKeepsDeclSiteMeaning` (that leg puts
  the bare half outside the namespace). Suspected cause: `ScanGenericTypeUses`'s `tryPreDeclare`
  pre-declares the shell under the ALIAS spelling while the main pass mangles the alias TARGET, so
  the two disagree - but that is a guess, not a diagnosis. Needs its own investigation.

Related: [[interface-issue-queue]], [[generic-interface-registered-as-opaque-struct]]
