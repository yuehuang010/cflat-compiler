#pragma once
#include <memory>
#include <string>
#include <utility>
namespace cppsf {
struct ModBase { virtual ~ModBase() = default; };
template<class M, class = decltype(&M::forward)>
struct AnyMod {
    std::shared_ptr<M> value;
    explicit AnyMod(std::shared_ptr<M> m) : value(std::move(m)) {}
    int run() { return value->forward(1); }
};
template<class M, class = decltype(std::declval<M&>().forward(1))>
struct CallMod {
    std::shared_ptr<M> value;
    explicit CallMod(std::shared_ptr<M> m) : value(std::move(m)) {}
    int run() { return value->forward(1); }
};
template<class M>
struct FreeMod {
    std::shared_ptr<M> value;
    explicit FreeMod(std::shared_ptr<M> m) : value(std::move(m)) {}
    int run() { return value->forward(1); }
};
struct Container {
    int result = 0;
    template<class M> void push_back(std::string, std::shared_ptr<M> m) {
        AnyMod<M> holder(std::move(m));
        result = holder.run();
    }
    template<class M> void push_call(std::string, std::shared_ptr<M> m) {
        CallMod<M> holder(std::move(m));
        result = holder.run();
    }
    template<class M> void push_free(std::string, std::shared_ptr<M> m) {
        FreeMod<M> holder(std::move(m));
        result = holder.run();
    }
};
struct HidingBase {
    int size() const { return 7; }
    int size(int x) const { return x + 10; }
    int read() const { return 8; }
    int same(int x) { return x + 100; }
private:
    int secret() { return 900; }
};
struct HidingLeft { int choose(int) const { return 31; } };
struct HidingRight { int choose(double) const { return 42; } };
struct HidingMulti : HidingLeft, HidingRight {};
template<class T> int inherited_size(T& value) { return value.size(1); }
template<class T> int inherited_read(const T& value) { return value.read(); }
template<class T> int same_call(T& value) { return value.same(5); }
template<class T> int inherited_right(const T& value) { return value.choose(2.5); }
template<class T> void accept_keywords(const T&) {}
}
