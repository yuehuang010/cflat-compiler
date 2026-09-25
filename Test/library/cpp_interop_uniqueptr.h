#pragma once
#include <memory>
// R5: the `unique` keyword on a C++ class pointee is std::unique_ptr<T>. Every structor bumps a
// counter so a leg can prove single construction and single destruction on each path.
namespace cpuq
{
    inline int& ctor_n() { static int n = 0; return n; }
    inline int& dtor_n() { static int n = 0; return n; }
    inline void reset_counts() { ctor_n() = 0; dtor_n() = 0; }
    inline int ctors() { return ctor_n(); }
    inline int dtors() { return dtor_n(); }

    class Widget
    {
    public:
        explicit Widget(int v) : v_(v) { ++ctor_n(); }
        Widget(const Widget&) = delete;
        Widget& operator=(const Widget&) = delete;
        ~Widget() { ++dtor_n(); }
        int value() const { return v_; }
        void set(int v) { v_ = v; }
        int v_;
    };

    // Sinks return the payload, or -1 for an empty pointer. The by-value sink destroys the
    // Widget at its own end; the && sink adopts it into a local, so it destroys it too.
    inline int take(std::unique_ptr<Widget> p) { return p ? p->value() : -1; }
    inline int take_rr(std::unique_ptr<Widget>&& p)
    {
        std::unique_ptr<Widget> local = std::move(p);
        return local ? local->value() : -1;
    }
    // Borrows without adopting: the caller keeps ownership.
    inline int take_rr_keep(std::unique_ptr<Widget>&& p) { return p ? p->value() : -1; }
    inline std::unique_ptr<Widget> make(int v) { return std::make_unique<Widget>(v); }
    inline int peek(const std::unique_ptr<Widget>& p) { return p ? p->value() : -1; }
    inline int peek_raw(Widget* p) { return p ? p->value() : -1; }

    struct Holder
    {
        std::unique_ptr<Widget> w;
        int tag = 0;
    };
    inline int holder_value(const Holder& h) { return h.w ? h.w->value() : -1; }
}
