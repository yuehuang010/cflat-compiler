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
