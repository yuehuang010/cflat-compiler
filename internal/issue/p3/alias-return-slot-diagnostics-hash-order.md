# CheckAliasReturnSlotAddressTaken reports in hash-map order

PRE-EXISTING (found by T109 review 1, 2026-10-11). `CheckAliasReturnSlotAddressTaken`
(cflat/LLVMBackend_CInterop.cpp ~885) iterates `functionTable` (an unordered_map) to emit its
diagnostics, so with two offending functions the reported order depends on the standard library's
hash order (differs between macOS libc++ and MSVC). T109 fixed the same pattern for deferred
compile_error diagnostics (sort: live > has-site > first-call order > name).

Fix direction: collect the offenders, sort them by source location (or the T109 key), then report.
Acceptance: a Test/errors leg with two offending functions whose expected first error is the
earlier one in the file passes on both hosts.
