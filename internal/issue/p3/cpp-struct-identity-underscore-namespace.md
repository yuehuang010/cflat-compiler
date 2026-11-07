# [cpp] struct named like an imported global C++ class (collision)

Status 2026-11-05 (nslookup r4): item 1 (namespace ending in `_`) fixed earlier; item 3 (CRTP base reading
a field of the generated self) fixed, see Test/test_cpp_interop_template.cb (`CrtpFieldSelf`); fields named
after imported macros (errno, EOF, stdout) stay opaque (`CrtpMacroFieldMix`). Only this item remains.

## Repro

`scratch/repro_keep/n51_followups/global_collision.cb` (needs `fixture.h`, not in that folder; the
reconstruction is `struct GlobalSelf {}` plus a `counts` namespace with a `Tick` RAII counter).

    import cpp "fixture.h";
    [cpp] struct GlobalSelf : std.enable_shared_from_this<GlobalSelf> { counts.Tick tick = default; int value = default; };

Master: `could not be bound (clang: use of undeclared identifier '__cflat_user')` (base spelled with the
import). Pre-fix probe `struct GlobalSelf { int value = default; }` plus `import cpp "fixture.h"` and
`GlobalSelf g = default; g.value = 41;`: `the C++ class's import group is unavailable`.

## Oracle

clang++ -std=c++20 (scratch/nslookup_tmp/oracle.cpp): the imported `::GlobalSelf` and the generated
`__cflat_user::GlobalSelf` coexist and both values check out (41, 61, 73, 89). Two spellings, no ODR clash.

## Root cause (verified)

- The import's C++ record `GlobalSelf` is registered during the code-gen walk, when `import cpp "fixture.h"`
  is processed. The [cpp] struct's declaration comes later in the same walk, so it reuses the import's
  dataStructures entry (CreateStructType finds it and adds no `value`). Later lookups of `g.value` fail.
- The import's class info (cxxClasses_, cxxRecordEntries_, cxxRecords_, cxxTypeOwnerGroup_, the reverse
  spelling map) is keyed by the same CFlat name, so ctor/dtor binding and spelling also resolve to the import.
- The base spelling `std::enable_shared_from_this<GlobalSelf>` comes from CxxSpellingForCflatType checking the
  reverse-spelling map before IsCppStructName.
- Narrower claim: a CFlat declaration does not shadow a same-named import when it is a `[cpp] struct`.
  The same name as a plain `struct` does shadow: `struct GlobalSelf { int value = default; }` declared after
  or before `import cpp` of a global `struct GlobalSelf` prints 41 on master and on this branch
  (gs_plain.cb, gs_before.cb). Only the `[cpp] struct` form hits "the C++ class's import group is unavailable"
  (gs_cpp.cb, master and branch alike).

## Attempts (all failed; none landed)

1. CxxSpellingForCflatType: IsCppStructName before the reverse-spelling map. Base spelled as the generated
   class; `p->value` / `owner->value` still "Unknown identifier 'value'".
2. Erase cxxCflatToCxxSpelling_[name] at the [cpp] declaration. No change.
3. Erase cxxClasses_ / cxxRecordEntries_ / cxxRecords_ [name] at the declaration. Ctor binding passes; field
   lookup still fails (dataStructures entry is the import's).
4. Erase dataStructures[name] at the declaration. No change (the import re-registers before the declaration).
5. Skip the import's record in RegisterCRecords pass 1 when IsCppStructName(name) and no generated record
   exists, plus attempt 3. Breaks the generated struct's own layout ("incomplete here", "no bound
   destructor", "cannot declare a local of C++ class") - the class info is still resolved by name somewhere else.

## Direction (decided by the coordinator, round 4)

Coexist, as clang does (`::GlobalSelf` and `__cflat_user::GlobalSelf` both exist, oracle value 41) and as a
plain CFlat `struct` already does on master. A refusal would make `[cpp] struct` stricter than plain `struct`.
The implementation is a separate identity-table change: the import needs its own key for the CFlat tables
(cxxClasses_, cxxRecordEntries_, cxxRecords_, cxxTypeOwnerGroup_, reverse spelling map, dataStructures) so that
a `[cpp] struct` of the same name owns the bare CFlat name. Not built in this landing.

## Legs

None yet. Expected: a value leg with the collision fixture (`owner->value == 41`, weak_ptr counts and the
Tick destructor count), plus a unique-name twin as accept-set. The plain-struct precedent (gs_plain.cb) is the
accept-set for the bare-name behaviour.
