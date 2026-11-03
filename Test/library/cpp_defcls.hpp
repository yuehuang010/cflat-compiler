#pragma once
#include <cstring>

namespace cppdefcls
{
    inline char trace[128] = {};
    inline int trace_size = 0;
    inline int live = 0;

    inline void mark(char value)
    {
        if (trace_size < 127)
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
        explicit Trace(int n = 0) noexcept : value(n) { mark('b'); ++live; }
        Trace(const Trace& other) noexcept : value(other.value) { mark('p'); ++live; }
        Trace(Trace&& other) noexcept : value(other.value) { mark('m'); ++live; }
        ~Trace() noexcept { mark('y'); --live; }
    };

    inline Trace make(int value) noexcept { return Trace(value); }
    inline Trace global = Trace(8);

    // Non-explicit default constructor: copy-list-initialization '= {}' is legal for it.
    struct Implicit
    {
        int value;
        Implicit(int n = 9) noexcept : value(n) { mark('b'); ++live; }
        Implicit(const Implicit& other) noexcept : value(other.value) { mark('p'); ++live; }
        Implicit(Implicit&& other) noexcept : value(other.value) { mark('m'); ++live; }
        ~Implicit() noexcept { mark('y'); --live; }
    };
    inline Implicit make_implicit(int n) noexcept { return Implicit(n); }
}
