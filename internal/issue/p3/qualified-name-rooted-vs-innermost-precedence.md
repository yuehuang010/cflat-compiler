# Qualified name precedence: rooted first vs C++/C# innermost-first

- Current rule (kept by the qualns fix): a dotted name `B.Late` used inside namespace `A` resolves ROOTED first (`::B.Late`);
  only on a miss does it try enclosing prefixes (`A.B.Late`). Pinned by `Test/test_generics.cb` `ta_dotted_arg_same_as_local` (909)
  and the rooted-wins controls in `Test/test_basic.cb`.
- C++ and C# resolve the first qualifier innermost-scope first (`A::B` hides `::B`). Flipping CFlat to that order changes which
  declaration existing programs bind to, so it needs a maintainer ruling; until then rooted-first stands.
- Acceptance if ruled innermost-first: flip the rooted-wins controls in test_basic and the 909 leg in test_generics, same change.
- Declaration order (pre-existing, review 2026-11-01): a ROOT namespace's functions / globals / enums declared AFTER the use are
  invisible from inside another namespace (same failure on master), so `B.g()` inside `A` binds `A.B.g` when root `B` comes later
  and root `B.g` when it comes earlier. Types are order-independent (root `B.Late` declared later still wins). Repro
  scratch/repro_keep/qualns/rev_qualns_c08_late_rooted_fn.cb. `global::B` is root-only (never the enclosing fallback).
