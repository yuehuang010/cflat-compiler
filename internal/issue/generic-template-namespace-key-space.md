# Generic templates have a namespace-blind key space, so a generic declared in a namespace is unusable

Filed 2026-07-29, consolidating three separately-filed issues that are all one root cause.
Supersedes and replaces:

- `generic-interface-namespace-scope-limit.md` (filed 2026-07-27, the declared scope limit of `c9acb6c`)
- `namespaced-generic-interface-in-signature.md` (filed 2026-07-29)
- `generic-interface-name-vetoed-by-core-template.md` (filed 2026-07-29)

All three were framed as interface problems. They are not: **every generic template kind is
affected**, and the interface case is only the one that was being looked at. Probes below were run
this session on `x64/Release/cflat` with the generic-interface fix applied; all are PRE-EXISTING
(they fail identically on master).

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

## Probes (all under `scratch/nsgi/`)

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
int use(list<T> l) { return l.Get(); }     // Unknown identifier 'Get'.
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
3. **Decide the mangled symbol form for a dotted base.** `NS.Box__int` is emitted as a lookup key
   today and fails. This is the hard part, called out by the predecessor issue and by finding 4 of
   [[iface-namespace-follow-ups]] (`class A : ga.IBox<int>` -> `unknown interface: 'ga.IBox__int'`).
   Whatever is chosen must be a legal LLVM identifier and must round-trip through the `--init` cache.
4. **Key the struct-wins tie-break on the declaring module**, not the bare name, so a core template
   cannot veto a user interface. Keep the tie-break for the genuine same-scope collision that
   `Test/test_generics.cb` depends on.
5. **Scanner sets** (`scannedGenericStructNames`, `scannedGenericInterfaceNames`,
   `MainListener.h:2459` and `2478`) must use the same qualified key, or step 1 just moves the
   disagreement.

Per CLAUDE.md's load-bearing `--init` rule, every key change here MUST be applied to the cache
round-trip in `LLVMBackend.cpp` in the same change, or it is silently dropped on a warm cache and
the tests stop firing.

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

**Step 1 - a standalone runnable corpus at `scratch/nsgi/test_namespaced_generic.cb`**, before
touching the compiler. It MUST stay in `scratch/` while the fix is in progress: `test.sh` /
`test.bat` glob `Test/*.*`, so a half-red file there breaks the suite for everyone. Compile and run
it as

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
pinning the message substring only, never a path. Delete `scratch/nsgi/` after the merge.

## Not consolidated here, but adjacent

- [[duplicate-generic-template-name-silently-accepted]] - the undiagnosed "struct wins" tie-break
  between a generic struct and a generic interface of the same name *in the same scope*. Its probes
  5 and 6 are this issue's root cause; its core accept-set question is separate, and it carries a
  stated blocker (`Test/test_generics.cb` depends on the collision).
- [[bare-interface-name-resolves-outward-before-namespace]] - resolution ORDER for non-generic
  interface names. Same inner-vs-outer walk that step 2 needs; fix them with one convention.
- [[iface-namespace-follow-ups]] - findings 4 and 5 are this root cause. That file's findings 2, 3
  and 6 are unrelated, so it stays.

Related: [[interface-issue-queue]], [[generic-interface-registered-as-opaque-struct]]
