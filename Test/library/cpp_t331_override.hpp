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

struct Immovable
{
    static inline int ctors = 0;
    static inline int dtors = 0;
    static inline int live = 0;
    static inline void reset() noexcept { ctors = dtors = live = 0; }

    inline explicit Immovable(int x) noexcept : value(x) { ++ctors; ++live; }
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    inline ~Immovable() noexcept { ++dtors; --live; }

    int value;
};

struct ImmovableBase
{
    static inline int intCalls = 0;
    virtual ~ImmovableBase() = default;
    virtual int use(Immovable value) = 0;
    virtual int use(int value) { ++intCalls; return value + 100; }
    static inline void reset() noexcept { intCalls = 0; }
};

inline int call_immovable(ImmovableBase* value, int input)
{
    return value->use(Immovable(input));
}

inline int catch_immovable(ImmovableBase* value, int input) noexcept
{
    try { return value->use(Immovable(input)); }
    catch (int error) { return -error; }
}

struct TrivialDtorImmovable
{
    static inline int ctors = 0;
    static inline void reset() noexcept { ctors = 0; }

    inline explicit TrivialDtorImmovable(int x) noexcept : value(x) { ++ctors; }
    TrivialDtorImmovable(const TrivialDtorImmovable&) = delete;
    TrivialDtorImmovable(TrivialDtorImmovable&&) = delete;
    ~TrivialDtorImmovable() = default;

    int value;
};

struct TrivialDtorBase
{
    static inline int intCalls = 0;
    virtual ~TrivialDtorBase() = default;
    virtual int use(TrivialDtorImmovable value) = 0;
    virtual int use(int value) { ++intCalls; return value + 100; }
    static inline void reset() noexcept { intCalls = 0; }
};

inline int call_trivial_dtor(TrivialDtorBase* value, int input)
{
    return value->use(TrivialDtorImmovable(input));
}

struct NonConstCopy
{
    inline explicit NonConstCopy(int x) noexcept : value(x) {}
    NonConstCopy(NonConstCopy&) = default;
    NonConstCopy(NonConstCopy&&) = delete;

    int value;
};

struct NonConstCopyBase
{
    static inline int intCalls = 0;
    virtual ~NonConstCopyBase() = default;
    virtual int use(NonConstCopy value) = 0;
    virtual int use(int value) { ++intCalls; return value + 100; }
    static inline void reset() noexcept { intCalls = 0; }
};

inline int call_nonconst_copy(NonConstCopyBase* value, int input)
{
    return value->use(NonConstCopy(input));
}

struct PrivateCopy
{
    inline explicit PrivateCopy(int x) noexcept : value(x) {}
private:
    PrivateCopy(const PrivateCopy&) = default;
    PrivateCopy(PrivateCopy&&) = default;
public:
    int value;
};

struct PrivateCopyBase
{
    static inline int intCalls = 0;
    virtual ~PrivateCopyBase() = default;
    virtual int use(PrivateCopy value) = 0;
    virtual int use(int value) { ++intCalls; return value + 100; }
    static inline void reset() noexcept { intCalls = 0; }
};

inline int call_private_copy(PrivateCopyBase* value, int input)
{
    return value->use(PrivateCopy(input));
}

struct FinalBase final {};

struct MixedBase
{
    static inline int intCalls = 0;
    virtual ~MixedBase() = default;
    virtual int use(Immovable immovable, Counted counted) = 0;
    virtual int use(int value, Counted counted)
    { ++intCalls; return value + counted.value + 100; }
    static inline void reset() noexcept { intCalls = 0; }
};

inline int call_mixed(MixedBase* value, int immovable, int counted)
{
    return value->use(Immovable(immovable), Counted(counted));
}
}
