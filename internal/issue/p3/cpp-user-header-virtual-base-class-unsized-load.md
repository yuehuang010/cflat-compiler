# A class with a virtual base from a user (non-system) C++ header fails module verification on base conversion

A reference result of a class that has a virtual base, imported from a NON-system header (eagerly
registered), reaching a base-reference conversion emits `load %ns.T, ptr ...` of the unsized opaque
type -> "Module verification failed" instead of a diagnostic. The same header seen as a system header
(`#line 2 "r4b_system/r4b.hpp"`) binds and returns clang's value (742). Pre-existing (master fails the
same way); found by the T26 round-5 review, 2026-10-03.

## Repro

`scratch/repro_keep/rev_t26_r4b.hpp` (+ `rev_t26_r4b_list.txt`, `rev_t26_r4b.sh` - adjust the `cd`):

```cflat
import cpp "rev_t26_r4b.hpp";   // struct plain final : virtual V { ... }; inline plain& held_plain();
extern int main()
{
    r4b.sink2 s2 = default;
    int x = s2.read(r4b.held_plain());   // expected 742 (clang); actual: Module verification failed
    printf("%d\n", x);
    return 0;
}
```

Also: an `expect_error` block around the non-final user-header case still exits 1 after printing PASS.
Related (unfiled earlier, T26 round 1): a C++ inline global of a class with a virtual base -> "loading
unsized types".

## Root cause (GUESS)

User-header records with virtual inheritance are registered with an opaque layout ("uses virtual
inheritance, which is not supported yet"), but the reference-result / base-conversion path still loads
the value by type.

## Fix direction

Either lay the record out from clang's record layout as the system-header path does, or LogError with
the opaque-layout reason wherever such a value would be loaded - never emit the unsized load.
