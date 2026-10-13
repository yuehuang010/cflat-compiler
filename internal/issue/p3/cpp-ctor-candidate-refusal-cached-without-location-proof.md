# Constructor-candidate wrapper refusals are cached without a location proof

Found by T108 review 2 (narrowed remainder of the retired cpp-wrapper-rejection-cache-keyed-by-text-only).

## Summary
T108 caches a NEG wrapper result only when every clang error maps into the wrapper source, except
`__cflat_ctor_` candidate wrappers (LLVMBackend_CInterop.cpp ~9693 and ~9723): those refusals are
cached even when the error lies in a header (direct or transitive). Header EDITS are safe - the key
carries the include-closure hash (`|C`) and the header stamp (`|T`), probes C1/C2 in
scratch/repro_keep/t108/ re-evaluate after the fix (value 7). What stays open is the original
concern for this one class: a change in TU context or in compiler behaviour (a later cflat fix that
makes the candidate viable) replays the stale refusal until the cache is cleared. The exemption
stays because the warm budget depends on it (bridge fixture: 96 NEG stores, warm header parses 1).

## Related timing note (same review)
The clang-buffer hash time `ht` is stamped late: CollectHeaderNamespaceNamesParallel
(LLVMBackend_CInterop.cpp ~389) at scan time and `hashNow` (~25503) after hashing, while clang read
the bytes earlier. An edit in that gap whose mtime lands > 2 s before `ht` is trusted. Capture the
time once before ExtractCHeaderClang (or before each read). Same hole existed before T108.

## Fix direction
Key ctor-candidate NEG entries on the compiler build stamp (see the cache build-stamp keying ruling,
default OFF until cold <= 1.1x clang), or prove the error location like other wrappers and measure
the warm-budget cost. Fix the `ht` capture point independently.
