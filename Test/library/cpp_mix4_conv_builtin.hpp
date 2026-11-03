#pragma once

// MIX4: a class whose only conversion is to a scalar reaches the built-in operators through
// that conversion, landing in the conversion's own return type (clang).
namespace mix4conv
{
    struct DOnly { operator double() const { return 3.5; } };
    struct LOnly { long long v; operator long long() const { return v; } };
    struct BOnly { operator bool() const { return true; } };
    struct COnly { operator char() const { return 'A'; } };
    struct DPlus
    {
        operator double() const { return 1.5; }
        friend double operator+(const DPlus&, int) { return 99.0; }
    };
    struct MOnly
    {
        int v;
        operator int() const { return v; }
        int operator-(int x) const { return v - x + 1000; }
    };
    // Integral promotion: a narrower-than-int result (short, unsigned char) is int in the operator.
    struct SShortConv { operator short() const { return 30000; } };
    struct UCharConv { operator unsigned char() const { return 200; } };
    // A scalar lvalue-reference return is loaded to its value (clang: `rd * 2` is 4.5).
    struct RDoubleRef
    {
        double x = 2.25;
        operator const double&() const { return x; }
    };
}
