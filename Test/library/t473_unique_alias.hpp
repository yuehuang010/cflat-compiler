#pragma once

namespace uqp
{
    inline int move_count = 0;
    inline int dtor_count = 0;
    inline int aligned_dtor_count = 0;

    struct W
    {
        int v;
        explicit W(int value) : v(value) {}
        W(const W&) = delete;
        W(W&& other) noexcept : v(other.v) { ++move_count; }
        ~W() { ++dtor_count; }
    };

    struct alignas(64) W64
    {
        int v;
        explicit W64(int value) : v(value) {}
        W64(const W64&) = delete;
        ~W64() { ++aligned_dtor_count; }
    };

    struct alignas(32) W32
    {
        int v;
        explicit W32(int value) : v(value) {}
        ~W32() { ++aligned_dtor_count; }
    };

    inline void reset_counts()
    {
        move_count = 0;
        dtor_count = 0;
    }

    inline void reset_aligned_counts() { aligned_dtor_count = 0; }

    inline int moves() { return move_count; }
    inline int dtors() { return dtor_count; }
    inline int aligned_dtors() { return aligned_dtor_count; }
}
