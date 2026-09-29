# Brace-list backing storage: shapes still built inside a thunk

Follow-up of fix/brace-backing (7ecfac3f) and fix/brace-backing-2 (2026-09-28). Caller-frame `E[N]`
backing now covers: array-reference and std::initializer_list constructor targets (inherited
`using Base::Base` ones included), a class-typed constructor parameter whose class has one
initializer_list<E> constructor, and `RequestCxxBraceFunction` scalar lists into a
std::initializer_list parameter or a class with one initializer_list<E> constructor. Still open (a
list can dangle only when the callee keeps a view of it):

1. `RequestCxxFunctionTemplate` (function TEMPLATE call) spells scalar lists `{p0, p1}` inside its
   thunk. `template <class T> const T* f(std::initializer_list<T> l) { return l.begin(); }` called
   as `f({a, b})` reads garbage on master and on fix/brace-backing-2. Needs a selector to learn what
   the list binds to after deduction.
2. `RequestCxxBraceFunction` class-element lists that need conversion (`convertElements`) build
   their `E arr[] = {E(p0), ...}` / `std::initializer_list<E>{...}` inside the thunk.
3. A constructor-TEMPLATE parameter pack that an argument reaches is not mirrored in the overload
   selector: `PkT<long>({2, 3}, 5)` (clang picks the pack ctor, which=2) is refused with "passes an
   argument into a parameter pack beside its brace-list constructors". An EMPTY trailing pack and a
   non-type template parameter are mirrored.
4. A class-typed constructor parameter is backed only when every constructor that fits takes the
   same class at that position, and the class declares exactly one non-template
   initializer_list<E> constructor; a list-ctor TEMPLATE (never listed by the extractor) keeps the
   thunk-built list.

Related, not backing: a non-template function with an array-reference parameter
(`const long (&)[2]`) does not accept a brace argument at all ("no overload ... matches"); the
copying (non-backed) path refuses `vector<double>` from `{floatVar, 2}` although clang accepts it
(CFlat call conversion of the thunk's per-element parameters).

Legs for the closed shapes: Test/test_cpp_interop.cb 7169-7186 and
Test/errors/err_cpp_brace_arg_no_match.cb (fixtures in Test/library/cpp_interop_ctorref.h, Brc*).
