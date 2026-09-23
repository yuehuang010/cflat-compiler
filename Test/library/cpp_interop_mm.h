#pragma once
namespace mmprobe {
template<class T> const T& byref(const T& a, const T& b) { return a < b ? a : b; }
inline float accept_float(float value) noexcept { return value; }
template<class T> T twice(T value) { return value + value; }
}
