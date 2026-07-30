# A generic struct and a generic interface may share a name with no diagnostic

Filed 2026-07-29 verifying the in-progress generic-interface fix on `x64/Release/cflat`.

Severity: no miscompile demonstrated - every probed shape either produces the correct
value or a clean compile-time rejection. This is an ACCEPT-SET problem: an undocumented,
unenforced "struct wins" tiebreak silently resolves what should arguably be a name
collision, with no diagnostic at either declaration site.

## The tiebreak, as it exists today

`Test/test_generics.cb` declares both under the bare name `Container<T>` and the file
still passes in full (`77/77 tests passed`, verified this session against
`x64/Release/cflat`):

- `struct Container<T>` at line 21 (verified: `grep -n "^struct Container<T>"`)
- `interface Container<T>` at line 204 (verified: `grep -n "^interface Container<T>"`)
- `Container<int> intContainer = default;` (struct use) at line 110
- `class Storage<T> : Container<T>` (interface use, as a base clause) at line 216

The tiebreak lives at `cflat/LLVMBackend.h:9350-9355`:

```cpp
// True when 'name' names a generic INTERFACE template. A generic struct/class template of the
// same name wins (same precedence ProcessPendingInstantiations applies).
bool IsGenericInterfaceTemplateName(const std::string& name) const
{
    return gts.genericInterfaceTemplates.count(name) != 0
        && gts.genericStructTemplates.count(name) == 0
        && gts.genericClassTemplates.count(name) == 0;
}
```

and is consumed in `ProcessPendingInstantiations` (`cflat/MainListener.h:23482` on, the
struct/class-vs-interface branch at `23499-23538`): a pending instantiation of a name
present in BOTH `genericStructTemplates`/`genericClassTemplates` and
`genericInterfaceTemplates` is always resolved as the struct/class. Both maps are
populated unconditionally with no collision check - `genericInterfaceTemplates[name] = ctx`
at `cflat/MainListener.h:2079` and `3766`, `genericStructTemplates[structName] = ctx` at
`23716`, `genericClassTemplates[structName] = ctx` at `26112` - none of the four
assignment sites checks whether another map already holds the same key.

**This tiebreak was ADDED by the in-progress generic-interface fix.** Before it, the
same collision was handled only incidentally (whichever map's instantiation path
happened to run reached for the name first); the deliberate, named precedence rule in
`IsGenericInterfaceTemplateName` is new.

## What was probed (all under `scratch/newissues/`, run against `x64/Release/cflat`)

| # | Shape | File | Result |
|---|---|---|---|
| 1 | struct declared before interface (matches `test_generics.cb` order) | `probe8_dup_struct_first.cb` | Compiles and runs; `Container<int>` resolves as the struct (`data`/`count` fields readable) |
| 2 | interface declared before struct (order reversed) | `probe9_dup_interface_first.cb` | Same result - struct still wins. The tiebreak is name-table-based, not declaration-order-based |
| 3 | both declared, PLUS a class implementing the interface via base clause, all in one file | `probe10_dup_interface_only_use.cb` | Compiles and runs correctly - the base-clause use of `Container<T>` resolves to the INTERFACE even though the bare-name/variable-decl use resolves to the STRUCT. Both coexist, each in its own context |
| 4 | struct wins, then call an interface-only method (`Set`) on a struct-typed variable | `probe11_dup_direct_interface_use.cb` | Clean rejection: `Unknown identifier 'Set'.` - proves the struct really won (no method-set leakage), not a miscompile |
| 5 | struct + interface declared in an IMPORTED file, implementing class in the importing file | `probe12_lib.cb` / `probe12_dup_different_files.cb` | FAILS with a confusing, unrelated-looking message: `cannot cast an aggregate value - a fixed array decays to a pointer to its first element`. Control: the same interface alone (no struct collision), split across the same two files, compiles and runs fine (`probe12b_lib_iface_only.cb` / `probe12b_main_iface_only.cb`, prints `iface=9`) - so the cross-file split itself is NOT the problem; the collision behaves differently, and worse, once the two declarations are not in the same file |
| 6 | struct + interface + implementing class, all inside one `namespace` block | `probe13_dup_namespace.cb` | FAILS: `unknown type 'boxes.Container__int'`. This matches the root cause consolidated in `generic-template-namespace-key-space.md` (generic templates have a namespace-blind key space) - not a new finding, just consistent with it |
| 7 | GENERIC interface `Box<T>` vs a NON-generic struct `Box` (different arity, same bare name) | `probe14_nongeneric_struct_vs_generic_interface.cb` | Compiles and runs correctly; both resolve independently (`tag=1 iface=3`) - `IsGenericInterfaceTemplateName` only consults the GENERIC maps, so a non-generic struct of the same name never blocks the generic interface |
| 8 | GENERIC struct `Box<T>` vs a NON-generic interface `Box` | `probe15_generic_struct_vs_nongeneric_interface.cb` | Compiles and runs correctly, same reasoning in reverse (`data=4 tag=8`) |

No shape produced a wrong VALUE - every failing shape failed at compile time with a
diagnostic (some clean, some confusing per #5), and every passing shape produced the
values the source asked for. Severity is therefore NOT raised to a miscompile; #5 is
flagged as a secondary oddity worth a look (a name collision that is silently resolved
in one file layout but produces a nonsense diagnostic when split across files), but it is
still a false-rejection-shaped failure, not a wrong-answer one.

## Fix direction

Probably a hard `LogError` at the SECOND declaration of a name already claimed by the
other generic-template kind (mirroring the existing non-generic interface collision
guard in `CreateInterfaceDefinition`, `cflat/LLVMBackend.h:9183-9201`), rejecting the
collision outright instead of resolving it by an undocumented precedence rule.

**Blocker, stated explicitly**: `Test/test_generics.cb` relies on exactly this collision
today (struct `Container<T>` at line 21, interface `Container<T>` at line 204, both
exercised) and would need one of the two renamed before a collision `LogError` could
ship - this cannot be done without touching that test, which is out of scope for this
issue.

## Variance

Not checked at `-O0`/`-O2`/`-g` separately; every probe here fails or succeeds at
source/name-resolution time, before codegen, so no optimization-level dependence is
expected but this was not empirically verified across levels.
