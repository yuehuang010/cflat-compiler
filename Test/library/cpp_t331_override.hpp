#pragma once

namespace cppt331 {
class Counted
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int live = 0;
    static inline void reset() noexcept { ctors = copies = moves = dtors = live = 0; }

    inline explicit Counted(int x) noexcept : value(x) { ++ctors; ++live; }
    inline Counted(const Counted& other) noexcept : value(other.value) { ++copies; ++live; }
    inline Counted(Counted&& other) noexcept : value(other.value)
    { other.value = -1; ++moves; ++live; }
    inline ~Counted() noexcept { ++dtors; --live; }

    int value;
};

struct Base
{
    virtual ~Base() = default;
    virtual int use(Counted value) = 0;
};

inline int call(Base* value, int input)
{
    return value->use(Counted(input));
}

inline int catch_call(Base* value, int input) noexcept
{
    try { return value->use(Counted(input)); }
    catch (int error) { return -error; }
}

inline int throw_if_positive(int value)
{
    if (value > 0) throw value;
    return value;
}
}
