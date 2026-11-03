#pragma once
#include <cstring>

namespace cppdefarg
{
    inline char trace[256] = {};
    inline int trace_size = 0;
    inline int live = 0;

    inline void mark(char value)
    {
        if (trace_size < 255)
        {
            trace[trace_size++] = value;
            trace[trace_size] = 0;
        }
    }
    inline void reset_trace() { trace_size = 0; trace[0] = 0; }
    inline int live_count() { return live; }
    inline bool trace_is(const char* expected) { return std::strcmp(trace, expected) == 0; }

    struct Trace
    {
        int value;
        explicit Trace(int n) noexcept : value(n) { mark('b'); ++live; }
        Trace(const Trace& other) noexcept : value(other.value) { mark('p'); ++live; }
        Trace(Trace&& other) noexcept : value(other.value) { mark('m'); ++live; }
        ~Trace() noexcept { mark('y'); --live; }
        int get() const noexcept { return value; }
    };

    inline Trace make(int value) noexcept { return Trace(value); }
}
