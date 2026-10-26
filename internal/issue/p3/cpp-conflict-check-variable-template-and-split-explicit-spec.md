# CFlat struct wrapper names can collide

The optional T436 neighbor remains: `[cpp]` CFlat struct names `ns_.Self` and `ns._Self` both
encode to `__cflat_user::ns___Self`. The `b8_ambig_lower` repro then refuses use of the second
type through `weak_ptr`.

Root cause: `CppStructCxxName` replaces `.` with `__` but leaves underscores unchanged;
`CppStructThunkStem` uses the same non-injective mapping for generated thunk names.

Fix direction: use an injective encoding for every namespace and type-name component, and apply it
consistently to the wrapper type and thunk stem. Add a value leg that names both types and uses
the second through `weak_ptr`.
