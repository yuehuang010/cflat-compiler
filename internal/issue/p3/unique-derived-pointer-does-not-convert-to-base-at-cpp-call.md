Bucket: p3 (C++ interop; found by the R3 move-into-callee fix; workaround: declare the holder as the base type)

# `move u` of a `unique<Derived*>` into an imported C++ `Base*` parameter finds no matching overload

Found 2026-09-21 by the fix on fix/cpp-move-into-callee (R3: `move p` transfers ownership into a
raw-pointer parameter of an imported C++ function). Measured there: an exact `unique<Base*>`
holder releases and destroys exactly once; a `unique<Derived*>` passed as `move u` to
`destroy_through(Base* p)` reports "no overload matches" - the derived-to-base conversion that
the raw `Derived*` path applies (`destroy_through(move m)` with `Derived* m` works) is not
applied when the released raw pointer comes out of a unique holder.

Fix direction: in the C++ call argument path, after the unique holder is released to its raw
pointer for a `move` argument, run the same derived-to-base pointer conversion the raw-pointer
argument path uses before overload matching. Acceptance: a bridge leg with `unique<Derived*> u`
moved into `destroy_through(Base*)` destroys once and exits 0; `use of moved variable` afterwards.
