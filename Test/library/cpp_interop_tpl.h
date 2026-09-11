// C++ fixture for M5b - CLASS TEMPLATES instantiated from CFlat. Header-only on purpose: a
// specialization CFlat asks for has no symbol anywhere until the compiler instantiates it, so
// every body below has to be emitted into the companion module clang CodeGen hands back.
//
// Coverage: a constructor taking T, get/set, a static instance counter (an inline static data
// member, one per specialization), operator[] returning T by value, operator==, a specialization
// over a nontrivial C++ class (cppi::Tracked), and a class whose destructor is DEFAULTED but
// nontrivial because a member has one.
#pragma once

#include "cpp_interop_basic.h"
#include <initializer_list>
#include <string>
#include <vector>

// This dependent callback alias is intentionally never instantiated. Its canonical function
// prototype is dependent and must not be handed to Clang CodeGen for ABI arrangement.

namespace cppt
{
    template <typename T>
    struct DependentAbiShape
    {
        typedef T (*Callback)(T, int);
    };

    inline int dependent_abi_neighbor() noexcept { return 6; }

    // A NAMESPACE ALIAS (simdjson's `namespace ondemand = arm64::ondemand`): CFlat spells the
    // alias, clang resolves it, and the canonical name aliases onto the one registration.
    namespace impl_detail
    {
        struct Payload
        {
            int v;
            Payload() noexcept : v(3) {}
            int get() const noexcept { return v; }
        };
    }
    namespace via_alias = impl_detail;

    template <typename T>
    class Box
    {
    public:
        explicit Box(T v) noexcept : v_(v) { ++live_; ++ctors_; }
        Box(const Box& o) noexcept : v_(o.v_) { ++live_; ++ctors_; }
        ~Box() noexcept { --live_; ++dtors_; }

        T get() const noexcept { return v_; }
        void set(T v) noexcept { v_ = v; }
        // Index is ignored: a one-slot box, present only so CFlat's index expression has an
        // operator[] to bind to. Returns T BY VALUE (the by-reference form is covered by
        // std::vector's operator[] in section M9).
        T operator[](int index) const noexcept { return v_; }
        bool operator==(const Box& o) const noexcept { return v_ == o.v_; }

        static int live() noexcept { return live_; }
        static int ctors() noexcept { return ctors_; }
        static int dtors() noexcept { return dtors_; }
        static void reset() noexcept { ctors_ = 0; dtors_ = 0; }

    private:
        T v_;
        inline static int live_ = 0;
        inline static int ctors_ = 0;
        inline static int dtors_ = 0;
    };

    template <typename T> T twice(T v) noexcept { return v + v; }
    template <typename T> Box<T> make_box(T v) noexcept { return Box<T>(v); }
    template <typename T> long count_chars(const T* s) noexcept
    {
        long n = 0;
        while (s[n]) ++n;
        return n;
    }

    struct Scaler
    {
        long factor;
        explicit Scaler(long f) noexcept : factor(f) {}
        template <typename T> T scaled(T v) const noexcept { return v * (T)factor; }
        template <typename T> static long from(T v) noexcept { return (long)v * 10; }
    };

    // Counter readers as FREE inline functions. A specialization's static member can only be
    // named in a DECLARATION today (`cppt.Box<int> b`), which is what drives instantiation, so a
    // test cannot write `cppt.Box<int>.live()` in expression position.
    inline int box_int_live() noexcept   { return Box<int>::live(); }
    inline int box_int_ctors() noexcept  { return Box<int>::ctors(); }
    inline int box_int_dtors() noexcept  { return Box<int>::dtors(); }
    inline void box_int_reset() noexcept { Box<int>::reset(); }
    inline int box_double_live() noexcept { return Box<double>::live(); }

    // This pair deliberately exposes LazyBox<int> only through member signatures. The CFlat
    // spelling of LazyBox<int> must request the specialization before those members are bound.
    template <typename T>
    class LazyBox
    {
    public:
        explicit LazyBox(T v) noexcept : v_(v) {}
        ~LazyBox() noexcept {}
        T get() const noexcept { return v_; }

    private:
        T v_;
    };

    template <typename T, typename Tag = void>
    class Tagged
    {
    public:
        Tagged() noexcept : v_() {}
        explicit Tagged(T v) noexcept : v_(v) {}
        T get() const noexcept { return v_; }
        std::string name() const { return "tagged"; }

    private:
        T v_;
    };

    using TaggedInt = Tagged<int>;
    namespace deep { using TaggedLong = Tagged<long>; }
    // Used only as a temporary built straight into a call argument (never declared first).
    using TaggedReal = Tagged<double>;
    inline double tagged_real_value(TaggedReal t) noexcept { return t.get(); }

    // A borrowed (pointer, count) view, like c10::ArrayRef; alias met first as a temporary.
    template <typename T>
    class View
    {
    public:
        View(const T* p, unsigned long n) noexcept : p_(p), n_(n) {}
        T sum() const noexcept { T s = T(); for (unsigned long i = 0; i < n_; ++i) s += p_[i]; return s; }

    private:
        const T* p_;
        unsigned long n_;
    };
    using LongView = View<long>;
    inline long long_view_sum(LongView v) noexcept { return v.sum(); }

    // Trivially copyable for calls (returned in registers), yet a C++ class with constructors.
    class Slot
    {
    public:
        Slot() noexcept : v_(0) {}
        explicit Slot(int v) noexcept : v_(v) {}
        int get() const noexcept { return v_; }

    private:
        int v_;
    };

    class LazyBoxFactory
    {
    public:
        LazyBox<int> make_box(int v) const noexcept { return LazyBox<int>(v); }
        int take_box(const LazyBox<int>& box) const noexcept { return box.get(); }
        const LazyBoxFactory& self() const noexcept { return *this; }
        Slot make_slot(int v) const noexcept { return Slot(v); }

    private:
        int marker_;
    };

    // A declaration-only specialization keeps the original refusal available when the lazy
    // request cannot find a complete C++ class definition.
    template <typename T> class NeverDefined;
    class LazyRefused
    {
    public:
        NeverDefined<int> missing() const noexcept;

    private:
        int marker_;
    };

    // The destructor is DEFAULTED, so there is no body in the header at all - but the member has
    // one, which makes the implicit destructor nontrivial and REQUIRED. As a template, the
    // specialization request forces Sema to define it and CodeGen to emit it.
    template <typename T>
    class Holder
    {
    public:
        // Takes an int and builds the member from it: a by-reference or by-value parameter of a
        // nontrivial class is not selectable at a CONSTRUCTOR yet (see M5B_REPORT.md).
        explicit Holder(int v) noexcept : t_(v) {}
        ~Holder() = default;
        int value() const noexcept { return t_.value(); }

    private:
        T t_;
    };

    inline long sum_il(std::initializer_list<long> xs) noexcept
    {
        long s = 0;
        for (long x : xs) s += x;
        return s;
    }

    inline double sum_ild(std::initializer_list<double> xs) noexcept
    {
        double s = 0;
        for (double x : xs) s += x;
        return s;
    }

    template <typename T>
    class Span
    {
    public:
        Span(std::initializer_list<T> il) noexcept : n_(il.size()) {}
        Span(const T* p, unsigned long n) noexcept : n_(n) {}
        unsigned long size() const noexcept { return n_; }

    private:
        unsigned long n_;
    };

    inline unsigned long span_size(Span<long> s) noexcept { return s.size(); }
    template <typename T> unsigned long span_count(Span<T> s) noexcept { return s.size(); }

    struct Shape
    {
        std::vector<long> dims;
        Shape(std::initializer_list<long> d) : dims(d) {}
        long rank() const noexcept { return (long)dims.size(); }
        long dim(long i) const noexcept { return dims[(unsigned long)i]; }
    };

    struct Named
    {
        std::string name;
        long id;
        Named(const char* n, long i) : name(n), id(i) {}
        const char* cname() const noexcept { return name.c_str(); }
    };

    struct Defaulted
    {
        std::vector<int> v;
        Defaulted() = default;
        ~Defaulted() = default;
        long n() const noexcept { return (long)v.size(); }
    };

    inline long shape_rank(const Shape& s) noexcept { return s.rank(); }

    struct BoolBits { bool ready : 1; bool mode : 1; };

    struct InheritedBase
    {
        explicit InheritedBase(int first, int second) noexcept
            : value_(first + second) {}
        int get() const noexcept { return value_; }

    private:
        int value_;
    };

    struct InheritedCtorHolder : InheritedBase
    {
        using InheritedBase::InheritedBase;
        const InheritedBase* operator->() const noexcept { return this; }
    };

    // A CLASS TEMPLATE used as a base, CRTP style. Clang's qualified name for a specialization
    // decl drops the arguments, so every TplBase<T> shares one CFlat identity - the derived
    // classes must still inherit base_tag(), and the SECOND specialization must not lose it.
    template <typename T>
    struct TplBase
    {
        int slot;
        int base_tag() const noexcept { return 7; }
    };

    struct TplHolderA : TplBase<TplHolderA>
    {
        int own_a() const noexcept { return 5; }
    };

    struct TplHolderB : TplBase<TplHolderB>
    {
        int own_b() const noexcept { return 9; }
    };

    // A dependent member is only instantiated when it is odr-used, and clang emits no body for
    // an uninstantiated one. Force both specializations here so the base HAS a bindable member
    // for the derived classes to inherit.
    inline int force_tpl_base_instantiation() noexcept
    {
        TplHolderA a{};
        TplHolderB b{};
        return a.base_tag() + b.base_tag();
    }

    // A NONTRIVIAL class crossing a CONSTRUCTOR boundary by value, fed straight from a function
    // that returns one by value: the argument must be move-constructed into the caller-owned
    // temp, never byte-copied over the returned temp's own buffer.
    struct ItemBag
    {
        cppi::Tracked held;
        explicit ItemBag(cppi::Tracked t) : held(std::move(t)) {}
        int value() const noexcept { return held.value(); }
    };

    // An unmappable parameter that is DEFAULTED must not sink the whole member: the shorter
    // arity never names the type, so `pick()` binds through the generated default wrapper.
    struct Picker
    {
        int marker;
        int pick(const std::vector<int>& (*chooser)() = nullptr) const noexcept
        {
            return chooser == nullptr ? 42 : 0;
        }
        int scale(std::vector<int> weights = {}) const noexcept
        {
            return weights.empty() ? 11 : (int)weights.size();
        }
    };
}
