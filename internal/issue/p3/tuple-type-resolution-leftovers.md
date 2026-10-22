# Tuple type resolution leftovers after T369 (nested tuples landed)

Found by the T369 review (2026-10-20). All PRE-EXISTING on master; probes in the T369 worktree scratch/rev_t369_opus_cases/
(copied to scratch/repro_keep/box1021/t369_rev/, gitignored).

1. Tuple inside a function type: `function<int((int,int))>` -> "cannot find the type '(int,int)'" (nested form the same).
   functionPointerParam resolution does not route tuple specifiers through the shared tuple resolver.
2. Tuple naming a generic parameter in a generic body: `(int,T)` -> "cannot find the type 'T'" (substitution not applied
   before tuple-shell registration).
3. Generic struct as a tuple element: `(list<int>,string)` -> "incomplete"; the nested form now says "cannot find the type
   'list<int>'" (arguably worse text after T369).
4. Unqualified sibling enum inside a namespace tuple: `(Late,int)` -> "cannot find the type 'Late'".
5. `Box<K*>` with a function-local enum K -> "cannot find the type 'K'" (not tuple-specific; generic arg + local enum pointer).

Fix direction: one shared tuple-element resolver that takes the current substitution map, namespace and local-type scope,
used by declarations, generic args and function-type params in both passes. Acceptance: each row compiles and runs with the
clang struct-oracle layout; T369 legs unchanged.
