# CFlat aggregates with nontrivial C++ fields still relocate by value

Found 2026-09-24 during round 2 of the C++ struct relocation investigation.

## Remaining cells

The retained `[cpp]` constructor-thunk fix makes the three `[cpp]` default-construction spellings correct. The plain CFlat constructor path is unchanged from baseline. The following measured cells remain wrong:

- Plain CFlat `Plain h = Plain()`, `Plain h;`, and `Plain h = default;` with a `std.list<int>` field return 134 instead of 52. Front-only, back-only, and size-only probes also return 134.
- Plain CFlat assignment `h = Plain()` after `h = default` returns 134.
- A `[cpp]` `Holder` returned from `make()` into a local returns 134.
- The measured move-return / by-value transfer paths for `Plain` and `Holder` return 134. `consume(make())` without an explicit move remains rejected because the prvalue is not addressable for copy construction.

The value tests for corrected `[cpp]` construction forms are in section 3580-3589 of `Test/test_cpp_interop_bridge.cb`; no tests are added for these still-wrong cells.

## Measured root cause

The C++ field default constructor now runs at the field address in the generated `[cpp]` constructor thunk. Plain CFlat aggregate default construction remains broken as measured at baseline. Separately, CFlat aggregate return and move paths bit-copy the enclosing aggregate, including the self-referential `std.list` field. In `scratch/round2-plain-list.ll`, the constructor is `define internal %Plain ...`, constructs the list through a pointer to `%cflat_ctor_result`, loads its three words, and returns `%Plain` by value; the caller receives that aggregate with a `call %Plain`. `scratch/round2-holder-return.ll` shows the corresponding `Holder` return and transfer path.

## Maintainer decision needed

Choose one design before extending this fix:

A. A CFlat struct containing a non-trivially-relocatable C++ field is constructed in place, returned via sret, and moved field-by-field with the C++ move constructor.

B. Refuse such a field with a diagnostic until design A exists.

Neither option is selected in this issue.
