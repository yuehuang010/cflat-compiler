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

struct W
{
    int v;
    W(BV b) noexcept : v(b.v + 1000) {}
    W(int x, BV b) noexcept : v(b.v + x + 1000) {}
};

inline A mkA(int value) noexcept { return A(value); }
inline BV mkBV(int value = 20) noexcept { return BV(value); }
inline int ctakeBV(BV value) noexcept { return value.v; }
inline int ctakeBVR(const BV& value) noexcept { return value.v; }
// MIXRES: member callees take the mixed join without a free-function sink.
// MIXRES r2: refused nested call-argument joins keep the arm-clash diagnostic.
struct EB { int v; explicit EB(A a) noexcept : v(a.v) {} EB(int n) noexcept : v(n) {} EB(const EB& o) noexcept : v(o.v) {} ~EB() noexcept {} };
inline EB mkEB(int n) noexcept { return EB(n); }
inline int takeEB(EB e) noexcept { return e.v; }
struct SS { int n; SS(const char* s) noexcept : n(1) {} SS(const SS& o) noexcept : n(o.n) {} ~SS() noexcept {} };
inline SS mkSS(const char* s) noexcept { return SS(s); }
inline int takeSS(SS s) noexcept { return s.n; }
struct Host
{
    int take(BV value) noexcept { return value.v; }
    int takeR(const BV& value) noexcept { return value.v; }
};

}
