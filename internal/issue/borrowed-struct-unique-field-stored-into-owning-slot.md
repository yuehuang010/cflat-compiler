# A borrowed struct's `unique` field can be stored into an owning slot - double free

Severity: silent double free (SIGABRT) from ordinary code. No cast, no unsafe construct, no
`move`, and no diagnostic. Diagnosed 2026-07-20.

## Summary

A by-value parameter of a struct type BORROWS: the struct's bits are copied (including the raw
`unique` pointer) but the callee does not take ownership and does not destruct. That is safe on
its own.

The defect is what happens when that borrowed value is **stored into an owning slot** - assigned
to a longer-lived variable, or returned by value. The raw pointer is copied into a second
`unique` field with no ownership transfer and no invalidation of the first, so two owners now
hold one pointer and both free it.

## Repro

`scratch/borrowstore/repro.cb` (standalone, no imports beyond the default runtime):

```cflat
int g_dtor = 0;
struct Payload { int v = 0; ~Payload() { g_dtor = g_dtor + 1; } };
struct Holder  { unique Payload* p = nullptr; };

// Leg 1: store the borrowed parameter into a longer-lived slot.
Holder g_stash = default;
void stashIt(Holder h) { g_stash = h; }

// Leg 2: return the borrowed parameter by value. Same defect, no global involved.
Holder passthru(Holder h) { return h; }

extern int main()
{
    if (g_dtor == 0)
    {
        Holder a = default;
        a.p = new Payload();
        Holder b = passthru(a);          // same scope, same lifetime
        printf("leg2: a.p=%p b.p=%p (one pointer, two owners)\n", a.p, b.p);
    }                                     // BOTH destruct here -> double free
    printf("unreachable when the defect is present, dtor=%d\n", g_dtor);
    return 0;
}
```

Observed:

```
leg2: a.p=0x600001a40020 b.p=0x600001a40020 (one pointer, two owners)
--- exit=134 ---
```

The final `printf` never runs: both destructors fire at the same scope exit and the second
`delete` aborts under the macOS allocator.

## This is NOT a lifetime bug

The repro above deliberately keeps `a` and `b` in the SAME scope with identical lifetimes.
Nothing outlives anything, and it still double-frees. The defect is ownership DUPLICATION, which
is detectable with local information at the point of the store.

This matters for scope: the standing maintainer ruling in
`internal/plan/unique-ownership.md` ("Lifetime / dangling for borrowed containers: ignored, none
planned") does NOT cover this. A borrow outliving its owner needs escape analysis that is ruled
out permanently; duplicated ownership does not.

The `g_stash` leg (where the alias genuinely outlives the owner) is included only because it was
the original discovery path. It is the same defect, not a second one.

## Root cause

Assignment to a `unique` field is a delete-then-store (null-checked, with an old==new guard).
Each `Holder` destructor therefore releases whatever its own slot holds. Because the by-value
parameter copied the pointer into a second slot without nulling the first, both slots hold the
same address and both release it.

Contrast - the direct form is handled CORRECTLY and MOVES:

```cflat
g_stash = a;    // measured dtor=0 in scope, ownership transferred, clean exit
```

So the same source value reaching the same destination is safe directly and unsafe through a
by-value parameter.

## What is already covered - do not re-fix these

- **Explicit copy** is rejected: `Holder c = a.copy();` gives "cannot copy 'Holder': its field
  'Holder.p' is 'unique' ...".
- **Container stores** are rejected by each container's own `compile_error` poison, with a
  better-targeted message (e.g. "write 'move' at the call site (s.push(move x))"). See
  `Test/errors/err_{list2,dict,btree,array,queue,stack}_noncopyable_*.cb`.
- **Traceable alias delete** is rejected: `delete stale;` where `stale` aliases `Holder.p` gives
  "it aliases unique field 'Holder.p', whose synthesized destructor already frees it".

The gap is exactly the non-container escapes: assignment into an owning slot, and by-value
return.

## Measured matrix: `move` into a `unique` field (2026-07-20)

Six legs, all standalone in `scratch/moveborrow/`. Only ONE of the four forms a user would
reasonably write is safe, and the compiler's own diagnostic recommends one of the unsafe ones.

| Leg | Form | Result |
|-----|------|--------|
| A | `void f(Payload* b) { g.p = move b; }` - borrowed POINTER param into a unique field | double free (134) |
| B | `Payload* stale = h.p; g.p = move stale;` - move a local aliasing a unique field | **errors correctly** |
| C | `void f(Holder h) { g.p = move h.p; }` - move the field out of a BORROWED struct param | double free (134) |
| D | `void f(move Holder h) { g = h; }` - `move` param, whole-struct assign | double free (134) |
| E | `void f(move Holder h) { g.p = move h.p; }` - `move` param, field-level move | **clean, dtor=1** |
| F | `{ Holder a; a.p = new ...; g = a; }` - owned LOCAL, whole-struct assign | **clean, dtor=1** |
| G1 | `Payload* h = new Payload(); g.p = h;` - new'd local into a unique field, same scope | **clean, dtor=1** |
| G2 | `Holder* h = new Holder(); o.g = h;` - new'd local into a `unique Holder*` slot | **clean, dtor=1** |

## MAINTAINER RULING (2026-07-20): this is NOT a language-design issue

Legs F, G1 and G2 prove the ownership-transfer machinery is present and CORRECT: assigning into
a `unique` slot transfers, the source local stops being an owner, and no `move` keyword is
needed. The failures are missing diagnostics on stores that cannot transfer, plus one path that
fails to recognise an owner. Do NOT redesign borrow/move semantics to fix this.

Per-leg disposition:

- **A - must ERROR.** `move` of a borrowed pointer param into a unique field.
- **B - already errors.** No behaviour change; wording needs the borrowed/owned split below.
- **C - must ERROR.** `move` of a field out of a borrowed struct param.
- **D - must IMPLY MOVE.** The destination is an owning slot, so assignment from a `move`
  parameter must transfer exactly as legs F/G1/G2 already do. The `move` param is simply not
  recognised as an owner by the assignment path.

Three separate defects, each needing its own fix:

**Leg A - `move` of a borrowed pointer param is not checked.** `delete` of one already IS
(`MainListener.h:12955`, "cannot delete borrowed parameter ... Declare the parameter 'move b'").
Storing into a `unique` field has identical consequences - the field's synthesized destructor
deletes it - so `g.p = move b` is a deferred `delete b`. The check exists and is simply not
reached from this path. Narrowest fix on the list. Note this is NOT about struct copyability: a
bare `Payload*` param is copyable and must stay so, so the C++-route call-boundary rule does not
cover leg A.

**Leg C - the compiler routes users into a double free.** Leg B's (correct) diagnostic ends with
"Use 'move h.p' to move the field itself (which nulls it)." Leg C is that advice verbatim. It
does null the field - but `h` is a borrowed by-value copy, so it nulls the CALLEE's copy while
the caller's `a.p` still holds the pointer, and both free it. The advice is right when `h` is
owned (leg E) and harmful when `h` is borrowed. The diagnostic cannot currently tell the two
apart; once leg C is fixed, leg B's wording needs a caveat or a borrowed/owned split.

**Leg D - the documented "correct" form double-frees.** This is the most alarming result: `move`
on the parameter is exactly what every other diagnostic tells the user to write, and
`g = h` then COPIES rather than transfers, after which `h` destructs at function exit. Contrast
leg F: the identical assignment from an owned LOCAL transfers correctly (dtor=0 in scope). So
transfer-on-assign is implemented for locals and not for `move` parameters. Leg E shows the
field-level move out of the same parameter works - it is whole-struct assignment from a `move`
param specifically that is broken.

## STATUS 2026-07-20: the four queued items are DONE; the by-value RETURN escape is NOT

Fixed (all in `cflat/MainListener.h`, regression tests
`Test/errors/err_move_borrowed_ptr_into_unique_field.cb`,
`Test/errors/err_move_field_out_of_borrowed_struct_param.cb`, and
`testUniqueLocalAndParam` in `Test/test_move.cb`):

- **A** errors. `ParseMoveExpression`'s pointer result now carries `IsBorrowed`/`BorrowedOrigin`,
  so `move b` no longer launders a borrow past the existing Trap A store check.
- **C** errors. New `IsBorrowedStructParameter` check in `ParseMoveExpression`, keyed on
  `TypeAndValue.ParentVariableName`. Leg E (`move` param) is unaffected.
- **D** transfers. The owning-value MOVE-at-reassignment block excluded any source with
  `TypeAndValue.IsMove`, which wrongly covered a `move` PARAMETER; now `|| IsOwningStruct`.
- **B**'s wording splits on borrowed vs owned source.

STILL BROKEN - by-value RETURN of a borrowed struct param (`Holder passthru(Holder h) { return h; }`)
duplicates the `unique` pointer and aborts. Repro `scratch/moveborrow/orig.cb`. Escape (2) of the
two listed below is untouched; keep this file open for it.

## Escape (2), by-value RETURN: MAINTAINER RULING 2026-07-20

`Holder passthru(Holder h) { return h; }` **should return an `alias`.** Returning a borrowed
unique-owning struct BY VALUE is the error; the remedy is to say which one you meant.

Both remedies are ALREADY IMPLEMENTED and verified working - this needs no new semantics, only
the missing diagnostic on the unannotated form:

| Form | Measured |
|------|----------|
| `Holder passthru(Holder h)` - unannotated | double free, exit 134, NO diagnostic |
| `alias Holder passthru(Holder h)` (`scratch/moveborrow/h1.cb`) | clean, exit 0, dtor=1 |
| `move Holder passthru(move Holder h)` (`scratch/moveborrow/h2.cb`) | clean, exit 0, dtor=1 |

`alias` in RETURN position works today: the caller's `Holder b = passthru(a);` binds `b` as a
borrow, so only `a` frees. `alias` is a soft keyword handled in `ParseDeclarationSpecifiers`
(`MainListener.h:825`, sets `IsAlias`) - position validity is checked in the MainListener copy.

### Return-position matrix, measured 2026-07-20 (`scratch/moveborrow/i*.cb`, `j*.cb`, `k1.cb`)

| Leg | Form | Result |
|-----|------|--------|
| J5 | `move Holder* f(Holder* h)` + `return h` (borrowed POINTER) | **errors correctly** |
| K1 | `move Holder f(Holder h)` + `return h` (borrowed VALUE STRUCT) | double free (134) |
| I1 | value return, mixed branches, OWNED branch taken | clean, exit 0 |
| I2 | value return, mixed branches, BORROW branch taken | double free (134) |
| J1 | bare `Holder*` return, borrow branch | clean, exit 0 |
| J3 | bare `Holder*` return, `return new Holder()` | exit 0 - C escape hatch, NOT a defect (see below) |
| J4 | caller writes `unique Holder* b = f(...)` to own J3's result | errors (correctly) |
| J2 | `Holder mk() { return new Holder(); }` (value return type) | **`module verification failed`** |

**K1 is the narrow fix.** The `move`-return ownership check ALREADY EXISTS and is correct for
pointers (J5: "function declares 'move' return type but returned expression is not owned - value
must come from 'new', a move parameter, or another move-returning function"). It simply does not
cover by-value struct returns. Extending it to value structs is the whole of the value-side fix -
no new rule, no new message, same check.

### STATUS 2026-07-20: J2 and K1 are FIXED

Both in `cflat/MainListener.h`, in the return-statement handler next to the existing pointer
`move`-return check. Regression tests `Test/errors/err_return_new_from_value_return_type.cb`,
`Test/errors/err_move_return_borrowed_struct_value.cb`, plus positive pins for the two pointer
factory modes (`bare_ptr_factory_manual_delete`, `move_ptr_factory_unique_local`) in
`Test/test_move.cb`.

- **J2** errors: returning a `T*` where the declared return type is the value struct `T` is now a
  typed diagnostic naming `T*` / `move T*` / dereference, instead of invalid IR.
- **K1** errors: the `move`-return ownership check now also covers by-value struct returns, reusing
  the pointer form's message verbatim. It keys off `currentFunctionReturnTV.IsMove && !Pointer`
  rather than `currentFunctionReturnsOwned`, since a `move S` VALUE return is deliberately NOT
  returns-owned - `ComputeReturnsOwned` was NOT widened.

STILL OPEN in this file: the unannotated by-value RETURN escape (`orig.cb`, still exit 134) and the
mixed-return uniformity rule (I1/I2).

**MAINTAINER RULING 2026-07-20, in two parts.**

**J2 (value return type) is a CLEAR ERROR.** `Holder mk() { return new Holder(); }` emits invalid
IR - `ret ptr %0` from a function whose LLVM return type is `%Holder` - and surfaces as
`module verification failed`. Per the repo rule (CLAUDE.md, "when encountering an LLVM assert ...
write a proper error message") this needs a real diagnostic at the return. A pointer is not a
value struct; there is no ownership question here, just a missing type error.

**J3: SUPERSEDED by the MAINTAINER RULING of 2026-07-20 below. J3 is now an ERROR.**

A previous revision of this section said the bare pointer return was the C escape hatch and must
NOT be rejected. That is no longer the rule. The escape hatch still exists, but it is now
EXPLICIT: `alias T*`. See "IMPLEMENTED 2026-07-20: bare-pointer fresh-allocation return" below.

| Mode | Signature | Caller | Result |
|------|-----------|--------|--------|
| bare / unannotated (`scratch/moveborrow/l1.cb`) | `Holder* make()` | `Holder* b = make(); delete b;` | **now a compile ERROR** |
| manual, explicit (`scratch/moveborrow/n1.cb`) | `alias Holder* make()` | `Holder* b = make(); delete b;` | clean, exit 0 |
| RAII (`scratch/moveborrow/l2.cb`) | `move Holder* make()` | `unique Holder* b = make();` | clean, freed at scope exit |

J4 ("cannot initialize unique 'b' from a borrowed value") remains CORRECT: adopting ownership
requires the `move` return type.

**Mixed returns must be rejected at the definition.** I1/I2 show ownership of the return value is
branch-dependent under one signature, and J3/J4/J5 show no annotation makes such a function
correct: bare leaks the owned branch, `move` refuses the borrow branch. So the rule is: infer
`alias` when ALL returns yield a borrowed param (this keeps `passthru` annotation-free, which is
the point); error naming `alias` and `move` when returns disagree.

Implementer note: this lands near `ComputeReturnsOwned` (`MainListener.h:684`), which carries a
NOTE recording that a previous widening caused a SIGABRT in `move list<T> copy()`. Treat the
existing narrow condition as load-bearing.

## Fix direction, and a MEASURED dead end

**Do NOT check at the call/parameter boundary.** This was tried on 2026-07-20 (a
`DiagnoseUniqueOwningStructPassedByValue` sibling to `DiagnoseExplicitMoveToBorrowParam` in
`cflat/LLVMBackend.h`) and MEASURED as wrong - 13 suite failures:

- **A genuine false positive**: `bool operator==(LeakHolder a, LeakHolder b)`
  (`Test/test_collection_leaks.cb:149`) takes owning structs by value, reads fields, and never
  stores or destructs. Passing by value is SAFE and must stay legal - a plain borrow measures
  `dtor=1`, not 2.
- **12 preemption failures** across the six `err_*_noncopyable_*` tests (cold + warm): the new
  message fired ahead of the container poison, which is better targeted.

REINTERPRETATION (2026-07-20): the 13 failures are real, but calling the `operator==` case a
"false positive" assumed by-value borrowing of an owning struct is legitimate. C++ and Rust both
say it is not - C++ deletes the copy ctor so the call will not compile, Rust makes by-value a
destructive move; the idiom in both is an explicit reference (`const Holder&` / `&Holder`), i.e.
cflat's `alias`. Under that rule the `operator==` site is simply wrong and takes a one-line edit
to `alias`, and the 12 container failures are a diagnostic-ORDERING problem (run the container
poison first), not evidence against the rule. So the call-boundary check is not dead; what is
dead is blocking pass-by-value while leaving every existing call site unchanged. Pending a
maintainer decision between that route and the store-side route below.

The check belongs at the STORE, not the call. The natural home is alongside the existing alias
tracker that already produces the "it aliases unique field 'Holder.p'" diagnostic - it has the
right shape and the right vocabulary; it just does not follow a pointer that escaped inside a
copied struct.

Both escapes need covering, or the fix is half a fix:
1. assignment of a borrowed struct into an owning slot (global, field, or longer-lived local)
2. by-value RETURN of a borrowed struct parameter

Whatever the diagnostic says, it must name a usable alternative (repo rule): `move` at the call
site to transfer, or `alias` on the parameter to borrow explicitly.

## Related

- `internal/plan/unique-ownership.md` "Deferred by maintainer ruling" records **Blocker 1** - a
  plain by-value param BORROWS while local init MOVES - as "DORMANT ... nothing currently
  exercises the gap". That is no longer true: this repro exercises it and aborts. Update that
  entry when this is fixed or triaged.
- This is the concrete cost of "no-copy is not no-alias": `unique` gained real copy suppression
  for all six lock types in commit `6e0a1c1`, but suppression does not reach a pointer that
  escapes inside a copied struct.

## IMPLEMENTED 2026-07-20: bare-pointer fresh-allocation return is an ERROR

Returning a FRESH ALLOCATION from a function whose declared return type is a BARE `T*` is now
rejected. Rationale: a bare pointer return gives the caller no signal that it owns the result, so
"the caller forgot to delete" is a silent leak the compiler should catch. Both remedies already
existed and needed no new semantics; only the diagnostic was missing.

Check: `cflat/MainListener.h`, in the return-statement handler, immediately after the
interface-return ownership check. Gated on `AsDirectNew(assignExpr)`, a pointer return type that
is not `move`, not `alias`, not returns-owned, and not an array view.

Diagnostic names BOTH remedies: `move T*` (caller adopts with `unique T* x = f();`) and
`alias T*` (caller manages the lifetime by hand).

Regression test `Test/errors/err_return_new_from_bare_pointer_return.cb`, which also pins both
remedies as positive cases. `Test/test_move.cb`'s `makeManual` migrated from a bare `FactoryHolder*`
to `alias FactoryHolder*`, keeping it a positive guard for the new explicit escape hatch.

### SCOPE: direct form only

Only `return new T();` is covered. The two-step (`T* h = new T(); return h;`,
`scratch/moveborrow/m1.cb`) and unique-local (`scratch/moveborrow/m5.cb`) forms leak identically
and are NOT covered.

Provenance IS reachable at the return for both - measured by probing the existing move-return
ownership check, which accepts `move T* f() { T* h = new T(); return h; }` and the `unique` local
form, so `IsOwningValue(right) || lastOwningResult` is already true there. The blocker is not
information, it is BLAST RADIUS: widening the gate to indirect provenance was measured at
**250 of 460 tests failing**, because it rejects `core/function.cb:12`
(`i8* __closure_env_alloc(i64 n) { i8* base = new i8[n]; return base; }`) and other core
allocator/closure primitives, which every program imports. Covering the indirect forms therefore
requires migrating the core raw-memory plumbing to `alias`/`move` first; it is not a one-line
widening.

### Migration performed

- `cflat/core/json.cb` and `cflat/core/xml.cb` `_allocNode()` -> `alias`. These are MIXED-return
  arena allocators (arena node = caller must not free; heap node = caller frees), so `move` would
  be actively WRONG - it would tell the caller to adopt arena memory, turning a leak into a double
  free. `alias` is correct.
- `alias` is VIRAL through locals: a local bound from an `alias` return cannot be returned from a
  non-`alias` function. The json/xml migration therefore cascaded to every node-returning parser
  helper (`_parseNumber/_parseLiteral/_parseObject/_parseArray/_parseValue`, `_parseElement`) and
  to the public `JsonParser::parse`. That cascade is semantically right - the whole parsed node
  graph is manually managed - but it is the main migration cost to expect elsewhere.
- `Test/test_basic.cb:3411` (`return new double[n];`) is NOT hit: its return type is the array
  view `alignas(0, 64) double[]`, and array-view returns are excluded from the check.

### Measured blast radius (macOS, Release)

Before migration: 2 failures (`test_move`, `test_reflect`) - both predicted sites, no surprises.
After migration: `./test.sh Release` 460 passed / 0 failed / 8 skipped; `example_mac.sh` 35/0;
`test_lsp.sh` 152/0. The rule is VIABLE - it rejected no legitimate site that lacked a correct
one-word migration.
