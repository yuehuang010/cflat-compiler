#pragma once

namespace t20abi
{
    struct PtrPair { void* first; void* second; };
    struct PtrLong { void* first; long second; };
    struct LongPair { long first; long second; };
    struct I64Pair { long long first; long long second; };
    struct DoublePair { double first; double second; };
    struct Float4 { float a; float b; float c; float d; };
    struct Large { long a; long b; long c; };

    inline bool check_ptr_pair(PtrPair value)
    {
        return value.first != nullptr && value.second != nullptr
            && value.first != value.second;
    }
    inline long check_ptr_long(PtrLong value)
    {
        return value.first != nullptr ? value.second : -1;
    }
    inline long sum_long_pair(LongPair value) { return value.first + value.second; }
    inline long long sum_i64_pair(I64Pair value) { return value.first + value.second; }
    inline double sum_double_pair(DoublePair value) { return value.first + value.second; }
    inline float sum_float4(Float4 value) { return value.a + value.b + value.c + value.d; }
    inline long sum_large(Large value) { return value.a + value.b + value.c; }
    inline PtrPair make_ptr_pair(void* first, void* second) { return {first, second}; }
    inline PtrLong make_ptr_long(void* first, long second) { return {first, second}; }
    inline LongPair make_long_pair(long first, long second) { return {first, second}; }
    inline I64Pair make_i64_pair(long long first, long long second) { return {first, second}; }
    inline DoublePair make_double_pair(double first, double second) { return {first, second}; }
    inline Float4 make_float4(float a, float b, float c, float d) { return {a, b, c, d}; }
    inline Large make_large(long a, long b, long c) { return {a, b, c}; }

    struct MemberProbe
    {
        long sum(LongPair value) const { return value.first + value.second; }
        double sum(DoublePair value) const { return value.first + value.second; }
        float sum(Float4 value) const { return value.a + value.b + value.c + value.d; }
    };

    struct CtorProbe
    {
        long value;
        explicit CtorProbe(LongPair input) : value(input.first + input.second) {}
        long get() const { return value; }
    };

    struct FriendProbe
    {
        long bias;
        friend long operator+(FriendProbe lhs, LongPair rhs)
        {
            return lhs.bias + rhs.first + rhs.second;
        }
    };

    template<class T, class P, class R>
    struct CursorProbe
    {
        P pointer;
        long position;
        CursorProbe(P p, long n) : pointer(p), position(n) {}
        template<class OtherP, class OtherR>
        CursorProbe(const CursorProbe<T, OtherP, OtherR>& other)
            : pointer(other.pointer), position(other.position) {}
    };

    struct InsertProbe
    {
        using iterator = CursorProbe<int, int*, int&>;
        using const_iterator = CursorProbe<int, const int*, const int&>;
        iterator begin() { return {nullptr, 0}; }
        int insert(const_iterator, int&& value) { return 20 + value; }
        int insert(const_iterator, const int& value) { return 10 + value; }
        int insert(const_iterator, unsigned long count, const int& value)
        {
            return 100 + static_cast<int>(count) + value;
        }
    };
}
