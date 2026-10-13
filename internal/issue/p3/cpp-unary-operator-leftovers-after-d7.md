# Unary operator leftovers after D7+C3

Summary: the reviewed T123 fixes cover unary filtering, conversion candidates, and verified overload results. These two C++ overload paths remain open.

## Still open

8. Member/free ranking is incomplete when a const unary member competes with a by-value free overload. For `BV` with `int operator-() const` and `int operator-(BV)`, clang reports ambiguity, while cflat picks the member. Repro: `struct BV { int operator-() const; }; int operator-(BV);` / `BV x; -x;`. Root cause: member/free candidate ranking does not compare the member's implicit object binding against the free parameter's binding for this by-value case. Fix direction: compare both candidates through clang's normal overload rank, then reject only when clang leaves them tied.

   Finding 2 adds the prvalue spellings: with a non-const `int operator-()` or `int operator-() &&` competing with `int operator-(S&&)`, clang reports ambiguity while cflat picks the free overload. The same holds for `!`. Repros: `struct S { int operator-() const; int operator!() const; }; int operator-(S&&); int operator!(S&&);` / `-makeS(); !makeS();` and `struct R { int operator-() &&; int operator!() &&; }; int operator-(R&&); int operator!(R&&);` / `-makeR(); !makeR();`.

10. The binary half of the const/rvalue candidate filter still loses the candidate reason. With only `operator%(M&, int)`, `rk.crefM(m) % 1` prints `Candidates (0):` instead of identifying the rejected `M&` candidate and the const/rvalue reason. Repro: `namespace rk { struct M {}; int operator%(M&, int); const M& crefM(M&); }` / `rk.M m = default; int n = rk.crefM(m) % 1;`. Root cause: filtering the candidate before diagnostic construction discards its declaration and the failed binding reason. Fix direction: preserve rejected candidates through the diagnostic path and print the const/rvalue mismatch when no viable candidate remains.

## Pre-existing unary gaps

- A C++ call returning `T&&` bypasses overloaded `!`: `struct S { int operator!() const; }; S&& xv(S&);` / `!xv(x);`. CFlat returns logical not of the pointer (0) instead of calling the operator.
- A C++ call returning `const S&` can be refused with `mutable C++ pointer parameter 'this'`. A free const-reference operator works in clang but is refused by CFlat: `struct S {}; int operator-(const S&); const S& cr(S&);` / `-cr(x);`. A non-const member is also refused by CFlat, while clang rejects the call: `struct S { int operator-(); }; const S& cr(S&);` / `-cr(x);`.
- On a mutable lvalue, a member plus free `T&` overload is ambiguous in clang, while CFlat picks the free overload: `struct S { int operator+(int); }; int operator+(S&, int);` / `S x; x + 1;`.
- A `const&&`-qualified member is called on a const lvalue receiver: `struct S { int operator-() const&&; }; const S& cr(S&);` / `-cr(x)`. CFlat returns 5 (master too); clang has no viable candidate. The `const&&` ref-qualifier must reject an lvalue implicit object argument.
