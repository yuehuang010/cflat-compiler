# Mangled generic names leak into user-facing diagnostics (`Box__unique_Itemptr`)

Filed 2026-07-31 while fixing `unique-ptr-field-stack-address-aborts-silently`. Surfaced by
that fix's error test, but **not caused by it** - the mangled owner name comes from
`nv.OwningStructName`, which has always held the monomorphized symbol.

**BROADENED 2026-08-02**: a SECOND site was found (function-pointer signature mismatch), and the
test-fragility this file predicted actually fired. Both are recorded below. The fix direction
changed as a result - a source-spelling field on `StructData` alone is NOT sufficient.

Severity: diagnostic quality only. Nothing miscompiles and no legal code is rejected. Filed
because it also creates a TEST-FRAGILITY problem, below.

## Repro

```cflat
struct Item { int v = default; };
struct Box<T> { T t = default; };
extern int main() { Item i; Box<unique Item*> b = default; b.t = &i; return 0; }
```

```
repro.cb(7,4): cannot store the address of a stack value into unique field
'Box__unique_Itemptr.t' - ...
```

The user wrote `Box<unique Item*>`. The diagnostic answers in the compiler's mangling scheme.

## Second site: a whole function-pointer SIGNATURE rendered mangled (2026-08-02)

Worse than the owner-name case, because the mangled names appear TWICE in one sentence and the
message's whole job is to let the user compare two types:

```cflat
import "function.cb";
struct Box<T> { T v = default; };
void onBoxDouble(Box<double>* b) { b->v = 51.0; }
int run(function<void(Box<i32>*)> f) { Box<i32> q = default; f(&q); return (int)q.v; }
extern int main() { function<void(Box<double>*)> g = onBoxDouble; printf("r=%d\n", run(g)); return 0; }
```
```
q1.cb(5,83): no overload of 'run' matches the given arguments.
  Call arguments (1):
    [0] ptr <unnamed>
  Candidates (1):
    _run_int_cfuncptr_void_Box__i32Ptr_(__c_fn_ptr f)
  Argument mismatch detail (single resolved candidate: _run_int_cfuncptr_void_Box__i32Ptr_):
    [0] arg=ptr  param=__c_fn_ptr
  [_run_int_cfuncptr_void_Box__i32Ptr_] function-pointer signature mismatch: parameter takes
  'void(Box__i32*)' but the argument is 'void(Box__double*)' - parameter 1 differs
```

The REJECTION is correct and is the intended behaviour - a function pointer is called with no
conversion site, so the lowered types must be identical (see the `fix/funcptr-sig` and
`fix/funcptr-close` records in [[interface-issue-queue]]). Only the wording is at fault. The source is
`FuncPtrSpellingOf` (`cflat/LLVMBackend.h`), which prints `FuncPtrParam.TypeName` raw, and that
field holds the MANGLED key - so this is the same defect reached through a different field than
`nv.OwningStructName`.

Two further defects share this message and are NOT the mangling issue. Recorded here because
they are seen together and a fix pass should sweep them at once; neither needs its own file:

1. **Internal type tokens leak.** `__c_fn_ptr`, `_run_int_cfuncptr_void_Box__i32Ptr_`, and the
   `[0] arg=ptr  param=__c_fn_ptr` line, which carries no information at all - under opaque
   pointers EVERY function pointer is `ptr`, so that row is noise on every funcptr mismatch.
   The one useful sentence is last, under a dump that argues against it.
2. **The component is named but the difference is not.** "parameter 1 differs" - not "the type
   argument is `double` where the slot has `i32`". The user has to diff two mangled strings by
   eye to find out what the compiler already knows.

The SCALAR form of the same message is fine, which is a useful control - no generic, no mangling:
`parameter takes 'int(int)' but the argument is 'double(double)' - the return type differs`.

## Why it is worth a file, beyond aesthetics

An `expect_error` test that pins such a message is pinned to the MANGLING SCHEME, so any
change to `MangleTypeArg` silently breaks tests that have nothing to do with mangling. When
this was found, the proposed leg would have been the only `expect_error` string in all of
`Test/errors/` containing a `__` mangled name. It was instead pinned to the stable prefix
(`... into unique field 'Box`), which keeps the suite robust but leaves the diagnostic itself
unfixed. **Prefer prefix-pinning over pinning a mangled name** in any new test until this is
fixed.

**This fired on 2026-08-02, exactly as predicted.** `CanonicalPrimitiveSpelling` folded `int` ->
`i32` in `MangleTypeArg`, and two `expect_error` tests failed with no relation to the change:
`err_iface_call_too_few_args.cb` (pinned `GLive__int`) and `err_global_container_brace_init.cb`
(pinned `list__int`). So the "only one such string" claim above was already stale when written -
there were two more. Both were updated to the new mangled name, which keeps them fragile; they
should be re-pinned to the source spelling once this is fixed.

## Why it was not fixed in that round

Investigated and deliberately deferred - the fix is not contained:

- No demangler exists anywhere in the codebase (no `Demangle` / `UnmangleGeneric` /
  `PrettyTypeName` helper).
- `MangleTypeArg` (`cflat/MainListener.h:225`) is a lossy ONE-WAY transform: `Item*` becomes
  `Itemptr`, a `unique ` prefix is folded in, and args are `_`-joined. There is no inverse,
  and a reverse-parse would misfire on any type name that itself contains `_`.
- `StructData` stores no source spelling to recover.

## Fix direction

Do NOT write a reverse-parser. Store the SOURCE SPELLING at instantiation time and have the
diagnostic helpers prefer it. **Two carriers are needed, not one** - this is the part that
changed on 2026-08-02:

1. A field on `StructData` holding the as-written generic name (`Box<unique Item*>`) alongside
   the mangled symbol. Covers `nv.OwningStructName` and every diagnostic that names a generic
   OWNER.
2. A source spelling reachable from a function-pointer signature COMPONENT.
   `FuncPtrParam.TypeName` / `FuncPtrReturnTypeName` hold the mangled key directly, so (1) does
   not reach them. Either give `FuncPtrSpellingOf` a lookup from mangled key -> spelling (a
   registry keyed on the mangled name serves both carriers and avoids a second field), or record
   the spelling on `FuncPtrParam` itself.

A key -> spelling registry is the better shape: it is one map, it fixes both sites, and it needs
no new field on the per-parameter struct.

**If you add such a field and any analysis reads it, CLAUDE.md's `--init` rule applies**: it
must be added to the cache round-trip in `cflat/LLVMBackend.cpp` in the same change, or it is
silently dropped on a warm cache.

This affects every diagnostic that names a generic owner, not just the `unique` family, so it
is worth doing once centrally rather than per-message.

This is the DISPLAY-NAME half of the identity/display split in
[`internal/plan/funcptr-type-mangling.md`](../../plan/funcptr-type-mangling.md): the mangled key
stays the identity used for comparison and symbols, and the spelling is what diagnostics print.
That plan's layers 1-2 touch `FuncPtrSpellingOf`'s neighbourhood anyway, so doing this alongside
them is cheaper than doing it standalone.

## Test coverage

Indirect: `Test/errors/err_unique_stack_address.cb` currently prefix-pins to avoid the issue.
Once fixed, that leg can pin the source spelling instead. Same for the two legs that fired on
2026-08-02 (`err_iface_call_too_few_args.cb`, `err_global_container_brace_init.cb`), which are
currently pinned to `GLive__i32` / `list__i32`.

The funcptr site is exercised by the generic reject leg at the end of
`Test/errors/err_data_pointer_to_closure_param.cb`, which deliberately pins only
`"function-pointer signature mismatch"` - the stable prefix - and not the mangled operands.

Related: [[interface-issue-queue]], [[funcptr-refuted-candidate-rebinds-onto-pointer-sibling]],
[[generic-function-call-diagnostics-are-misleading]]
