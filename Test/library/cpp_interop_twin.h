#pragma once

#include "cpp_interop_basic.h"

extern "C" cppi::Tracked bridge_exported_for_test(int payload) noexcept;

namespace cpptw
{
    inline int call_bridge_exported(int payload) noexcept
    {
        return bridge_exported_for_test(payload).value();
    }

    inline int& ctor_counter() { static int value = 0; return value; }
    inline int& copy_counter() { static int value = 0; return value; }
    inline int& move_counter() { static int value = 0; return value; }
    inline int& dtor_counter() { static int value = 0; return value; }
    inline int& copy_assign_counter() { static int value = 0; return value; }
    inline int& move_assign_counter() { static int value = 0; return value; }
    inline int& live_allocation_counter() { static int value = 0; return value; }

    inline void reset_counts()
    {
        ctor_counter() = 0;
        copy_counter() = 0;
        move_counter() = 0;
        dtor_counter() = 0;
        copy_assign_counter() = 0;
        move_assign_counter() = 0;
        live_allocation_counter() = 0;
    }
    inline int ctor_count() { return ctor_counter(); }
    inline int copy_count() { return copy_counter(); }
    inline int move_count() { return move_counter(); }
    inline int dtor_count() { return dtor_counter(); }
    inline int copy_assign_count() { return copy_assign_counter(); }
    inline int move_assign_count() { return move_assign_counter(); }
    inline int live_allocations() { return live_allocation_counter(); }

    class Twin
    {
    public:
        int field;

        Twin() : data_(new int[3]), field(0)
        {
            data_[0] = 0; data_[1] = 1; data_[2] = 2;
            ++ctor_counter(); ++live_allocation_counter();
        }
        explicit Twin(int value) : data_(new int[3]), field(value)
        {
            data_[0] = value; data_[1] = value + 1; data_[2] = value + 2;
            ++ctor_counter(); ++live_allocation_counter();
        }
        Twin(const Twin& other) : data_(new int[3]), field(other.field)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++copy_counter(); ++live_allocation_counter();
        }
        Twin(Twin&& other) noexcept : data_(other.data_), field(other.field)
        {
            other.data_ = new int[3];
            other.data_[0] = -1; other.data_[1] = -1; other.data_[2] = -1;
            other.field = -1;
            ++move_counter(); ++live_allocation_counter();
        }
        Twin& operator=(const Twin& other)
        {
            if (this != &other)
            {
                data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
                field = other.field;
            }
            ++copy_assign_counter();
            return *this;
        }
        Twin& operator=(Twin&& other) noexcept
        {
            if (this != &other)
            {
                delete[] data_;
                --live_allocation_counter();
                data_ = other.data_;
                field = other.field;
                other.data_ = new int[3];
                other.data_[0] = -1; other.data_[1] = -1; other.data_[2] = -1;
                other.field = -1;
                ++live_allocation_counter();
            }
            ++move_assign_counter();
            return *this;
        }
        ~Twin()
        {
            delete[] data_;
            --live_allocation_counter();
            ++dtor_counter();
        }

        int value() const { return field; }
        void set(int value)
        {
            field = value;
            data_[0] = value; data_[1] = value + 1; data_[2] = value + 2;
        }
        bool operator==(const Twin& other) const { return field == other.field; }
        int& operator[](int index) { return data_[index]; }
        int operator[](int index) const { return data_[index]; }
        operator bool() const { return field != 0; }
        int* begin() { return data_; }
        int* end() { return data_ + 3; }
        const int* begin() const { return data_; }
        const int* end() const { return data_ + 3; }

        static int static_value() { return 73; }

    private:
        int* data_;
    };

    inline int& tracked_iter_ctor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_copy_counter() { static int value = 0; return value; }
    inline int& tracked_iter_move_counter() { static int value = 0; return value; }
    inline int& tracked_iter_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_bad_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_live_counter() { static int value = 0; return value; }
    inline int& tracked_range_ctor_counter() { static int value = 0; return value; }
    inline int& tracked_range_copy_counter() { static int value = 0; return value; }
    inline int& tracked_range_move_counter() { static int value = 0; return value; }
    inline int& tracked_range_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_range_bad_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_range_live_counter() { static int value = 0; return value; }

    inline void reset_tracked_range_counts()
    {
        tracked_iter_ctor_counter() = 0;
        tracked_iter_copy_counter() = 0;
        tracked_iter_move_counter() = 0;
        tracked_iter_dtor_counter() = 0;
        tracked_iter_bad_dtor_counter() = 0;
        tracked_iter_live_counter() = 0;
        tracked_range_ctor_counter() = 0;
        tracked_range_copy_counter() = 0;
        tracked_range_move_counter() = 0;
        tracked_range_dtor_counter() = 0;
        tracked_range_bad_dtor_counter() = 0;
        tracked_range_live_counter() = 0;
    }
    inline int tracked_iter_ctor_count() { return tracked_iter_ctor_counter(); }
    inline int tracked_iter_copy_count() { return tracked_iter_copy_counter(); }
    inline int tracked_iter_move_count() { return tracked_iter_move_counter(); }
    inline int tracked_iter_dtor_count() { return tracked_iter_dtor_counter(); }
    inline int tracked_iter_bad_dtor_count() { return tracked_iter_bad_dtor_counter(); }
    inline int tracked_iter_live_count() { return tracked_iter_live_counter(); }
    inline int tracked_iter_made_count()
    {
        return tracked_iter_ctor_count() + tracked_iter_copy_count() + tracked_iter_move_count();
    }
    inline int tracked_range_ctor_count() { return tracked_range_ctor_counter(); }
    inline int tracked_range_copy_count() { return tracked_range_copy_counter(); }
    inline int tracked_range_move_count() { return tracked_range_move_counter(); }
    inline int tracked_range_dtor_count() { return tracked_range_dtor_counter(); }
    inline int tracked_range_bad_dtor_count() { return tracked_range_bad_dtor_counter(); }
    inline int tracked_range_live_count() { return tracked_range_live_counter(); }
    inline int tracked_range_made_count()
    {
        return tracked_range_ctor_count() + tracked_range_copy_count() + tracked_range_move_count();
    }

    class TrackedRangeIterator
    {
    public:
        TrackedRangeIterator(int* current, int* finish)
            : current_(current), finish_(finish), destroyed_(false)
        {
            ++tracked_iter_ctor_counter();
            ++tracked_iter_live_counter();
        }
        TrackedRangeIterator(const TrackedRangeIterator& other)
            : current_(other.current_), finish_(other.finish_), destroyed_(false)
        {
            ++tracked_iter_copy_counter();
            ++tracked_iter_live_counter();
        }
        TrackedRangeIterator(TrackedRangeIterator&& other) noexcept
            : current_(other.current_), finish_(other.finish_), destroyed_(false)
        {
            ++tracked_iter_move_counter();
            ++tracked_iter_live_counter();
        }
        ~TrackedRangeIterator()
        {
            ++tracked_iter_dtor_counter();
            if (destroyed_)
            {
                ++tracked_iter_bad_dtor_counter();
                return;
            }
            destroyed_ = true;
            --tracked_iter_live_counter();
        }

        bool operator!=(const TrackedRangeIterator& other) const
        {
            return current_ != other.current_;
        }
        int operator*() const { return *current_; }
        TrackedRangeIterator& operator++()
        {
            ++current_;
            return *this;
        }

    private:
        int* current_;
        int* finish_;
        bool destroyed_;
    };

    class TrackedRange
    {
    public:
        TrackedRange() : destroyed_(false)
        {
            data_[0] = 3; data_[1] = 5; data_[2] = 7;
            ++tracked_range_ctor_counter();
            ++tracked_range_live_counter();
        }
        TrackedRange(const TrackedRange& other) : destroyed_(false)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++tracked_range_copy_counter();
            ++tracked_range_live_counter();
        }
        TrackedRange(TrackedRange&& other) noexcept : destroyed_(false)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++tracked_range_move_counter();
            ++tracked_range_live_counter();
        }
        ~TrackedRange()
        {
            ++tracked_range_dtor_counter();
            if (destroyed_)
            {
                ++tracked_range_bad_dtor_counter();
                return;
            }
            destroyed_ = true;
            --tracked_range_live_counter();
        }

        TrackedRangeIterator begin()
        {
            return TrackedRangeIterator(data_, data_ + 3);
        }
        TrackedRangeIterator end()
        {
            return TrackedRangeIterator(data_ + 3, data_ + 3);
        }

    private:
        int data_[3];
        bool destroyed_;
    };

    inline TrackedRange make_tracked_range()
    {
        return TrackedRange();
    }

    class RawPointerRange
    {
    public:
        RawPointerRange() : first_(4), second_(7)
        {
            values_[0] = &first_;
            values_[1] = &second_;
        }
        int** begin() { return values_; }
        int** end() { return values_ + 2; }

    private:
        int first_;
        int second_;
        int* values_[2];
    };

    class PrvalueTwinIterator
    {
    public:
        PrvalueTwinIterator(int value, int finish) : value_(value), finish_(finish) {}
        bool operator!=(const PrvalueTwinIterator& other) const
        {
            return value_ != other.value_;
        }
        Twin operator*() const { return Twin(value_); }
        PrvalueTwinIterator& operator++()
        {
            ++value_;
            return *this;
        }

    private:
        int value_;
        int finish_;
    };

    class PrvalueTwinRange
    {
    public:
        PrvalueTwinIterator begin() { return PrvalueTwinIterator(1, 3); }
        PrvalueTwinIterator end() { return PrvalueTwinIterator(3, 3); }
    };
}
