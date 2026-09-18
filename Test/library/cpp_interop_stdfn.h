// Section M100: std::function in every position a member or a return can put it. The free
// function forms next to them are the shapes section M7f already covered, kept here so both
// sides of the same signature are exercised by one header.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace cppfn
{
    inline int apply_val(std::function<int(int)> f, int x) { return f(x); }
    inline int apply_ref(const std::function<int(int)>& f, int x) { return f(x); }
    inline int apply_rv(std::function<int(int)>&& f, int x) { return f(x); }

    inline std::function<int(int)> make_adder(int n) { return [n](int x) { return x + n; }; }

    class Plain
    {
    public:
        Plain() : bias_(100) {}
        explicit Plain(std::function<int(int)> f) : bias_(f(10)) {}
        int run_val(std::function<int(int)> g, int x) { return bias_ + g(x); }
        int run_ref(const std::function<int(int)>& g, int x) { return bias_ + g(x); }
        int run_rv(std::function<int(int)>&& g, int x) { return bias_ + g(x); }
        static int run_static(std::function<int(int)> g, int x) { return 1000 + g(x); }
        std::function<int(int)> get_adder(int n) { return [n](int x) { return x + n; }; }
        static std::function<int(int)> make_scaler(int n) { return [n](int x) { return x * n; }; }
        int bias() const { return bias_; }

    private:
        int bias_;
    };

    // The callable outlives the CFlat frame that made it: set_handler stores it, fire() runs it
    // after that frame is gone.
    class Holder
    {
    public:
        Holder() : h_(nullptr) {}
        void set_handler(std::function<int(int)> g) { h_ = g; }
        int fire(int x) { return h_ ? h_(x) : -1; }

    private:
        std::function<int(int)> h_;
    };

    inline int call_void(std::function<void()> f) { f(); return 7; }
    class VoidHolder
    {
    public:
        VoidHolder() : pad_(0) {}
        int go(std::function<void()> f) { f(); return 5 + pad_; }

    private:
        int pad_;
    };

    inline int call_str(std::function<int(const std::string&)> f) { return f(std::string("abcd")); }
    class StrHolder
    {
    public:
        StrHolder() : pad_(0) {}
        int go(std::function<int(const std::string&)> f) { return f(std::string("abcde")) + pad_; }

    private:
        int pad_;
    };

    // Constructor parameters spelled as REFERENCES: these keep the Itanium pointer shape, and
    // binding them by value emitted a call whose argument type disagreed with the declaration.
    class CtorRef
    {
    public:
        CtorRef() : v_(1) {}
        explicit CtorRef(const std::function<int(int)>& f) : v_(f(2)) {}
        CtorRef(std::function<int(int)>&& f, int bump) : v_(f(3) + bump) {}
        int v() const { return v_; }

    private:
        int v_;
    };

    // A spelling that merely CONTAINS a std::function is NOT one: these three positions must keep
    // refusing, and Test/errors/err_cpp_std_function_nested.cb pins each refusal.
    inline std::vector<std::function<int(int)>> vec_of_fn() { return {}; }
    // The FREE-parameter position of the same shape: it used to bind as the inner std::function
    // and pass a callable where a vector was expected.
    inline int take_vec_free(std::vector<std::function<int(int)>> v) { return (int)v.size() + 1; }
    class NestedHolder
    {
    public:
        NestedHolder() : pad_(0) {}
        int take_vec(std::vector<std::function<int(int)>> v) { return (int)v.size() + pad_; }
        std::vector<std::function<int(int)>> get_vec() { return {}; }

    private:
        int pad_;
    };

    // A std::function over a signature cflat cannot map: the member stays refused, and the
    // refusal names the whole type because that is what has no CFlat spelling.
    class VecHolder
    {
    public:
        VecHolder() : pad_(0) {}
        int go(std::function<int(std::vector<int>)> f) { return f(std::vector<int>()) + pad_; }

    private:
        int pad_;
    };
}
