# Assignment through a C++ `const T&` / `const T&&` call result is accepted (ruled: refuse)

`s.const_member_ref() = 32;` where the C++ member returns `const long&` compiles and writes the
referent; C++ rejects it (ill-formed). Same for a free function returning `const T&`, and (since the
T&& return fix, fix/rref-return) for `const T&&` scalar results (`cppi.rref_const_long() = 1;`,
`o.const_member_long() = 1;` in Test/library/cpp_interop_basic.h). Found 2026-09-27: the rref-return
fix first added a refusal keyed on IsAlias && IsCxxConstRef; its review showed that also refused
the const T& member shape master accepted, so the refusal was removed pending a ruling.

## Question for the maintainer

The 2026-08-26 ruling leaves CFlat `const` unenforced, but C++ const namespace objects ARE refused
(err_cpp_namespace_const_assign.cb, "it is a const C++ object"). Should writes through a C++ const
reference RESULT be refused the same way (C++ parity, bridge safety), for const T& and const T&&
alike, free and member?

## Fix direction if refused

The assignment path in MainListener_Expressions.cpp (after `ParseUnaryExpression(unaryCtx)` in the
assignment handler): refuse when the target is IsAlias && IsCxxConstRef and came from a C++ call
result; also compound `+=` and postfix `++`. Check the IsCxxConstRef flag on free-function const T&
returns (MapRawSig sets it only for scalar T&& returns). Legs in err_cpp_namespace_const_assign.cb.

## Ruling (maintainer, 2026-09-27)

Refuse. CFlat has no user-facing `const` (the 2026-08-26 ruling stands: CFlat `const` is not exposed or
enforced), but the compiler must respect C++ const internally, so a write through a C++ `const T&` /
`const T&&` result (free or member; `=`, compound assignment, postfix `++`/`--`) is an error, the same
way a C++ const namespace object already is. Follow the fix direction above.
