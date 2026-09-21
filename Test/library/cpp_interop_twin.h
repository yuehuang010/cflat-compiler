#pragma once

namespace cpptw
{
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
}
