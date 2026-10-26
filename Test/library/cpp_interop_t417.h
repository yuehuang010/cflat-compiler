#pragma once

namespace t417
{
    inline int& copies_ref() { static int value = 0; return value; }
    inline int& dtors_ref() { static int value = 0; return value; }
    inline int& live_ref() { static int value = 0; return value; }
    inline int& moves_ref() { static int value = 0; return value; }
    inline int& move_live_ref() { static int value = 0; return value; }
    inline void reset_ops() { copies_ref() = 0; dtors_ref() = 0; }
    inline void reset_move_ops() { moves_ref() = 0; }
    inline int copy_count() { return copies_ref(); }
    inline int dtor_count() { return dtors_ref(); }
    inline int live_count() { return live_ref(); }
    inline int move_count() { return moves_ref(); }
    inline int move_live_count() { return move_live_ref(); }

    class CopyOnly
    {
    public:
        CopyOnly() : value_(0) { ++live_ref(); }
        explicit CopyOnly(int value) : value_(value) { ++live_ref(); }
        CopyOnly(const CopyOnly& other) : value_(other.value_)
        {
            ++copies_ref();
            ++live_ref();
        }
        CopyOnly& operator=(const CopyOnly& other)
        {
            value_ = other.value_;
            return *this;
        }
        ~CopyOnly()
        {
            ++dtors_ref();
            --live_ref();
        }
        int value() const { return value_; }
        void set(int value) { value_ = value; }

    private:
        int value_;
    };

    class MoveOnly
    {
    public:
        MoveOnly() : value_(0) { ++move_live_ref(); }
        explicit MoveOnly(int value) : value_(value) { ++move_live_ref(); }
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&& other) noexcept : value_(other.value_)
        {
            other.value_ = -1;
            ++moves_ref();
            ++move_live_ref();
        }
        MoveOnly& operator=(MoveOnly&& other) noexcept
        {
            value_ = other.value_;
            other.value_ = -1;
            return *this;
        }
        ~MoveOnly() { --move_live_ref(); }
        int value() const { return value_; }
        void set(int value) { value_ = value; }

    private:
        int value_;
    };

    class DefaultedAssign
    {
    public:
        DefaultedAssign() : value_(0) {}
        DefaultedAssign(const DefaultedAssign& other) : value_(other.value_) {}
        ~DefaultedAssign() {}
        DefaultedAssign& operator=(const DefaultedAssign&) = default;
        int value() const { return value_; }
        void set(int value) { value_ = value; }

    private:
        int value_;
    };
}
