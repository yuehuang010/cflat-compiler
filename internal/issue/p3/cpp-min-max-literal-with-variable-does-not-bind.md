# std.min / std.max with one literal and one variable does not bind

## Summary
`std.min(a, 5)`, `std.min(5, a)`, `std.max(a, 5)` fail with "'min' is not a member of namespace 'std' (C++ free
function 'std.min' could not be bound (clang: no matching function for call to 'min'))". `std.min(a, b)`,
`std.min(a, b + 1)`, `std.max(a + 1, b)` and `std.min(5, 6)` all bind and run.

## Repro
    import cpp "algorithm";
    extern int main() { int a = 3; return std.min(a, 5) == 3 ? 0 : 1; }

## Root cause (suspected)
The discriminator is literal-ness, not value category: an rvalue EXPRESSION binds. The deduction probe spells the
literal argument differently from the variable, so `const _Tp&, const _Tp&` deduces two conflicting `_Tp`. An
own-header `template<class T> const T& byref(const T&, const T&)` with a literal works, so the libc++ overload set
(initializer_list + comparator overloads) is part of it. Same area as the declared-identity spelling (eb7e9737).

## Fix direction
Spell an integer literal by the CFlat type it would take against the sibling argument (int), and check which libc++
overload clang rejects with the current spelling. Add legs to Test/test_cpp_interop_template.cb.
