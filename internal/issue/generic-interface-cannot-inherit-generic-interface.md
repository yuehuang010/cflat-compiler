# A generic interface inheriting another generic interface fails at the declaration

Filed 2026-07-29 verifying the in-progress generic-interface fix on `x64/Release/cflat`
(the binary with the registration-as-opaque-struct fix applied, not the preserved
pre-fix copy at `scratch/gi/cflat_master`). Existing repro:
`scratch/gi/probe_15_iface_to_iface_as_downcast.cb` (read, not modified).

Severity: FALSE REJECTION. A legal-looking source shape is rejected outright with a
diagnostic that gives the user nothing to act on (`unknown parent interface: 'IBase'`,
naming an interface that IS declared in the same file). No miscompile - the program
simply never compiles, so there is no wrong runtime value to observe.

## Repro

```cflat
interface IBase<T> { T Get(); };
interface IDerived<T> : IBase<T> { void Set(T v); };
class Impl<T> : IDerived<T> { T data = default; T Get() { return data; } void Set(T v) { data = v; } };

extern int main()
{
    Impl<int> im = default;
    im.Set(9);
    printf("%d\n", im.Get());
    return 0;
}
```

Verified against `x64/Release/cflat` (this session's binary, containing the in-progress
generic-interface fix):

```
probe6_gen_extends_gen_impl_no_downcast.cb(4,44): unknown parent interface: 'IBase'
```

Same message from the original downcast repro (`probe_15_iface_to_iface_as_downcast.cb`),
run with `-i Test` (its `test_helper.cb` import lives directly under `Test/`, not
`Test/library`):

```
probe_15_iface_to_iface_as_downcast.cb(5,35): unknown parent interface: 'IBase'
```

## What was narrowed (all run against `x64/Release/cflat`, files under `scratch/newissues/`)

| # | Shape | File | Result |
|---|---|---|---|
| 1 | Generic interface inherits generic interface, DECLARED ONLY, never instantiated | `probe5_gen_extends_gen_baseline.cb` | PASSES |
| 2 | Same, instantiated via a plain implementing class (no downcast) | `probe6_gen_extends_gen_impl_no_downcast.cb` | FAILS, same message |
| 3 | Same, with `IDerived<T>` declared textually BEFORE `IBase<T>` | `probe4_gen_extends_gen_reorder.cb` | FAILS, same message (order-independent) |
| 4 | Generic interface inherits a NON-generic interface (`IDerived<T> : IBase`) | `probe2_generic_extends_nongeneric.cb` | PASSES |
| 5 | NON-generic interface inherits a generic interface INSTANTIATION (`IDerived : IBase<int>`) | `probe3_nongeneric_extends_genericinst.cb` | FAILS, same message |
| 6 | Same as #5, with `IBase<int>` forced to instantiate earlier via an unrelated class | `probe7_nongeneric_extends_genericinst_reorder.cb` | FAILS, same message |

So the failure needs two things at once: the PARENT is a generic interface, and the
CHILD relationship is ever actually instantiated with concrete type arguments (a bare
uninstantiated declaration containing the base-clause text is fine - #1). Declaration
order does not matter (#3, #6). Whether the child interface itself is generic or not
does not matter (#5 fails the same way as the primary repro) - what matters is that the
PARENT reference needs its own type arguments substituted/mangled and isn't getting them.

## Root cause

Not fully diagnosed, but the resolution site and a strong lead were verified by reading
the code (not by a source-level fix attempt):

- The error is thrown at `cflat/LLVMBackend.h:9235`, inside `CreateInterfaceDefinition`:
  it looks up each `parentName` in `interfaceTable` (`LLVMBackend.h:9232`) and logs
  `"unknown parent interface: '{}'"` on a miss.
- The parent names it is given come from `BaseSpecifierName()` (`cflat/MainListener.h:465`),
  which by design "spells [the base-clause name] without its generic type arguments" -
  `IBase<T>` yields the bare string `"IBase"`, never `"IBase__int"` or any mangled form.
- Two call sites feed `CreateInterfaceDefinition` this bare name without ever
  reapplying substitution/mangling to a parent that itself needs instantiating:
  - `InstantiateGenericInterface` (`cflat/MainListener.h:3859-3861`), used when the
    interface ITSELF is a generic template being instantiated (repro #2, #3).
  - `ScanInterfaceDefinition` (`cflat/MainListener.h:1655-1657`), used for a NON-generic
    interface whose base clause names a generic instantiation like `IBase<int>` (repro #5).
- Both sites end up looking `interfaceTable` up for the bare template name `"IBase"`,
  which is never registered there (only mangled instances like `IBase__int` are, once
  instantiated) - consistent with every observed failure and with #1 passing (no
  instantiation ever reaches either site).

This is inferred from reading the two call sites and the shared helper, not confirmed by
a source-level fix-and-retest; treat it as a strong lead, not a proven root cause.

## Fix direction

At both call sites, when a base-clause parent name resolves to a generic interface
template, apply the CURRENT instantiation's type substitutions to the parent's type
arguments and mangle to the parent's concrete name (`IBase` + `<T>` under `T=int` ->
`IBase__int`) before looking it up / passing it to `CreateInterfaceDefinition`, and queue
the parent's own instantiation if it has not happened yet (mirroring how
`ProcessPendingInstantiations` already drains interface instantiations before struct
layouts, per `generic-interface-registered-as-opaque-struct`). The non-generic-parent
case (repro #4) already works, so whatever the fix is, it must not touch the plain
bare-name path.

## Variance

Not checked at `-O0`/`-O2`/`-g` separately - the failure is a source-level rejection
before codegen, so it is expected to be identical across those; not empirically verified.
