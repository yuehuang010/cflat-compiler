#pragma once

#include "cpp_interop_lastuse.h"

namespace t518refdef
{
    namespace
    {
        inline int const_lref(const t418::Val& value, int b)
        { return 10000 + value.value() * 100 + b; }
        inline int rref(t418::Val&& value, int b)
        { return 11000 + value.value() * 100 + b; }
        inline int nonconst_lref(t418::Val& value, int b)
        { return 12000 + value.value() * 100 + b; }
        inline int refs(t418::Val& value, int b)
        { return 20000 + value.value() * 100 + b; }
        inline int refs(const t418::Val& value, int b)
        { return 10000 + value.value() * 100 + b; }
        inline int byval(t418::Val value, int b)
        { return 30000 + value.value() * 100 + b; }
        // No default constructor: a `default` arm of this class has nothing to build.
        struct NoDefault
        {
            explicit NoDefault(int) {}
            NoDefault(const NoDefault&) {}
            int value() const { return 1; }
        };
        inline int nodef(const NoDefault& value, int b) { return b; }
        template <class T>
        inline int tmpl(const T& value, int b)
        { return 40000 + value.value() * 100 + b; }
        // Converting class parameter: `Other` is built from a Val (its `w` is value + 900).
        struct Other
        {
            int w;
            Other(const t418::Val& v) : w(v.value() + 900) {}
        };
        inline int convf(const Other& o, int b) { return 60000 + o.w * 10 + b; }
        template <class T>
        inline int tconv(const Other& o, T b) { return 60000 + o.w * 10 + (int)b; }
    }
}

namespace t518refdefbound
{
    inline int const_lref(const t418::Val& value, int b)
    { return 10000 + value.value() * 100 + b; }
    inline int rref(t418::Val&& value, int b)
    { return 11000 + value.value() * 100 + b; }
    inline int nonconst_lref(t418::Val& value, int b)
    { return 12000 + value.value() * 100 + b; }
    inline int refs(t418::Val& value, int b)
    { return 20000 + value.value() * 100 + b; }
    inline int refs(const t418::Val& value, int b)
    { return 10000 + value.value() * 100 + b; }
    inline int byval(t418::Val value, int b)
    { return 30000 + value.value() * 100 + b; }
}
