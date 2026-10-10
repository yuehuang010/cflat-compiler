# Cached clang rejection of a generated C++ wrapper is keyed by the wrapper text only

Found by T58 round 3 (pre-existing design).

## Summary
When clang rejects a generated wrapper (`__cflat_tpl_<hash>`), the rejection is cached by the
wrapper source text. Whether clang rejects it can depend on what else is already in the shared
wrapper TU (T58 r3: a brace selector `__cflat_bsel_<hash>` emitted twice -> "redefinition of
el<std::initializer_list<T>>"). After the TU-context bug is fixed, a warm cache written by the old
compiler keeps replaying the stale error until the cache is cleared.

## Fix direction
Key cached rejections by the compiler build stamp too (see cache build-stamp keying ruling
2026-09-28), or only cache rejections whose clang diagnostic points inside the wrapper's own text.
Repro shape: scratch/repro_keep/t58_rev/r3/ (r.cb).
