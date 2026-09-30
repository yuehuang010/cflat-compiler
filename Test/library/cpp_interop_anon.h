#pragma once

// Anonymous fields exercise the promoted-member path in C++ header imports.

namespace cppanon
{
    union U
    {
        struct { unsigned lo; unsigned hi; };
        struct { unsigned lo; unsigned hi; } u;
        unsigned long long q;
    };

    // Holds U by value inside an anonymous member: the batch projecting U mid-registration must
    // lay out U's own synthetic members first, or U.u registers as opaque bytes.
    struct Wrap
    {
        union { void* p; U v; };
        int k;
    };

    class Holder
    {
    public:
        union { unsigned lo; unsigned hi; };
        unsigned long long q;
    };
}
