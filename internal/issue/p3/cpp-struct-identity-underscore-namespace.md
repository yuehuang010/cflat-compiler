# [cpp] struct identity canonicalization misses namespaces ending in `_` (and two CRTP leftovers)

Found by the N51 Sol review (master identical). Repros: scratch/repro_keep/n51_followups/.
1. `namespace ns_ { [cpp] struct Self : std.enable_shared_from_this<Self> ... }`: explicit `std.weak_ptr<ns_.Self>` local refused ("no destructor"), auto works (namespace_underscore.cb + .cpp). CanonicalGeneratedCxxIdentity (LLVMBackend_CInterop.cpp) decodes generated `ns___Self` as `ns._Self`. Fix: match the encoded identifier against registered [cpp] names with the generator's exact encoding instead of guessing separators from `__`.
2. An imported global C++ class GlobalSelf and a [cpp] GlobalSelf collide: the generated class loses weak_from_this (global_collision.cb).
3. A CRTP base method reading D.value is instantiated against a generated self declaration lacking the field (nested.cb + nested.h); clang accepts.
