#pragma once
#include <algorithm>
#include <utility>

// Reference-returning and pair-returning templates over an unnamed-enum macro: the macro keeps
// master's int path when the enum-identity instantiation cannot register (macleft regression legs).
namespace cpi_t98r {
template <class T> inline const T& retr(const T& x) { return x; }
template <class T> inline T&& retf(T&& x) { return static_cast<T&&>(x); }
}
