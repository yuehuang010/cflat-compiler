# Ternary owned and borrowed arms have wrong join behavior

Summary: Owned-vs-borrowed ternary joins are refused in valid expressions and accepted expressions have incorrect copy/move behavior.
Repro: `a + (c ? default : x)` for by-value member `operator+`; `y = c ? default : x`; `Holder h{ c ? default : mk(5) }`; `a + (c ? mk(3) : mk(4))`.
Master vs expected: master accepted some forms while leaking or copying incorrectly; after T453 guard these forms are refused. Clang accepts with expected move/copy counts.
Root cause hint: `IsTernaryArmOfBinaryOperand`, MainListener_Declarations.cpp:51.
Found: T453 r1 opus review, 2026-10-24 box, PRE-EXISTING
