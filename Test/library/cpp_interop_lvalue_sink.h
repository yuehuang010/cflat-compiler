#pragma once
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
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

    // Temporaries into an inherited forwarding variadic constructor of a class-template base
    // (libtorch `Sequential(Linear(2, 3), Tanh(), ...)`): each stays an rvalue, nothing copied.
    struct FwdCount { inline static int copies = 0, moves = 0, dtors = 0, rvalues = 0; };
    class Part
    {
    public:
        explicit Part(int x) : v(x) {}
        Part(const Part& o) : v(o.v) { ++FwdCount::copies; }
        Part(Part&& o) noexcept : v(o.v) { ++FwdCount::moves; }
        ~Part() { ++FwdCount::dtors; }
        int v;
    };
    struct PartSum
    {
        int sum = 0;
        template<class... P> explicit PartSum(P&&... p)
        {
            ((sum += p.v, FwdCount::rvalues += std::is_rvalue_reference_v<P&&> ? 1 : 0), ...);
        }
    };
    template<class C> class FwdHolder
    {
    public:
        template<class H, class... T>
        explicit FwdHolder(H&& h, T&&... t)
            : impl_(new C(std::forward<H>(h), std::forward<T>(t)...)) {}
        FwdHolder(FwdHolder&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
        FwdHolder(const FwdHolder&) = delete;
        ~FwdHolder() { delete impl_; }
        int sum() const { return impl_->sum; }
    private:
        C* impl_;
    };
    class PartSeq : public FwdHolder<PartSum> { public: using FwdHolder<PartSum>::FwdHolder; };
    inline void reset_fwd() { FwdCount::copies = FwdCount::moves = FwdCount::dtors = FwdCount::rvalues = 0; }
    inline int fwd_copies() { return FwdCount::copies; }
    inline int fwd_moves() { return FwdCount::moves; }
    inline int fwd_dtors() { return FwdCount::dtors; }
    inline int fwd_rvalues() { return FwdCount::rvalues; }

    // libtorch OrderedDict shape: a nested Item declared in the class template and defined after
    // it, members defined out of line WITHOUT `inline` (no library symbol for this specialization).
    template<class K, class V> class NamedDict
    {
    public:
        class Item;
        NamedDict() = default;
        Item& operator[](std::size_t index);
        std::size_t size() const;
        void insert(K k, V v);
    private:
        std::vector<Item> items_;
    };
    template<class K, class V> class NamedDict<K, V>::Item
    {
    public:
        Item(K k, V v) : pair_(std::move(k), std::move(v)) {}
        const K& key() const noexcept { return pair_.first; }
        V& value() noexcept { return pair_.second; }
    private:
        std::pair<K, V> pair_;
    };
    template<class K, class V>
    typename NamedDict<K, V>::Item& NamedDict<K, V>::operator[](std::size_t index) { return items_[index]; }
    template<class K, class V> std::size_t NamedDict<K, V>::size() const { return items_.size(); }
    template<class K, class V> void NamedDict<K, V>::insert(K k, V v) { items_.emplace_back(std::move(k), std::move(v)); }
    struct Weight { int v; };
    class Net
    {
    public:
        Net() { params_.insert("w1", Weight{ 4 }); params_.insert("bias", Weight{ 9 }); }
        NamedDict<std::string, Weight> named() const { return params_; }
        // A member template whose loop test is a rewritten `!=` (C++20 `!(a == b)`).
        template<class R = int, class... In> R forward(In&&... in)
        {
            R acc = (static_cast<R>(in) + ... + 0);
            for (auto it = stages_.begin(); it != stages_.end(); ++it) acc = acc * *it;
            return acc;
        }
    private:
        NamedDict<std::string, Weight> params_;
        std::vector<int> stages_{ 2, 3 };
    };
}
