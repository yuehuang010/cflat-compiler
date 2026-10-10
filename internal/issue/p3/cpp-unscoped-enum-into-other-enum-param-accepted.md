# Unscoped C++ enum value into a parameter of a different enum type is accepted

From the T40/T49 notes (2026-10-05); T53 verified it untouched. A value of unscoped C++ enum `U1` binds
a C++ parameter of enum type `U2`; clang++ refuses (no implicit enum -> other-enum conversion). A
read-only leg in Test/test_cpp_interop.cb (was near line 1627 on 2026-10-05; line has drifted - find
the U1/U2 enum-param leg) pins it as compiling.

## Ruling (maintainer, 2026-10-09)
CFlat needs an explicit cast. Enums are type-safe: no implicit conversion from one enum type to
another, C++ unscoped enums included (`p((U2)u1)` is the spelling). Authorized: rewrite that leg to
the cast form and add the refusal as an expect_error leg in an existing Test/errors file.
Related, native side: p3/same-named-local-enums-in-two-functions-collide (`G g = E.V;`, `E e = 5;`
pass --check) - this ruling's type-safety reading covers enum -> other enum there too.
