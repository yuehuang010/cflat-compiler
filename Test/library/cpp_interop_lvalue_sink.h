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
}
