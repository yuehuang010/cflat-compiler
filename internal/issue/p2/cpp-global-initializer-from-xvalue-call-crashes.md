# Global C++ class variable initialized from a C++ call result crashes the compiler

Found by the ST8 review (2026-10-01), pre-existing on master 1cfe8a3b. Repro kept in
scratch/repro_keep/st8_preexisting/ (rv.h + global_init.cb / global_auto.cb).

```cflat
import cpp {"rv.h", "utility"};
rv.R src = default;
rv.R glob = std.move(src);      // also: auto glob = std.move(src);
extern int main() { return glob.get() == 7 ? 0 : 1; }
```

Observed: cflat exits 139 ("internal compiler error") on master and on the std::move branch.
Expected: a global initialized from a C++ call result is constructed by a dynamic initializer, like
clang++ (move-construct from src, which is left moved-from); or, if CFlat globals must be constant
initialized, a clean LogError at the initializer. Check the global-static storage ruling
(owning types legal, implicit consume an error, `move` re-inits) before choosing.
Check whether any C++ call result (not just T&&) in a global initializer crashes the same way.

## Status 2026-10-05: T45 PARKED after the final Sol review (round 2)

Branch fix/t45-globalinit (worktree cflat-fix-t45-globalinit, commit ddee8f86) fixes this issue, the enum-initializer
crash and the qualified-enumerator case/if-const gap, but the final review found two REGRESSIONS vs master:
`static auto g = std.move(s);` lands on stack storage and is destroyed per call (clang counts 2 0 1 2, branch 2 0 1 4);
`enum F:int { V = true ? 7 : 8 };` is rejected (scratch function lowers ?: as branch/PHI; TryFoldConstInt folds Select
only). Plus an incomplete side-effect check (store into an EXISTING local alloca from if const / case; master too).
Review: worktree scratch/briefs/t45_review2.md, probes scratch/rev_t45_r2/. Next: fix the two regressions (keep the
static request when deducing auto; constant-context ?: stays a Select), then one review round and land.
