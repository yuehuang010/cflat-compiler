# Imported union cannot refer to a later holder type

Summary: A union in an imported file cannot name a holder declared later in the importing file; forward struct declarations also fail to parse.
Repro: Imported union names `XLater`, which is defined after the import; `struct Never;` forward declaration.
Master vs expected: master reports `cannot find the type XLater` and rejects the forward declaration; expected cross-file forward resolution and declaration support.
Found: T440 r2 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX8): the cross-file part is a general import-order gap, not union-specific. Imports are compiled
(CompileImportedFile) before the importing file's forward-ref pre-scan (LLVMBackend.cpp, import loop precedes
"forward-ref scan"), so the importer's struct/union names are unknown there. Reproduced for a union by-value
and pointer field, and for a struct by-value and pointer field (scratch/mix8_tmp/u). Same-file forward use works.
Not fixed: pre-registering importer shells would give an EMPTY layout to by-value uses; needs a deferred-layout
design. The `struct Never;` forward-declaration syntax is left for a maintainer ruling (not added).

RULED 2026-10-31 (maintainer): KEEP REFUSING, no new syntax. An import is a module, not a textual include: an imported file cannot see
the importer's declarations, later or earlier (C header model); the located "cannot find the type XLater" error stands. C's
`struct Never;` forward declaration is NOT added - same-file forward use already works without it (and the same-file by-value
`list<Later>` field is ruled legal in list-template-later-types-unresolved.md). Test consequence: none; the cross-file shapes may
get an err_ leg pinning the located error. Closed as intended.
