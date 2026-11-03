#pragma once

namespace t513mix {

inline int t513M3Sequence = 0;
inline void t513M3Reset() { t513M3Sequence = 0; }
inline int t513M3Stats() noexcept { return t513M3Sequence; }

struct A
{
    int v;
    A(int n) noexcept : v(n) { t513M3Sequence = t513M3Sequence * 10 + 1; }
    A(const A& other) noexcept : v(other.v) {
        t513M3Sequence = t513M3Sequence * 10 + 2;
    }
    A(A&& other) noexcept : v(other.v) {
        t513M3Sequence = t513M3Sequence * 10 + 3;
    }
    ~A() noexcept { t513M3Sequence = t513M3Sequence * 10 + 4; }
};

struct BV
{
    int v;
    BV(int n) noexcept : v(n) {
        t513M3Sequence = t513M3Sequence * 10 + 5;
    }
    BV(A a) noexcept : v(a.v + 100) {
        t513M3Sequence = t513M3Sequence * 10 + 6;
    }
    BV(const BV& other) noexcept : v(other.v) {
        t513M3Sequence = t513M3Sequence * 10 + 8;
    }
    BV(BV&& other) noexcept : v(other.v) {
        t513M3Sequence = t513M3Sequence * 10 + 9;
    }
    ~BV() noexcept { t513M3Sequence = t513M3Sequence * 10 + 7; }
};

inline A mkA(int value) noexcept { return A(value); }
inline BV mkBV(int value = 20) noexcept { return BV(value); }
inline int ctakeBV(BV value) noexcept { return value.v; }
inline int ctakeBVR(const BV& value) noexcept { return value.v; }

}
