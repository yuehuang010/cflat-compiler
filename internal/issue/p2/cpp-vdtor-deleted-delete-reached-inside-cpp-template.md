# C++ vtable with an unusable operator delete reached only inside C++ code crashes (139)

Severity p2 (compiler crash on code clang refuses). Found by the T344 r3 review (2026-10-20); pre-existing on master.

## Repro
Repros in the T344 review folder (copied to scratch/repro_keep/box1020/t344_followup/): holder_deleted.cb, holder_ambig.cb,
cpptpl_local.cb, cpptpl_val.cb against Test/library/cpp_vdtor_opdelete_template.h.
- `r3.Holder<cppvdtemplate.Deleted<char>> h;` as a CFlat local (Holder has a by-value member of the bad template)
- the Holder<Ambiguous<char>> variant
- `r3.Build<Private<char>>()` / `r3.MakeVal<Deleted<char>>()` - a C++ template constructs the object
Master and T344: exit 139 cold+warm; clang++ -std=c++20 refuses each. Scoped expect_error forms fail (no error reported).

## Root cause
The T344 guard (CClangExtract.cpp ~7880, constructor-use verdict) checks only the record that owns the demanded constructor. The inner
Deleted<char> vtable is pulled in later by clang codegen of the outer constructor / C++ template body and is never validated.

## Fix direction
Validate transitively: when clang codegen of a demanded constructor (or C++ function body) requires a vtable, run the same delete verdict
for that record (e.g. hook the vtable-use / MarkVTableUsed point in the incremental group), report clang's text at the CFlat use site,
mark the dtor invalid so EmitDeferred never emits the deleting dtor. Add legs to Test/errors/err_cpp_vdtor_template_deleted_delete.cb.
