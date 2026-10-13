# [cpp] struct identity canonicalization misses namespaces ending in `_` (and two CRTP leftovers)

Found by the N51 Sol review (master identical). Repros: scratch/repro_keep/n51_followups/.
2. An imported global C++ class GlobalSelf and a [cpp] GlobalSelf collide: the generated class loses weak_from_this (global_collision.cb).
3. A CRTP base method reading D.value is instantiated against a generated self declaration lacking the field (nested.cb + nested.h); clang accepts.

Status 2026-10-10 (T94): item 1 fixed (CanonicalGeneratedCxxIdentity decodes by the generator's
encoding; namespace_underscore.cb now runs). Items 2 and 3 not attempted in T94 (optional, no
one-site cause established); still open.
