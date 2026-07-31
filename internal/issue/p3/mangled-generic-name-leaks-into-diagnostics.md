# Mangled generic names leak into user-facing diagnostics (`Box__unique_Itemptr`)

Filed 2026-07-31 while fixing `unique-ptr-field-stack-address-aborts-silently`. Surfaced by
that fix's error test, but **not caused by it** - the mangled owner name comes from
`nv.OwningStructName`, which has always held the monomorphized symbol.

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

## Why it is worth a file, beyond aesthetics

An `expect_error` test that pins such a message is pinned to the MANGLING SCHEME, so any
change to `MangleTypeArg` silently breaks tests that have nothing to do with mangling. When
this was found, the proposed leg would have been the only `expect_error` string in all of
`Test/errors/` containing a `__` mangled name. It was instead pinned to the stable prefix
(`... into unique field 'Box`), which keeps the suite robust but leaves the diagnostic itself
unfixed. **Prefer prefix-pinning over pinning a mangled name** in any new test until this is
fixed.

## Why it was not fixed in that round

Investigated and deliberately deferred - the fix is not contained:

- No demangler exists anywhere in the codebase (no `Demangle` / `UnmangleGeneric` /
  `PrettyTypeName` helper).
- `MangleTypeArg` (`cflat/MainListener.h:225`) is a lossy ONE-WAY transform: `Item*` becomes
  `Itemptr`, a `unique ` prefix is folded in, and args are `_`-joined. There is no inverse,
  and a reverse-parse would misfire on any type name that itself contains `_`.
- `StructData` stores no source spelling to recover.

## Fix direction

Do NOT write a reverse-parser. Store the SOURCE SPELLING at instantiation time - add a field
to `StructData` carrying the as-written generic name (`Box<unique Item*>`) alongside the
mangled symbol, and have the diagnostic helpers prefer it.

**If you add such a field and any analysis reads it, CLAUDE.md's `--init` rule applies**: it
must be added to the cache round-trip in `cflat/LLVMBackend.cpp` in the same change, or it is
silently dropped on a warm cache.

This affects every diagnostic that names a generic owner, not just the `unique` family, so it
is worth doing once centrally rather than per-message.

## Test coverage

Indirect: `Test/errors/err_unique_stack_address.cb` currently prefix-pins to avoid the issue.
Once fixed, that leg can pin the source spelling instead.

Related: [[interface-issue-queue]]
