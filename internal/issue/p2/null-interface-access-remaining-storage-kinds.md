# Null interface access: the storage kinds the three-stage widening left open

Residue of `null-interface-access-residue-unproven-receivers`, whose three stages landed 2026-08-03
(`9e7ffc4`, `727f53d`, and the global stage). The design record, the full accept set and the
measured before/after for what IS closed live in
[`internal/plan/null-interface-access-widening.md`](../../plan/null-interface-access-widening.md) -
read it before touching this, especially section 2b (deliberately accepted, do not "close") and the
MUST-vs-MAY correction in section 5.

Severity: SIGSEGV (139) or SIGTRAP (133) at run time with a clean compile and no diagnostic. **Not a
regression** - both behave identically on `904f026`, before any of the three stages.

Shared preamble:

```cflat
interface PLive { int tag; int Get(); };
class PImpl : PLive { int tag = default; int d = default; int Get() { return d; } };
struct PHolder { PLive c = default; };
```

## 1. A field or element of a GLOBAL aggregate - FIXED 2026-08-03

```cflat
PHolder gh = default;
extern int main() { printf("%d ", (int)gh.c.Get()); return 0; }   // was: compile 0, run 139

PLive[2] gArr = default;
extern int main() { printf("%d ", (int)gArr[0].Get()); return 0; } // was: compile 0, run 139
```

Both now reject at compile time, naming the receiver as written (`gh.c`, `gArr[0]`). New
`ResolveIfaceStorageGlobal` (`cflat/LLVMBackend.h`) walks a chain of `llvm::GEPOperator` (covers
both the instruction and constant-expression GEP forms) back to a `GlobalVariable` base, with the
same discipline as `ResolveIfaceStorageLoc`. `PendingNullIfaceGlobalAccess` gained a `Path` field;
`RunNullIfaceGlobalCheck`'s null-initializer test now walks that path with `getAggregateElement`
instead of requiring the whole initializer to be null. `InterfaceGlobalNeverWritten` (fact 1) was
left whole-global/non-path-aware on purpose - coarser than necessary but strictly conservative.
Accept legs 47-51 of `testNullIfaceDispatchAcceptSet` in `Test/test_interface.cb` cover a global
struct field assigned in another function (47, pre-existing, was already accepted before this fix
since the access was simply never recorded), a never-written field/element guarded by
`if (g.c != nullptr)` (48, 50), and one under a run-time-false branch (49, 51).

Reject legs: each error file can carry only ONE module-end (bare-semicolon `expect_error`) leg,
since the report throws and ends the compile - so the three global shapes have two slots.
`Test/errors/err_iface_field_missing.cb` takes the FIELD-of-a-global-struct spelling;
`Test/errors/err_iface_call_too_few_args.cb` KEEPS the whole-global one, because that is the shape
that does not share code with the other two (it arrives as a literal `GlobalVariable` and walks an
empty path). The ARRAY-ELEMENT reject shape has no slot left; it differs from the field shape only
in the index fed to `getAggregateElement`, so accept legs 50-51 are what pin it.

## 2. A local with NO initializer at all

```cflat
extern int main() { PLive lv; printf("%d ", (int)lv.Get()); return 0; }   // compile 0, run 133
```

Note the exit code: **133, not 139** - a different signal, and the tell that this is a different
failure. There is no null store and no constructor call to reason through; unlike `PHolder h;`
(which DOES call a synthesized default ctor that returns a zero aggregate, and now rejects), a bare
interface local is genuinely uninitialised.

Fix direction: this is a "read of a variable that was never initialised" check, not a
definitely-null one, and the diagnostic should say so - the existing wording ("has not been assigned
an implementation since it was last set to null") would be factually false here. Do not fold it into
the null-interface proof.

## NOT residue - decided, do not "close" these

Beyond the list in the plan's section 2b, the three stages settled these permanently:

- **Heap, through-pointer and by-value-parameter bases** (`new Q()` then `q->c.Get()`;
  `PHolder* p = &h; p->c.Get()`; `f(h)` where the callee dispatches `h.c.Get()`). The base must be a
  non-escaping frame-local alloca or a proven-clean global. This restriction is what keeps
  `this->field.method()` compiling, which is the ordinary way interface fields are used across
  `core/ui_native/`, `example/ui/` and `Test/`. **Widening the base is the one change that would
  break real code.**
- **A loop-carried access that precedes its own assignment** (`for { lv.Get(); lv = s; }`). The
  first iteration really is null, but the MUST lattice meets the back-edge's not-null state and
  accepts. A false negative, which is the correct direction.
- **Any compilation involving C interop** (`--c-include` / `--c-lib` / a positional `.c`). cflat
  globals have `ExternalLinkage`, so a C TU could write one with no in-module store; the whole
  global check is skipped in that case by design.

Guard polarity remains the load-bearing constraint: every gate degrades to "no diagnostic". A false
rejection is strictly worse than the SIGSEGV described here.
