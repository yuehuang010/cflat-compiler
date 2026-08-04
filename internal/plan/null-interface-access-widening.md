# Widening the null-interface-access proof to non-frame-slot receivers

Status: ALL THREE STAGES LANDED (sections 4a, 4b, 4c). Sections 1-3 and 5 are the original plan,
kept as the design record; read 4a/4b/4c for what is actually in the tree. All numbers below are
MEASURED on this worktree
(macOS arm64, Release, `x64/Release/cflat` built from the branch point) unless marked otherwise.
Baseline suite before any change: `./test.sh Release` = **576 passed / 0 failed / 8 skipped**.

**Provenance.** This file absorbs and replaces
`internal/issue/p1/null-interface-access-residue-unproven-receivers.md`, which was the P1 record of
this bug; the issue file is deleted and `internal/issue/interface-issue-queue.md` now points here.
The item is still LIVE and unfixed - it moved homes, it did not close. Everything the issue file
recorded is preserved below: the residue (section 1), the deliberately-accepted set (section 2b),
and the closed parenthesized-receiver design record and the constraint it imposes on anyone
widening the anchor (in section 3).

**Severity**: SIGSEGV at run time (exit 139) with a clean compile and no diagnostic - the same class
as the closed `interface-method-call-on-null-value-segfaults`, and the reason a tracked record has
to survive it. **Not a regression**: every shape below behaves identically on the pre-fix binary and
on master.

Repros and probes live in `scratch/nia/`. Shared preamble for every repro:

```cflat
interface PLive { int tag; int Get(); };
class PImpl : PLive { int tag = default; int d = default; int Get() { return d; } };
struct PHolder { PLive c = default; };
PLive gLv = default;
```

---

## 1. What is broken

The issue recorded 8 open shapes (struct-field / array-element / global receivers). Measured, all 8
still compile rc 0 and crash rc 139:

```cflat
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.Get()); return 0; }   // struct field
extern int main() { PHolder h = default; printf("%d\n", (int)h.c.tag);   return 0; }
extern int main() { PHolder h = default; printf("%d\n", (int)(h.c).tag); return 0; }
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].Get()); return 0; } // array element
extern int main() { PLive[2] a = default; printf("%d\n", (int)a[0].tag);   return 0; }
extern int main() { printf("%d\n", (int)gLv.Get()); return 0; }                        // global
extern int main() { printf("%d\n", (int)gLv.tag);   return 0; }
extern int main() { printf("%d\n", (int)(gLv).tag); return 0; }
```

A struct field and an array element resolve through a GEP and a global through a `GlobalVariable`,
so none of them is the `AllocaInst` the record requires.

**The residue is wider than those 8.** Twelve further spellings were measured on the same binary,
none of them previously recorded anywhere, all compiling clean and crashing:

| # | Shape | Result |
|---|---|---|
| s1 | `PHolder h;` (no initializer at all) then `h.c.Get()` | 0 / 139 |
| s2 | `PHolder h = {};` then `h.c.Get()` | 0 / 139 |
| s3 | `Q* q = new Q(); q->c.Get()` (heap base) | 0 / 139 |
| s4 | `PHolder* p = &h; p->c.Get()` (through a pointer) | 0 / 139 |
| s5 | `PHolder gh = default; gh.c.Get()` (global struct's field) | 0 / 139 |
| s6 | `int k = 1; a[k].Get()` (variable index) | 0 / 139 |
| s7 | local `lv`, a folded `if (1==1) { }` between init and use | 0 / 139 |
| s8 | `f(h)` by value, callee dispatches `h.c.Get()` | 0 / 139 |
| t1 | local `lv`, a live `if (pick(1)==1) { printf(); }` between init and use | 0 / 139 |
| t2 | local `lv`, a `for` loop between init and use | 0 / 139 |
| t4 | `PLive lv;` (no initializer) then `lv.Get()` | 0 / **133** |
| t5 | `PLive lv = nullptr;` + a branch between | 0 / 139 |

**t1, t2, t5 and s7 are the important discovery: the LOCAL-variable case has residue too.** The
closed proof is same-basic-block, so *any* intervening control flow - even an `if` whose body never
touches the receiver - drops the diagnostic on a receiver that was never assigned anything. The
deliberately-accepted list in section 2b covers a receiver **assigned** in a branch or loop; it
does not license a receiver **never assigned at all** merely because a branch sits nearby. That
distinction is the pivot of this plan.

t4 exits 133, not 139 - a different signal, and a genuinely uninitialized read rather than a
proven-null one. Treat it as a separate question (section 7).

## 2. The accept set - measured BEFORE designing the guard

Per `internal/fix-issue-lessons.md` ("Build the accept-set BEFORE the guard, not after"). Every
row below compiles today, runs, and produces the right value. **A widened guard that rejects any of
these is a regression strictly worse than the SIGSEGV it fixes.**

| # | Shape | Today |
|---|---|---|
| a1 | `h.c = i;` then `h.c.Get()`, same block | 0 / 0 / `7` |
| a2 | `a[0] = i;` then `a[0].Get()` | 0 / 0 / `7` |
| a3 | `gLv = gi;` then `gLv.Get()`, same function | 0 / 0 / `7` |
| a4 | `gLv` assigned in `setup()`, dispatched in `main()` | 0 / 0 / `11` |
| a5 | `h.c?.Get()` | 0 / 0 / `0` |
| a6 | `gLv?.Get()` | 0 / 0 / `0` |
| a7 | `if (h.c != nullptr) { h.c.Get(); }` | 0 / 0 (skipped) |
| a8 | `for (k) a[k] = gi;` then `a[1].Get()` | 0 / 0 / `7` |
| c1 | `class Wrap { PLive c = default; int Use() { return c.Get(); } }` + a `Set()` | 0 / 0 / `5` |
| c2 | same, field assigned in the class constructor | 0 / 0 / `6` |
| e1 | `fill(&h, i);` then `h.c.Get()` (address escape) | 0 / 0 / `9` |
| e2 | global assigned in another function of the same file | 0 / 0 / `11` |
| e3 | `if (gLv == nullptr) { ... } else { gLv.Get(); }` | 0 / 0 / `null` |
| g1 | global assigned in an imported `.cb`, dispatched in `main()` | 0 / 0 / `42` |

Two of these are load-bearing design constraints, not just test legs:

- **c1 / c2 are the single largest false-rejection hazard in the whole change.** `int Use() { return
  c.Get(); }` is `this->c.Get()` - a struct-field receiver that is *never* assigned inside `Use()`.
  It is the ordinary way interface fields are used. Any rule shaped as "reject a field receiver with
  no assignment in this function" destroys it. **The base must be required to be a non-escaping
  frame-local `alloca` in the same function, or a global under the whole-module rule of section 5.
  A `this`/parameter/heap base must never be provable.** This also disposes of s3, s4 and s8 -
  they stay accepted, permanently, and are not residue.

- **e3 proves a pure existential rule for globals is unsound.** `gLv` has no store anywhere in that
  program, yet the `else` arm is correct code. A "zero-initialized and never stored" rule rejects
  it. Guard suppression is therefore mandatory for globals, not optional polish.

### 2a. The in-repo inventory

A sweep of all 205 interface declarations across `core/`, `Test/`, `example/` and `performance/`
found every real receiver of the three widened kinds. The result is a clean safety story, because
**every high-risk site is excluded by the base restriction, not by luck**:

- **Field receivers whose assignment is in another function, sometimes another thread**:
  `core/ui_native/cocoa.cb:156-157` (`app`/`tree`, dispatched at `:2114`, `:3436`, assigned at
  `:3392`), `core/ui_native/win32.cb:595-596` (assigned at `:4772`),
  `example/ui/07-testing/todo_app.cb:37` (`this.uiCtx.invalidate()` at `:88`, and `app.uiCtx.post()`
  at `:160` from a worker thread), `example/ui/09-map/map_app.cb:157`/`:378`,
  `Test/test_interface.cb:1056` (`Container.name()` dispatching `inner.name()`),
  `Test/test_interface.cb:2451` (`IfaceViewHolder.sum()`), `core/page_arena.cb:49` (`_root`, read
  across threads). Every one has a `this`/heap/`alias T*` base. Excluded by the frame-local base
  rule. `cocoa.cb:3436` is the extreme case - `curWin().app.render(...)`, where the base is a
  **call result**.
- **Array-element accept witness, better than the synthetic one**: `Test/test_interface.cb:2425-2428`
  - `IShape[4] loc = default;` written only by `loc[i] = sqs[i]` in a loop body, then dispatched at
  `loc[0].area()` / `loc[3].area()`. The loop store is a **non-constant-index GEP off the array
  alloca**, which makes `InterfaceSlotIsFrameLocal(%loc)` return false, so the access is never
  recorded. Same for `Test/test_interface.cb:2413` (`s.vals[i]`). Use `loc[0]` as the regression
  witness rather than a synthetic one.
- **Globals: every in-repo interface-typed global receiver has at least one store somewhere in the
  module.** `__active_allocator` (`core/memory.cb:9`, stored from `program.cb`, `thread.cb:171`,
  `list.cb:235`), `_gApp`/`_gTree` (`core/ui_canvas/win32.cb:61`, stored at `:376`), `_wApp`/`_wTree`
  (`core/ui_native/winui.cb:447`, stored at `:3102`/`:3110`), `_postCtx`
  (`example/ui/08-fedit/fedit.cb:448`, stored at `:575`), `gStackBox`
  (`Test/test_interface.cb:1112`), `gGiR5Global` (`:4111`). Only the synthetic repros have none.
  So Stage 2's "no store anywhere in the module" test costs nothing in this repo - it is the
  control-dependence test (e3) that has to carry the guarded cases like
  `if (__active_allocator != nullptr)`.
- **Globals declared and never touched at all** (`Test/test_interface.cb:3513`, `:3640` - they exist
  to force template instantiation) are never *accessed*, so they produce no record.
- **Mid-block null stores into interface fields and globals are a real, live shape**:
  `core/ui_native/win32.cb:2390` and `cocoa.cb:1577` (`if (this.app != nullptr) { IComponent a =
  this.app; this.app = nullptr; delete a; }`), `winui.cb:3144`, `core/program.cb:26` (nulls
  `__active_allocator`, restores at `:28`), `core/list.cb:235`/`:247`, `core/page_arena.cb:126`.
  None is followed by a dispatch on the nulled slot in the same block - but these are exactly what
  a "last write in this block was null" rule keys on, so they are the first places to re-measure.
- **`core/thread.cb:210-212` / `:243-245`**: `IAllocator child = default;` is *deliberately* left
  null when there is no active allocator, and stored into `args->_allocator`. A legitimately-null
  interface local that is stored but never dispatched. Storing is not a member access, so no record
  - confirm that stays true under Stage 3.
- **Keep `is` / `as` out of the widened rule.**
  `Test/errors/err_generic_interface_unrouted_is_source.cb:36-39` uses a never-assigned global as an
  `is` source with an `expect_error` pinned to a *different* message. `is` is not a `.` member
  access and is not recorded today; if that ever changes, this test breaks by having its expected
  text replaced.

### 2b. Deliberately accepted - do not "close" these

Carried verbatim from the issue file. Per the maintainer's ratified design (no per-dispatch runtime
guard; `?.` is the language's answer wherever liveness is not statically known), the following keep
compiling **on purpose** and must not be turned into rejections - in the plain spelling AND the
parenthesized one:

- `?.` (`lv?.Get()`, `lv?.tag`, `(lv)?.tag`) - the sanctioned remedy, short-circuits.
- a receiver **assigned** in a branch, in a loop body, or anywhere other than the access's own
  straight-line block.
- an access sitting under a branch that is false at run time (`if (pick(0) == 1) { lv.Get(); }`).
- a parameter receiver - its slot's only write is the incoming argument, never a null constant.
- an access inside a folded-away `if const` arm (`if const (sizeof(int) == 99) { lv.Get(); }`).
- a read whose live value was loaded BEFORE a null store later in the same block
  (`lv = s; int t = (lv).tag; lv = nullptr;`) - the anchor is the load, not the access.

The accept set for all of these is `testNullIfaceDispatchAcceptSet` in `Test/test_interface.cb`.

**Note the boundary this plan turns on.** "A receiver *assigned* in a branch or loop" is accepted.
A receiver **never assigned anywhere** is not covered by that licence, and section 1 shows four
such shapes (s7, t1, t2, t5) crashing today purely because a branch sits nearby. Stage 3 closes
those without touching any row above.

Guard polarity remains the load-bearing constraint (`internal/fix-issue-lessons.md`): every gate
here degrades to "no diagnostic", which is the correct direction. **A false rejection would be
strictly worse than the SIGSEGV this plan describes.**

## 3. What already exists - three layers, and where each one stops

### Layer A: the record sites (`cflat/MainListener.h`)

Both sites **already fire for all three residue shapes.**

- Interface FIELD lvalue: [MainListener.h:20344-20432](cflat/MainListener.h:20344). Passes
  `interfaceVar.Storage` as `slot` and a fat-pointer `LoadInst` as `anchor`.
- Interface METHOD dispatch: [MainListener.h:23116-23156](cflat/MainListener.h:23116) into
  [`CallInterfaceMethod`](cflat/LLVMBackend.h:13420), which passes `ifacePtr` as `slot` and the
  vtable `LoadInst` at [LLVMBackend.h:13428](cflat/LLVMBackend.h:13428) as `anchor`.

Neither site filters on storage kind. So the user's read is right at this layer: **nothing needs
inventing here.**

### Layer B: the record filter (`RecordPendingNullIfaceDispatch`)

[LLVMBackend.h:1424-1437](cflat/LLVMBackend.h:1424). One line kills all three shapes:

```cpp
auto* alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(slot);
if (alloca == nullptr || anchorInst == nullptr) return;
```

`PendingNullIfaceDispatch::Slot` is typed `llvm::AllocaInst*`, so the record cannot even hold
anything else. What `Storage` actually is per shape:

| Receiver | `interfaceVar.Storage` | Null-init emission |
|---|---|---|
| local `lv` | `AllocaInst` | `store {ptr,ptr} zeroinitializer, ptr %lv` |
| field `h.c` | `GetElementPtrInst` (`CreateStructGEP`) | **`store %PHolder %callresult, ptr %h`** - the default ctor's return, not a constant |
| elem `a[0]` | `GetElementPtrInst`, 2 constant indices | `store [2 x {ptr,ptr}] zeroinitializer, ptr %a` (whole array) |
| global `gLv` | `llvm::GlobalVariable` | `@gLv = global {ptr,ptr} zeroinitializer` - **no instruction at all** |

### Layer C: the proof (`RunNullIfaceDispatchCheck`)

[LLVMBackend.h:19657-19714](cflat/LLVMBackend.h:19657), plus
[`InterfaceSlotIsFrameLocal`](cflat/LLVMBackend.h:19599) and
[`StoreWritesInterfaceSlot`](cflat/LLVMBackend.h:19641). Same-basic-block linear scan; requires the
last write before the anchor to be a whole-slot store of a null `Constant`.

### The asset the issue record never mentioned: `nulldf`

`cflat/MoveDataflow.h:254-601` is a complete, shipped, per-function control-flow analysis:
post-dominators, control-dependence sets with transitive closure, a MAY fixpoint over witness sets,
and the reporting rule

> report a deref at block D witnessed at block M only when `CD*(D)` is a SUBSET of `CD*(M)`

documented as **false-positive-free by construction** rather than by enumerating guard shapes - its
own header comment names "a struct field, a global" among the guard forms it handles without
recognising them. `nulldf::AnalyzeFunction(F, events, proven)` is fully parameterised on the event
vector, so it can be driven by a second, independent stream without touching the move diagnostics.

This is the machinery that makes the accept rows a7 and e3 fall out for free, and it is why the
plan below reaches for it rather than growing more special cases into the linear scan.

### The closed parenthesized-receiver case, and what it forbids

`(lv).tag` was the one remaining spelling on a NAMED LOCAL that escaped the proof: parenthesising
left the fat value already in `Primary`, and the field record was gated on emitting the load here
and now. Closed 2026-08-02 by `78c678b`, by anchoring on `Primary`'s defining `LoadInst` when that
load reads the same slot in the same basic block as the access - the access consumes the loaded
value, so stores after the load cannot change what faults, and a load from an earlier block is
still declined. Measured before/after on `(lv).tag`, `((lv)).tag`, `(((lv))).tag`, `(lv).tag = 5`,
and the same shapes after a mid-block `lv = nullptr;`: all rejected now, all `exit 139` before. The
method path (`(lv).Get()`) already rejected in every paren spelling and is unchanged. The gate is
visible today at [MainListener.h:20380-20389](cflat/MainListener.h:20380).

**The constraint that binds this plan** (from the landed design record at
`internal/issue/interface-issue-queue.md`, `fix/null-iface`): the same-block reasoning is valid only
while the fat value is a FRESH load. When `Primary` is already set, the value may have been loaded
in an EARLIER block, and anchoring in the current block would inspect stores that happened AFTER the
read - a null store following an earlier non-null load would be a FALSE REJECTION. Anyone widening
this must **carry `Primary`'s defining `LoadInst` and require `load->getParent() ==
access->getParent()`, not widen the anchor.**

**Two things not to mistake for safety here.** The value-asserting companion accepts (legs 13-20 of
`testNullIfaceDispatchAcceptSet`) are NOT tripwires for a widened anchor: a build that drops all
four anchor conditions and reloads at the access point still passed the whole suite. The guard's
narrowness rests on the argument above, not on test coverage. Closing the remaining gap would need
a witness where a null store sits between the anchor load and the access, and no such shape is
currently reachable.

## 4. The one structural gap found by reading, not by probing

`InterfaceSlotIsFrameLocal` walks the alloca's users **one GEP level deep**
([LLVMBackend.h:19619-19630](cflat/LLVMBackend.h:19619)): a constant GEP is allowed only if *its*
users are loads or stores-into-it.

That is exactly deep enough for a local (`%lv` -> `GEP(fatTy,%lv,0)` -> `load`) and one level too
shallow for a field or element **in the method form**:

```
%h -> GEP(%h,0,0)  [field c] -> GEP(fatTy, .,0)  [vtable field] -> load
```

The inner GEP is a GEP user, so the walk returns `false` and the dispatch is silently accepted.
The field-READ form is unaffected, because `EmitInterfaceFieldAddress`
([LLVMBackend.h:12565](cflat/LLVMBackend.h:12565)) consumes the fat **value** via `extractvalue`,
leaving the field GEP with only a load user.

So `h.c.tag` and `h.c.Get()` fail for *different* reasons and Stage 1 must fix both. Verify this
empirically as the first implementation step - it was derived from reading the walk, not measured.

## 4a. STAGE 1 LANDED - 2026-08-03

Implemented and independently verified. Not committed (per CLAUDE.md).

`cflat/LLVMBackend.h` (+211/-70), `cflat/MainListener.h` (+18): `PendingNullIfaceDispatch::Slot`
became `Base` (`AllocaInst*`) + `Path` (constant GEP indices); new `ResolveIfaceStorageLoc`;
`InterfaceSlotIsFrameLocal` is now a bounded worklist walk over constant GEPs of any depth;
`StoreWritesInterfaceSlot` became `StoreWritesInterfaceLoc` (prefix / equal / extension match);
new `NullIfaceStoredConstant` reads the single constant `ret` of a directly-called defined function,
which is what closes the struct field. New `NullIfaceDispatchSite::ReceiverText`, filled from the
postfix-expression children, so the message names `h.c` / `a[0]`; a sub-object receiver that cannot
be named is **not recorded at all**, which also keeps the plain-local wording byte-identical.

**Measured PRE (`904f026`, rebuilt in a detached worktree) vs POST**, every pair in its own spelling:

| shape | PRE | POST |
|---|---|---|
| r1-r5 (field call/read/paren, element call/read) | compile 0, run 139 | **compile 1** |
| s1 `PHolder h;`, s2 `PHolder h = {};` | compile 0, run 139 | **compile 1** (see section 7 correction) |
| r6-r8 (globals) | compile 0, run 139 | compile 0, run 139 - unchanged, Stage 2 |
| s7, t1, t2, t5 (cross-block locals) | compile 0, run 139 | compile 0, run 139 - unchanged, Stage 3 |

Accept set: all 14 rows of section 2 re-run and **value-checked** on POST - a1 `7`, a2 `7`, a3 `7`,
a4 `7`, a5 `0`, a6 `0`, a7 empty/exit 0, a8 `7`, **c1 `5`, c2 `6`**, e1 `9`, e2 `11`, e3 `null`;
s3/s4/s6/s8 still compile 0. Three adversarial shapes added during review, all accept correctly: a
UNION whose interface member aliases another member, a sibling-field write (`t.b = i` then
`t.b.Get()`), and a whole-struct copy from another local (`t = s`).

Suites: `./test.sh Release` **576 / 0 / 8**, `./example_mac.sh` **35 / 0**, `./test_lsp.sh`
**152 / 0**. Repo sweep: all 546 `.cb` files under `cflat/core`, `example`, `Test`, `performance`
compiled with `--check`; **exactly two** emit a null-interface diagnostic, both intentional error
tests. Zero hits in `core/` or `example/`.

Non-vacuity: `Test/errors/err_iface_field_missing.cb` exits **1 on PRE and 0 on POST**, so its six
new legs assert something the old compiler does not do. Accept legs are tripwires by construction -
a false rejection would stop `Test/test_interface.cb` compiling at all - and each asserts a distinct
value (36-48, 137, 143) so a collapse cannot hide.

**Residual gap, correctly accepted:** a field of a NESTED struct (`POuter{PInner{PLive c}}`,
`o.inner.c.Get()`) is not proven - that constructor returns an `insertvalue` chain rather than a
constant, so `NullIfaceStoredConstant` bails. Per "anything else -> accept" it was left alone. The
two-level path IS proven when it comes off one constant store (`PTagBox[2] pb = default;
pb[1].c.kind()`), and that is a reject leg.

## 4b. STAGE 3 LANDED - 2026-08-03 (ran before Stage 2, per the maintainer's ordering)

`cflat/LLVMBackend.h` only (+298/-52); `MainListener.h` untouched. `RunNullIfaceDispatchCheck` now
gates (escape walk unchanged), then tries the straight-line proof, then the cross-block one.

- `NullIfaceStoreAffectsLoc` classifies one store against a `(Base, Path)` location; a longer-path
  write reports "affects, not provably null", which accepts.
- `CollectNullIfaceLocFacts` classifies every store against every candidate location in one pass -
  `O(instrs + stores*locs)` rather than a walk per record.
- `CrossBlockProvesNullIface` is the **MUST** fixpoint: meet = intersection, entry seeded unproven,
  other blocks seeded optimistically (greatest fixpoint = MOP here, since every transfer is a
  constant or the identity). Keyed on `AllocaInst*` + constant path, never a source name. On top of
  the lattice, `nulldf::ComputeControlDependence` + `IsSubset` must hold for EVERY witness block.
- `SameBlockProvesNullIface` and `ReportNullIfaceAccess` are the old proof and messages, extracted
  verbatim - existing reject messages are byte-identical, same line and column.
- Reused from `nulldf`/`movedf`: `ComputeRpoIndex`, `ComputeControlDependence`,
  `ComputePostDominators`, `IsSubset`, `CdSet`, `provenNoReturn_`. **Not** reused: `ApplyEvent`,
  `MergeInto`, `NullState`, `AnalyzeFunction`. The correction in section 5 held up.
- Second escape hatch `CFLAT_NULL_IFACE_XBLOCK_OFF` disables the cross-block half alone; verified
  live (`t1.cb` compiles 0 with it set, 1 without).

**Measured PRE (`904f026`) vs POST**: t1 (live `if` between), t2 (`for` between), t5
(`= nullptr` + branch), s7 (folded `if (1==1)`) all go compile 0 / run 139 -> **compile 1**.
Unchanged: t4 (bare `PLive lv;`, no null store to prove), r6-r8 (globals, Stage 2), s3/s4/s6/s8
(non-alloca bases, permanently accepted).

**Perf**: the `O(B^2)` control-dependence closure is built only after a record is already proven
definitely-null at its access, which correct code never reaches - that is this analysis's
counterpart of `nulldf`'s `haveSet` fast path. Full-suite user+sys 52.2 s before, 51.0-51.6 s
after; `cocoa.cb --check` 0.81/0.78/0.78 s before, 0.81/0.79/0.80/0.81/0.80 after.

**Verification.** `./test.sh Release` 576/0/8, `./example_mac.sh` 35/0, `./test_lsp.sh` 152/0.
Sweep of all 546 `.cb` files: exactly two hits, both intentional error tests, zero in `core/` or
`example/`. `err_iface_field_missing.cb` exits 1 on PRE and 0 on POST.

Independent adversarial set run at review (`scratch/rev3/`), all accepted with correct values:
five MAY-lattice tripwires (assigned in one `if` arm / in an `else` / via `while`+`break` / a
struct field in a branch / every `switch` arm), three control-dependence tripwires (`if (lv !=
nullptr)`, `if (pick(0)==1)`, the field spelling), a loop-carried access that precedes its own
assignment, an early-`return` arm, and a shadowed inner scope. The fix agent separately
mutation-tested the design: flipping the meet to UNION fails to compile `Test/test_interface.cb` at
existing accept leg 2 `nid_branch_assign` - the exact counterexample section 5 predicts - and
deleting the containment test false-rejects existing leg 4 and probe a7.

**Honest gaps recorded rather than papered over.** New accept legs 34 and 35 are not discriminated
by either mutation; they are value legs for shapes nothing else spells, and the test comment says
so. A loop-carried access that precedes its own assignment (`for { lv.Get(); lv = s; }`) is a false
negative and stays compiling - the safe direction, and consistent with the ratified design.

**Provenance note.** Mid-run the fix agent ran `git checkout cflat/LLVMBackend.h` intending to
revert a mutation patch and wiped the implementation; it reconstructed the file and proved the
rebuild `cmp`-identical to the pre-revert binary. At review the tree was rebuilt from source and
every result above re-measured against that binary, so what is verified here is what is in the
tree, independent of that reconstruction.

## 4c. STAGE 2 LANDED - 2026-08-03

`cflat/LLVMBackend.h` (+180), `cflat/LLVMBackend.cpp` (+8), `cflat/MainListener.h` (-3/+3).
Not committed (per CLAUDE.md).

- New `PendingNullIfaceGlobalAccess` ledger (`llvm::WeakVH` for the global and the anchor, so a
  body erased before module end - a temp global-init function - drops out instead of dangling).
  `RecordPendingNullIfaceDispatch` routes a `slot` that IS a `GlobalVariable` there and returns;
  the alloca path is untouched. Recording still cannot reject.
- Resolved by `RunNullIfaceGlobalCheck`, called from `RunMoveDataflow` where the leftover
  per-function records are dropped - inside the compile `try`, before the did-not-occur check.
- Fact 1 is `InterfaceGlobalNeverWritten`: a users walk (transitively through `GEPOperator`, which
  covers the constant-expression GEPs a global's fat-pointer halves are addressed through) where
  every user must be a load or a dbg/lifetime marker. Stricter than `InterfaceSlotIsFrameLocal`
  on purpose - that one permits stores INTO the slot; here one store anywhere is the disqualifier.
  It subsumes the escape test: an address that escapes is not a load user.
- Fact 2 is the Stage 3 CD machinery with the witness synthesised at the accessing function's
  ENTRY block, so the test reduces to `cd[access]` being empty - the access must be reached on
  every path through its function.
- **Only a WHOLE-global receiver is resolved.** A field of a global struct (s5) or an element of a
  global array reaches through a constant-expression GEP and is deliberately not recorded; both
  stay compiling and still SIGSEGV, exactly as before. Extending to them would need their own
  accept legs and was left out.
- Third escape hatch `CFLAT_NULL_IFACE_GLOBAL_OFF` for the global half alone.
- **Interop caveat, decided rather than documented away.** cflat globals get `ExternalLinkage`, so
  a user C TU could write one with no in-module store. The whole global check is skipped when
  `cObjectFiles_`/`cLinkLibs_` is non-empty or a positional `.c` input was given (noted at
  arg-parse time - clang runs for positionals only AFTER the module-end analyses). Verified live:
  `probe.cb probe.c -o x` compiles clean, the same `.cb` alone rejects.

**Measured PRE (the Stage 3 binary) vs POST**: r6 `gLv.Get()`, r7 `gLv.tag`, r8 `(gLv).tag` all go
compile 0 / run 139 -> **compile 1**. Every other row of the probe set is byte-identical to the
Stage 3 baseline (diffed): a3 `7`, a4 `7`, a6 `0`, e2 `11`, **e3 `null`**, g1 `42`, a1/a2/a5/a7/a8,
c1 `5`, c2 `6`, e1 `9`, s3/s4/s5/s6/s8 still compile 0, r1-r5/s1/s2/s7/t1/t2/t5 still reject with
identical text, and all 11 `scratch/rev3/` adversarial probes keep their values.

**Mutation-tested, per leg.** Replacing `InterfaceGlobalNeverWritten` with `true` false-rejects new
accept legs 41, 42 and 43 in isolation, plus the existing in-repo `gStackBox`
(`Test/test_interface.cb:1124`) and probes a3/a4/e2/g1. Deleting the containment test false-rejects
legs 45 and 46 in isolation, plus probe e3. Neither mutation touches the other's legs. The tree was
restored by hand and `diff`-verified identical to the pre-mutation implementation before the final
build.

**One compiler gap had to be fixed to test this at all.** A module-end diagnostic is only catchable
by the BARE-SEMICOLON file-scope `expect_error`, and that form was silently destroyed by any nested
`expect_error` in the same file: the nested one overwrites `expectedError` and cleared it on
completion. Every error test file uses nested forms, so no leg could be added anywhere. New
`LLVMBackend::RestoreFileScopeExpectedError()` re-arms `fileScopeExpectedError_` at the three sites
that used to clear; with an empty file-scope expectation it is exactly the old `clear()`. No
existing test combined the two forms (such a file fails today), so nothing else changes.

**Tests.** Reject legs are one per file, since the module-end report throws and ends the compile:
the FIELD spelling is the last leg of `Test/errors/err_iface_field_missing.cb`, the METHOD spelling
the last leg of `err_iface_call_too_few_args.cb`, both in the bare file-scope form with the reason
in the comment. Accept legs 41-47 of `testNullIfaceDispatchAcceptSet`.

**Verification.** `./test.sh Release` **576 / 0 / 8**, `./example_mac.sh` **35 / 0**,
`./test_lsp.sh` **152 / 0**. Sweep of all 546 `.cb` under `cflat/core`, `example`, `Test`,
`performance`: **exactly two** hits, both intentional error tests, zero in `core/` or `example/`.
Error tests re-run against a WARM `--init-local` cache (the 4 failures there are the 4 on `test.sh`'s
Windows-only ERR_SKIP list). `cocoa.cb --check` 0.76-0.78 s (Stage 3 recorded 0.79-0.81 s).

Further shapes measured, all correct: a namespaced never-assigned global rejects and names
`NS.gLv`; a `thread_local` global is covered (`core/memory.cb`'s `__active_allocator` is saved by
fact 1 - the module really does carry stores to it from `thread.cb`, verified in the IR - and by
fact 2 when it does not); a global accessed inside a LAMBDA body resolves correctly in both
directions (assigned -> `5`, guarded -> `3`, never-assigned unguarded -> rejected); a store
performed inside a lambda body counts as a module store; `switch`-arm and early-`return` shapes
accept; a body aborted by `expect_error` produces no spurious global diagnostic.

## 5. Design

Three stages. **Stage 1 is independently shippable and closes 5 of the 8 shapes in section 1.** Do
not start Stage 2 before Stage 1 is landed green.

### Stage 1 - frame-local base, constant path (struct field + array element)

Replace the `AllocaInst*` key with a canonical storage location:

```cpp
struct IfaceStorageLoc
{
    llvm::Value* Base = nullptr;        // AllocaInst* only in Stage 1
    llvm::SmallVector<uint64_t, 4> Path; // constant GEP indices from Base to the fat slot
};
```

- **Resolve**: walk `slot` back through `GetElementPtrInst`s to a base. Bail - i.e. do not record,
  i.e. accept - on any non-constant index, any non-GEP link, any base that is not an `AllocaInst`
  in the anchor's function. This is what keeps c1/c2 (`this->c`), s3 (heap), s4 (pointer), s6
  (variable index) and s8 (parameter) accepted. `a[k]` also self-excludes because a variable index
  makes `InterfaceSlotIsFrameLocal(%a)` false.
- **Escape**: make the constant-GEP arm of `InterfaceSlotIsFrameLocal` **recursive** over
  constant-indexed GEPs of any depth, still terminating in loads / stores-into-it / dbg / lifetime,
  still returning `false` on everything else. Keep the deliberate exclusion of `llvm.mem*` and the
  comment at [LLVMBackend.h:19603](cflat/LLVMBackend.h:19603) explaining why - a memcpy into the
  slot is a write the scan would miss, and that reasoning survives the widening unchanged.
- **May-write**: generalise `StoreWritesInterfaceSlot` to "this store's destination is `Base`, or a
  GEP off `Base` whose constant path is a prefix of, equal to, or an extension of `Path`". A store
  through a non-constant GEP off `Base` cannot occur (the escape walk already returned false).
- **Null value at the location**: given the last covering store, extract the value actually landing
  at `Path`:
  - store of a `Constant` -> `getAggregateElement` along `Path`, require `isNullValue()`. This
    closes the array element (`store [2 x {ptr,ptr}] zeroinitializer, ptr %a`).
  - store of a `CallInst` whose callee has a single `ret` of a `Constant` -> walk `Path` into that
    returned constant. This closes the struct field, whose null-ness comes from the synthesised
    default constructor's returned aggregate, not from a constant store.
  - **anything else -> accept.** No partial credit, no heuristics.

Closes r1-r5. Leaves r6-r8 (globals) and the cross-block local residue open.

### Stage 2 - globals

**LANDED - see section 4c for what was actually built and measured.**

Globals need three things Stage 1 does not have: the null fact lives in the module-level
initializer; the "never assigned" fact is whole-module; and e3 proves guard suppression is
mandatory. Therefore:

- Keep a separate ledger of global-receiver access records (base `GlobalVariable`, constant path,
  the access's block, name, line, col). Recording is unconditional and cannot reject.
- Resolve at module end, in `RunMoveDataflow` ([LLVMBackend.h:19720](cflat/LLVMBackend.h:19720)) -
  which already runs inside the compile `try` and **before** the expect-error "did not occur"
  check, so a file-scope bare-semicolon `expect_error` still catches it. A *scoped* `expect_error`
  block will not; write the global error tests in the bare form and say so in the test file.
- Reject only when all of: the initializer is null/zeroinitializer at `Path`; **no** store anywhere
  in the module writes the global or any GEP off it; the global's address never escapes (no user
  other than loads and constant GEPs feeding loads); and the access survives the control-dependence
  test of Stage 3 with a synthesised witness in the accessing function's entry block.
- a3/a4/e2/g1 are all saved by "no store anywhere in the module" - verified: the cross-file store
  in g1 is emitted into the same module (`store ptr @PImpl_PLive_vtable, ptr @gLv` appears in
  `g1.ll`). e3, and every guarded `__active_allocator` dispatch in `core/memory.cb`, are saved only
  by the control-dependence test. **Both mechanisms are required; neither alone is sufficient** -
  and per section 2a the in-repo globals all have in-module stores, so the CD test is the one
  carrying real code.
- State explicitly in the code comment that a `.c`-interop TU could in principle write an
  external-linkage cflat global without any in-module store. If that risk is judged real, restrict
  the rule to globals with no `--c-*` interop in the compilation, or drop Stage 2.

### Stage 3 - cross-block, via the existing control-dependence engine

**ORDERING (set 2026-08-03): Stage 3 runs BEFORE Stage 2.** Locals first, then globals. This is also
the right dependency order - section 2's `e3` proves the global rule cannot work without the
control-dependence test, so Stage 2 is built on machinery Stage 3 lands.

> **CORRECTION - the lattice direction. Read this before writing any code.**
>
> An earlier revision of this section said to drive `nulldf::AnalyzeFunction` with a second event
> stream. **That is wrong and would false-reject.** `nulldf` is a MAY analysis: `MergeInto` is a
> plain set UNION, so a witness surviving on *any one* predecessor path reaches the access. That is
> correct for "was this moved out", and wrong for "was this never assigned an implementation".
>
> Worked counterexample, which is accept leg 2 of the existing suite
> (`nid_branch_assign`, [Test/test_interface.cb:4332](Test/test_interface.cb:4332)):
> ```cflat
> NdLive b = default;
> if (ndPick(1) == 1) { b = s22; }
> Test("nid_branch_assign", b.Get(), 22);
> ```
> The access sits in the merge block, which post-dominates the branch and is therefore control
> -dependent on nothing - `cd[merge]` is empty, `cd[entry]` is empty, so the containment test passes
> and the union still carries the entry block's null witness down the else edge. **Reported. A false
> rejection of a program the ratified design deliberately accepts.**
>
> The right shape is a **MUST analysis: intersection at merges, not union.** A location is
> definitely-null at a block only if it is definitely-null on *every* predecessor. That accepts the
> branch-assigned and loop-assigned rows (the assigned path contributes "not null", so the meet is
> not null) and still rejects t1/t2/s7/t5, where no path assigns anything.
>
> So: **reuse `nulldf`'s CFG machinery, not its dataflow.** `ComputeRpoIndex`,
> `ComputePostDominators`, `ComputeControlDependence`, `IsSubset` and `BlockTerminatesProgram` are
> question-independent and are the actual asset. `ApplyEvent`, `MergeInto`, `NullState` and
> `AnalyzeFunction` encode move semantics and must NOT be reused. This is the
> "do not reuse a predicate across a change of QUESTION" lesson, hit for real.
>
> The control-dependence test is still required on top of the MUST lattice - it is what accepts
> `if (lv != nullptr) { lv.Get(); }` and `if (pick(0) == 1) { lv.Get(); }`, where the value really
> is null on every path reaching the access but the access is skippable.

Design, restated correctly. Use a **second, independent** event stream (`nullIfaceEventLog_`) so the
move diagnostics are untouched:

- `SetNull` where the storage is proven null-initialised (Stage 1's proof, at the init block).
- `ClearNull` at any covering write.
- `Deref` at the access.
- `Escape` is unnecessary - `InterfaceSlotIsFrameLocal` already latches escape off entirely.
- **No `Read` events.** A plain read does not initialise anything, and `nulldf`'s read-as-kill is
  move-specific. Guard suppression comes from control dependence.
- **Witness selection for the containment test.** With a MUST lattice a location can have several
  blocks establishing its null-ness. `nulldf` reports when SOME witness satisfies
  `IsSubset(cd[D], cd[M])`; take the conservative direction instead and require it of EVERY
  witness, since more suppression means more accepts. If that loses one of the four target shapes,
  report it rather than silently switching to "some".
- Key events on **storage identity, not source name**. `NullState` is `map<string, WitnessSet>`
  with no scope discriminator, so bare names alias across shadowed scopes; a shadow's `SetNull`
  landing on an outer variable is a FALSE REJECTION mechanism. Build the key from the base
  `Value*` pointer plus the constant path (e.g. `"L<ptr>.0"`), and keep the human-readable name in
  a side map for the message.
- **Perf**: `ComputePostDominators` is O(B^2) and is only affordable today because of the
  `haveSet` fast path ([MoveDataflow.h:371-374](cflat/MoveDataflow.h:371)). The new stream has a
  `SetNull` in every function with an interface local, so `haveSet` no longer bails. Add a second
  fast path - return immediately unless the function has at least one `Deref` event - and measure
  a large compile before and after.

Closes s7, t1, t2, t5, and the cross-block form of every Stage 1 sub-object shape.

**The accept set is the acceptance criterion, not the four target shapes.** Legs 1-33 of
`testNullIfaceDispatchAcceptSet` all pass today and every one of them is now reachable by this
analysis, where under Stage 1 most were out of scope by construction. Legs 2, 3, 4, 5, 9, 10, 12,
16, 17, 26 are the ones that specifically discriminate a MAY lattice from a MUST one, or that rest
on control-dependence suppression.

## 6. Verification

- `./test.sh Release` must stay at **576 / 0 / 8** plus whatever legs are added.
- `example_mac.sh` (35 / 0) and `test_lsp.sh` (152 / 0).
- Every one of the 14 accept rows in section 2 becomes a **value-asserting** leg in
  `testNullIfaceDispatchAcceptSet` ([Test/test_interface.cb:4314](Test/test_interface.cb:4314)) -
  asserting the value, never "it compiled", per the lessons file. c1/c2/e3 in particular.
- The in-repo witnesses of section 2a are already legs and must keep their values:
  `Test/test_interface.cb:2427-2428` (`loc[0]`/`loc[3]` after a loop-body store - the array-element
  witness), `:1058` (`Container.name()`), `:4157` (`n.inner.ci.Get()` - the two-level constant
  path), `:1124-1125` (`gStackBox` assigned same-BB), `:4387-4390` (leg 12, the dead-branch field
  receiver). `example_mac.sh` covers the `ui_native` / `todo_app` / `map_app` field receivers and
  `core/memory.cb`'s guarded `__active_allocator`; those are the real-code half of the argument and
  a suite pass without them is not evidence.
- Reject legs extend `Test/errors/err_iface_field_missing.cb`. No new test files.
- **PRE binary**: a detached worktree at the branch point `904f026`, built with `cmake_build.sh
  release` - currently `/tmp/cflat-pre-904f026/x64/Release/cflat`. Identity verified before use:
  `r1.cb`/`r4.cb` compile rc 0 (the bug present), `k1.cb` rejects with the existing plain-local
  diagnostic (the working case intact). Report every claim as a measured pre/post pair in the exact
  spelling the claim is about - "pre-existing" / "not a regression" is a MEASUREMENT, never an
  inference from a sibling spelling.
  **Keep the PRE binary OUTSIDE the repo.** A copy parked in `scratch/pre/` during Stage 1 was
  deleted by the implementation agent tidying `scratch/`, mid-review, and had to be rebuilt from
  scratch. `scratch/` is the agent's workspace; the reviewer's reference cannot live there.
- Run `--check` over `core/` and `example/` as well as compiling them: the LSP path re-analyses
  partially-typed buffers, so a new rejection shows up as an editor squiggle before it shows up in
  any suite.
- **`--init` round-trip**: no new `TypeAndValue` / `StructData` / `AnnotationValue` field is
  proposed here, so nothing to add - but confirm that before landing, and re-run the error tests
  against a WARM cache, since that is how `expect_error` tests silently stop firing.

## 7. Explicitly out of scope

- **t4 - a bare LOCAL declaration with no initializer (`PLive lv;`).** There is no null store and no
  ctor call to reason through; the memory is genuinely uninitialised, which is a different (and
  probably more valuable) check - "read of an interface that was never initialised". t4's exit 133
  rather than 139 is the tell that this is a different failure. File separately; do not fold it in.
  **Correction (Stage 1 landing):** this paragraph originally also claimed s1 (`PHolder h;`) and s2
  (`PHolder h = {};`) were out of scope for the same reason. That was WRONG for the struct-field
  shapes - both emit a call to the synthesized default constructor and store its result, and that
  constructor returns a zero aggregate, so the field is provably null. Verified behaviourally on the
  PRE binary: `PHolder h; if (h.c == nullptr)` prints `isnull`, i.e. the zero-init is pre-existing
  language semantics, not something the fix introduced. Both now reject, correctly. The claim holds
  only for the bare LOCAL (t4), which is untouched.
- **s3 / s4 / s8 - heap, through-pointer, and by-value-parameter bases.** Permanently accepted, by
  the same rule that protects c1/c2. These are DECIDED, not residue - if this plan lands and a
  successor record is written, they belong in the deliberately-accepted list of section 2b, not in
  a new issue.
- Any per-dispatch runtime guard. The ratified design says `?.` is the answer wherever liveness is
  not statically known.

## 8. Do not retry

- **Do not widen the base past a non-escaping frame-local alloca or a proven-clean global.** c1/c2
  are the witness; `this->field` is how interface fields are normally used.
- **Do not use a pure existential "never stored" rule for globals.** e3 is the counterexample.
- **Do not key the dataflow on source names.** Shadowed scopes alias, and the aliasing direction
  includes false rejection.
- **Do not reuse `CallIsPointerOpaqueIntrinsic` in the escape walk.** It admits `llvm.mem*`; the
  existing comment already records why that is wrong for this question.
- **Do not reuse `nullEventLog_` itself.** Different wording, different `Read` semantics; a shared
  stream couples two diagnostics whose polarities are set independently.
