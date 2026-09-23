#pragma once
// Fixture for assignment INTO a live nontrivial C++ object from a temporary (codes 3320-3339).
// Header-only; every special member bumps an inline static counter, so a leg asserts the EXACT
// number of constructions, copies, moves, assignments and destructions an assignment performs.

namespace cppas {

class Life
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int copy_assigns = 0;
    static inline int move_assigns = 0;
    static inline int last_dtor = 0;
    static inline void reset() noexcept
    {
        ctors = 0; copies = 0; moves = 0; dtors = 0; copy_assigns = 0; move_assigns = 0;
        last_dtor = 0;
    }
    inline Life() noexcept : v_(0) { ++ctors; }
    inline explicit Life(int v) noexcept : v_(v) { ++ctors; }
    inline Life(const Life& o) noexcept : v_(o.v_) { ++copies; }
    inline Life(Life&& o) noexcept : v_(o.v_) { o.v_ = -1; ++moves; }
    inline ~Life() noexcept { last_dtor = v_; v_ = -99; ++dtors; }
    inline Life& operator=(const Life& o) noexcept { v_ = o.v_; ++copy_assigns; return *this; }
    inline Life& operator=(Life&& o) noexcept { v_ = o.v_; o.v_ = -1; ++move_assigns; return *this; }
    inline int value() const noexcept { return v_; }
    // By-value MEMBER call result.
    inline Life twice() const noexcept { return Life(v_ * 2); }
    // By-value MEMBER operator result.
    inline Life operator-(const Life& o) const noexcept { return Life(v_ - o.v_); }
    int v_;
};

// Copy assignment DELETED: only a temporary or an explicit move may be assigned in.
class MoveOnly
{
public:
    static inline int move_assigns = 0;
    static inline int dtors = 0;
    inline explicit MoveOnly(int v) noexcept : v_(v) {}
    MoveOnly(const MoveOnly&) = delete;
    inline MoveOnly(MoveOnly&& o) noexcept : v_(o.v_) { o.v_ = -1; }
    inline ~MoveOnly() noexcept { v_ = -99; ++dtors; }
    MoveOnly& operator=(const MoveOnly&) = delete;
    inline MoveOnly& operator=(MoveOnly&& o) noexcept { v_ = o.v_; o.v_ = -1; ++move_assigns; return *this; }
    inline int value() const noexcept { return v_; }
    int v_;
};
inline MoveOnly operator+(const MoveOnly& a, const MoveOnly& b) noexcept { return MoveOnly(a.v_ + b.v_); }

// By-value FREE operator result.
inline Life operator+(const Life& a, const Life& b) noexcept { return Life(a.v_ + b.v_); }
// By-value free-function result.
inline Life make_life(int v) noexcept { return Life(v); }

} // namespace cppas
