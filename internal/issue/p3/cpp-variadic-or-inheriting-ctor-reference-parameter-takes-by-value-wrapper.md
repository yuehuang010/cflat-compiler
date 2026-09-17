# A variadic or inheriting C++ constructor with a `U&` parameter still goes through the by-value wrapper

Found 2026-09-17 during fix/cpp-ctor-ref-param. Not reproduced, inferred from code: RequestCxxVariadicConstructor spells the generated wrapper's parameters from the CFlat ARGUMENT types (by value), so a `U&` parameter in a variadic or inheriting (`using Base::Base`) constructor would receive a copy. The ordinary constructor-template shape (`template<class U> Box(U&)`) routes through the `__cflat_free_*` wrapper and was measured correct.

## Fix direction

Write the repro first (variadic ctor forwarding to a `U&` sink, inheriting ctor over `Base(Cnt&)`); if it reproduces, spell wrapper parameters from the C++ parameter kinds where known.

Suggested bucket: p3 (unconfirmed).
