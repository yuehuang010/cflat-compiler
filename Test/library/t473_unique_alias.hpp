#pragma once

namespace uqp
{
    inline int move_count = 0;
    inline int dtor_count = 0;

    struct W
    {
        int v;
        explicit W(int value) : v(value) {}
        W(const W&) = delete;
        W(W&& other) noexcept : v(other.v) { ++move_count; }
        ~W() { ++dtor_count; }
    };

    inline void reset_counts()
    {
        move_count = 0;
        dtor_count = 0;
    }

    inline int moves() { return move_count; }
    inline int dtors() { return dtor_count; }
}
