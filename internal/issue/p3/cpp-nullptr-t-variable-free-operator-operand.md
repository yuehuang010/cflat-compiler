# A std.nullptr_t VARIABLE as a free C++ operator operand is refused (`nn == n`); the literal `nn == nullptr` works

Bucket: p3 (one-site candidate). With `inline int operator==(const N&, const N&)` and `N(std::nullptr_t)`,
`std.nullptr_t n = nullptr; nn == n` reports "no overload of operator== matches" (clang: converting
temporary, 502). The free-operator argument builder (makeArgument, MainListener_Expressions.cpp ~12960)
marks IsCxxNullptrT only for a constant null operand; a nullptr_t variable arrives as a loaded value with
empty type name, so the declared-nullptr_t flag is lost. TryBinaryOperatorOverload receives no NamedVariable,
only type name / pointer depth, so the flag must be plumbed from its callers. Probe: a Test/test_cpp_interop.cb leg using Test/library/cpp_t48_nullptr.hpp (N, operator==) with `nn == n`.
