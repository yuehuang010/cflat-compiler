# Class xvalue at a C* parameter binds the object instead of calling operator C*()

PRE-EXISTING (master equal; T70 reviews 3-4). With a class `C` declaring an implicit conversion to
`C*` (own, inherited, template or via Derived*), `f(std.move(c))` into `f(C*)` is accepted since T70
(clang decides viability) but CFlat passes the address of `c` instead of the conversion result.
Repro: operator returns a pointer to a different static object (value 73): clang 73, CFlat 31.
Legs 7086-7092 in Test/test_cpp_interop.cb only assert the overload pick for this reason.
Also: `template<class T> operator T*()` with `C*` and `void*` overloads is ambiguous in clang but
CFlat picks `C*` (master same).
Fix direction: at the same-class pointer door, when the conversion query says convertible, call the
conversion through a clang wrapper (`return static_cast<S&&>(*p0);` already generated for the query)
and pass its result; treat ambiguity as a refusal.
