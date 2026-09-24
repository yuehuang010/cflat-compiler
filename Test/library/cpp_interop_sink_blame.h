#pragma once
#include <type_traits>
#include <utility>
#include <vector>

// Probe shapes for deleted-copy blame: a sink is blamed for a deleted copy only when clang's own
// diagnostic names the argument class's copy constructor.
namespace sbl {
class Key
{
public:
    explicit Key(int v) : v_(v) {}
    Key(const Key&) = delete;
    Key(Key&& o) noexcept : v_(o.v_) {}
    int v_;
};
struct Other
{
    Other() = default;
    Other(const Other&) = delete;
};
template<class T> int one(const T& t) { return 1; }
// observe fails for a reason unrelated to copying T.
template<class T> struct Obs
{
    int n = 0;
    int observe(const T& k) { return one(k, 1); }
};
// The const T& overload fails for an unrelated reason while the T&& one binds.
template<class T> struct Obs3
{
    int n = 0;
    int observe(const T& k) { return one(k, 1); }
    int observe(T&& k) { return 7; }
};
template<class T> struct Reg
{
    int n = 0;
    struct Hold { T* p = nullptr; Other o; } hold;
    // An lvalue U fails on a missing member, not on a copy of U.
    template<class U> int lv(U&& u)
    {
        if constexpr (std::is_lvalue_reference_v<U>) return u.nonexistent();
        else return 1;
    }
    // The failing copy is of Hold, not of U.
    template<class U> int del(U&& u) { auto h2 = hold; return u.v_; }
};
template<class T> struct ByVal
{
    std::vector<T> items;
    void add(const T& t) { items.push_back(t); }
    void add(T t) { items.push_back(std::move(t)); }
    int size() const { return (int)items.size(); }
};
}
