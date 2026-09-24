#pragma once
namespace cplv
{
    class Module
    {
    public:
        Module() = default;
        Module(const Module&) = delete;
        Module(Module&&) = default;
        virtual ~Module() = default;
        int tag = 0;
        virtual int forward(int x) { return x; }
    };
    inline int borrow(const Module& value) { return value.tag; }
    // Move-only ordered key: set/map members taking it by const& must bind, a copy must not.
    class Key
    {
    public:
        explicit Key(int v) : v_(v) {}
        Key(const Key&) = delete;
        Key(Key&& o) noexcept : v_(o.v_) { o.v_ = -1; }
        Key& operator=(const Key&) = delete;
        Key& operator=(Key&& o) noexcept { v_ = o.v_; o.v_ = -1; return *this; }
        bool operator<(const Key& o) const { return v_ < o.v_; }
        int value() const { return v_; }
        int v_;
    };
}
