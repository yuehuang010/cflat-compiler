# In-repo C++ `enum class` enumerators are usable without qualification

A scoped C++ enum imported from an in-repo header lets its enumerators be named bare (`bad`) instead of
`E.bad`; C++ requires the qualification for `enum class`. Accepting it widens what compiles, so tightening it
changes behaviour and needs a maintainer ruling on the surface (refuse vs keep as a CFlat convenience).

## Repro

scratch/repro_keep/t34_enum_class_unqualified.cb (copied from the T34 worktree): an `enum class` in a Test/library-style header, a bare enumerator use compiles.

## Fix direction

If ruled "follow C++": unscoped lookup must skip enumerators of scoped C++ enums (both ParseDeclarationSpecifiers
copies untouched - this is expression-name lookup). Needs an expect_error leg.

Found by: T34 (2026-10-03).
