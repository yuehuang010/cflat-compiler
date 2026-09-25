# After an incomplete-element std.map refusal, a later valid std.map blames the import

Bucket: p3 (misleading follow-on diagnostic after a first error; no wrong code). Found 2026-09-24
reviewing the incomplete list/map crash fix (before that fix this program crashed).

## Summary

When `std.map<int, Leaf>` is refused because `Leaf` is not yet defined (caught by expect_error),
a later `std.map<int, Leaf>` field declared after `Leaf` is defined reports "no imported C++
header declares 'std::map'" instead of binding. Only reachable after a first error in the same
compile, so it affects expect_error files and error cascades, not valid programs.

## Repro

scratch/p2repro/p3_refused_then_complete.cb (header Test/library/cpp_interop_tpl.h).

## Fix direction

The failed explicit-instantiation request likely leaves a negative cache entry keyed on the
template name rather than the full specialization; key the refusal on the specialization, or
drop it once the incomplete record completes. Measure first.
