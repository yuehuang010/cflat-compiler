#pragma once
// Two same-name, same-arity member templates whose bodies fail for different reasons: a stored
// refusal cause of one must never be printed for the other (err_cpp_struct_lvalue_sink.cb).
#include "cpp_interop_lvalue_sink.h"
class OtherSink
{
public:
    OtherSink() = default;
    OtherSink(const OtherSink&) = delete;
};
class ConstOnly
{
public:
    template <typename T> void add(const T& value) { T copy(value); (void)copy; }
    template <typename T> void add(T* value) { T copy(*value); (void)copy; }
};
