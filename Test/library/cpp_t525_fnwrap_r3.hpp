#pragma once
#include <string>
#include <type_traits>
#include <utility>
// FNWRAP fixtures: closure results and closure parameters that C++ takes by value, by reference,
// or as a struct / string / alias signature. Each mirrors a clang-checked oracle.
namespace fnwrap3 {
template<class F> F copy_return(F f) { return f; }
template<class F> const F& cref(const F& f) { return f; }
template<class F> struct OwnedBox { F f; int run(int x) { return f(x); } };
template<class F> OwnedBox<std::remove_reference_t<F>> holdowned(F&& f) { return { f }; }
struct Record { int value; };
using Alias = int;
template<class F> int apply_record(F&& f, Record r) { return f(r); }
template<class F> int apply_string(F&& f, std::string s) { return f(s); }
template<class F> int apply_alias(F&& f, Alias n) { return f(n); }
template<class F> int apply_move_string(F&& f, std::string s) { return f(std::move(s)); }
template<class F> int apply_alias_result(F&& f) { int& r = f(); return r; }
}
