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
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// This dependent callback alias is intentionally never instantiated. Its canonical function
// prototype is dependent and must not be handed to Clang CodeGen for ABI arrangement.

namespace cppt
{
    // M78: a header-only polymorphic owner for CFlat-defined C++ structs.
    inline int module_dtors_counter = 0;
    template <typename T>
    class HolderOf
    {
    public:
        explicit HolderOf(std::shared_ptr<T> value) : value_(value) {}
        std::shared_ptr<T> ptr() const { return value_; }
        T* operator->() const { return value_.get(); }
    private:
        std::shared_ptr<T> value_;
    };

    template <typename T>
    HolderOf<T> make_holder(std::shared_ptr<T> value)
    { return HolderOf<T>(value); }

    class ModuleBase
    {
    public:
        ModuleBase() noexcept {}
        virtual ~ModuleBase() noexcept { ++module_dtors_counter; }
        virtual int forward(int x) { return x; }

        template <typename T>
        std::shared_ptr<T> register_child(const char* name, std::shared_ptr<T> m)
        {
            names.push_back(name);
            children.push_back(m);
            return m;
        }

        template <typename T>
        std::shared_ptr<T> register_child(const char* name, HolderOf<T> h)
        { return register_child(name, h.ptr()); }

        template <typename T>
        std::shared_ptr<T> take(std::shared_ptr<T> value)
        { return value; }

        int child_count() const noexcept { return (int)children.size(); }
        int call_child(int i, int x) { return children[(size_t)i]->forward(x); }

        std::vector<std::string> names;
        std::vector<std::shared_ptr<ModuleBase>> children;
    };

    inline int run_module(const std::shared_ptr<ModuleBase>& m, int x)
    { return m->forward(x); }
    template <typename T>
    inline int module_use_count(const std::shared_ptr<T>& m)
    { return (int)m.use_count(); }
    inline int module_dtors() noexcept { return module_dtors_counter; }
    inline void reset_module_dtors() noexcept { module_dtors_counter = 0; }
    inline int m78_pick_string(std::string value) noexcept { return (int)value.size() + 10; }
    inline int m78_pick_string(const std::string& value) noexcept { return (int)value.size() + 20; }
    inline int m78_pick_pointer(int*) noexcept { return 11; }
    inline int m78_pick_pointer(char*) noexcept { return 22; }

    // M60: constructor temporaries used as C++ member-call and field receivers.
    class Builder
    {
    public:
        explicit Builder(double value) noexcept : value_(value), tag_((int)value)
        {
            ++live_;
        }
        Builder(const Builder& other) noexcept
            : value_(other.value_), weight_decay_(other.weight_decay_), tag_(other.tag_)
        {
            ++live_;
        }
        Builder(Builder&& other) noexcept
            : value_(other.value_), weight_decay_(other.weight_decay_), tag_(other.tag_)
        {
            ++live_;
        }
        ~Builder() noexcept { --live_; }

        auto wd(double value) noexcept -> decltype(*this)
        {
            weight_decay_ = value;
            return *this;
        }
        const double& wd() const noexcept { return weight_decay_; }
        double value() const noexcept { return value_; }
        Builder scaled(double factor) const { return Builder(value_ * factor); }
        static int live() noexcept { return live_; }

        int tag_;

    private:
        double value_;
        double weight_decay_ = 0;
        inline static int live_ = 0;
    };

    inline int consume(const Builder& value) noexcept
    {
        return (int)(value.value() * 10.0 + value.wd() * 100.0);
    }

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

    inline long default_long_step() noexcept { return 1; }

    struct Meter
    {
        double factor;
        long reading() const noexcept { return 3; }
        template <typename T> T reading() const noexcept { return (T)factor; }
        template <typename T> T* data_ptr() noexcept { return reinterpret_cast<T*>(&factor); }

        // M46: f performs the optional arithmetic on the C++ side while the CFlat boundary sees
        // the same long/default-wrapper shape as the plain g control.
        long f(long dim, std::optional<long> a = std::nullopt,
               std::optional<long> b = std::nullopt,
               long step = default_long_step()) const noexcept
        {
            return dim * 1000 + a.value_or(-1) * 100 + b.value_or(-1) * 10 + step;
        }

        long g(long dim, long a, long b, long step = 1) const noexcept
        {
            return dim * 1000 + a * 100 + b * 10 + step;
        }
    };

    template <typename T> T unit_of() noexcept { return (T)1; }

    template <typename V, typename... Rest>
    long pack_count(const V& v, Rest&&... rest) noexcept
    {
        return (long)sizeof...(Rest) + (long)v;
    }

    struct PackArg
    {
        long value;
        explicit PackArg(long v) noexcept : value(v) {}
        operator long() const noexcept { return value; }
    };

    inline long pack_value(const char* s) noexcept { return (long)s[0]; }
    template <typename T> long pack_value(const T& v) noexcept { return (long)v; }

    struct PackSink
    {
        long total = 0;
        template <typename... A>
        void add_all(A&&... a) noexcept { ((total += pack_value(a)), ...); }
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
    using TaggedShort = Tagged<short>;
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
        static int take_box_default(LazyBox<int> box = LazyBox<int>(7), int extra = 1) noexcept
        { return box.get() + extra; }
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

    struct ItemBagN
    {
        cppi::Tracked held;
        int n;
        ItemBagN(cppi::Tracked t, int count) : held(std::move(t)), n(count) {}
        int total() const noexcept { return held.value() + n; }
    };

    struct IntPairN
    {
        int a;
        int b;
        IntPairN(int first, int second) : a(first), b(second) {}
        int total() const noexcept { return a + b; }
    };

    struct IntegerWidths
    {
        short s;
        unsigned short us;
        char c;
        signed char sc;
        unsigned char uc;
        long long ll;
        IntegerWidths(short s_, unsigned short us_, char c_, signed char sc_,
                      unsigned char uc_, long long ll_)
            : s(s_), us(us_), c(c_), sc(sc_), uc(uc_), ll(ll_) {}
        long long total() const noexcept
        {
            return (long long)s + (long long)us + (long long)c + (long long)sc
                + (long long)uc + ll;
        }
    };

    inline int add_short(short a, int b) noexcept { return (int)a + b; }
    inline long add_mixed(cppi::Tracked t, long n) noexcept { return (long)t.value() + n; }

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

    // M40: an INHERITED member whose return type is a class-template SPECIALIZATION that nothing
    // has registered yet. The base binds its own members first, refuses this one ("returns
    // unsupported type"), and a refused base member is never cloned into the derived overload
    // set - so the derived class does not even know the name. This is at::Tensor::sizes()
    // returning c10::ArrayRef<long long> reduced to one header.
    template <typename T>
    struct RefSpan
    {
        const T* items;
        unsigned long count;
        T at(unsigned long i) const noexcept { return items[i]; }
        unsigned long size() const noexcept { return count; }
    };

    struct SpanBase
    {
        long values[3];
        SpanBase() noexcept : values{7, 8, 9} {}
        RefSpan<long> span() const noexcept { return RefSpan<long>{values, 3}; }
    };

    struct SpanDerived : SpanBase
    {
        int tag;
        SpanDerived() noexcept : tag(5) {}
        int get_tag() const noexcept { return tag; }
    };
    template <typename T> struct ListTraits
    {
        using boxed_type = std::vector<T>;
        using elem_type = T;
    };

    template <typename T> class TypedList
    {
        using boxed_type = typename ListTraits<T>::boxed_type;
        using elem_type = typename ListTraits<T>::elem_type;
        const boxed_type* boxed_ = nullptr;
        long tag_ = 0;

    public:
        TypedList(const boxed_type& b) : boxed_(&b) {}
        template <typename... A> TypedList(long tag, A&&...) : tag_(tag) {}
        long size() const noexcept { return boxed_ ? (long)boxed_->size() : tag_; }
        elem_type first() const { return (*boxed_)[0]; }
    };

    using LongList = TypedList<long>;
    inline long long_list_size(LongList l) noexcept { return l.size(); }

    namespace data {}

    template <typename D, typename T> struct Ex
    {
        D data;
        T target;
    };

    using NoTarget = int;
    using TEx = Ex<cppi::Tracked, NoTarget>;
    inline TEx make_ex(int data, int target) noexcept
    {
        return TEx{cppi::Tracked(data), target};
    }

    // M51: the default alias argument is written through another alias. The C++ return spelling
    // carries the canonical `void` argument, while TEx2 is the source-level specialization alias.
    using NoT = void;
    template <typename D, typename T = NoT> struct Ex2
    {
        D data;
        explicit Ex2(int value) : data(value) {}
    };
    using TEx2 = Ex2<cppi::Tracked>;
    inline std::vector<Ex2<cppi::Tracked, void>> make_ex2_vector()
    {
        std::vector<Ex2<cppi::Tracked, void>> result;
        result.emplace_back(17);
        return result;
    }

    // M57: class-valued brace arguments materialize a contiguous array, then build a vector-like
    // C++ parameter from its pointer and size. SpanOf also proves the generic pointer-pair path.
    template <typename T> struct SpanOf
    {
        const T* p;
        std::size_t n;
        SpanOf(const std::initializer_list<T>& list) noexcept : p(list.begin()), n(list.size()) {}
        SpanOf(const T* p_, std::size_t n_) noexcept : p(p_), n(n_) {}
        std::size_t size() const noexcept { return n; }
        int total() const noexcept
        {
            int result = 0;
            for (std::size_t i = 0; i < n; ++i) result += p[i].value();
            return result;
        }
    };

    inline int total_tracked(std::vector<cppi::Tracked> values) noexcept
    {
        int result = 0;
        for (const auto& value : values) result += value.value();
        return result;
    }
    inline int total_span(SpanOf<cppi::Tracked> values) noexcept { return values.total(); }
    inline int total_init(std::initializer_list<cppi::Tracked> values) noexcept
    {
        int result = 0;
        for (const auto& value : values) result += value.value();
        return result;
    }

    // M52: a CRTP base member template returns a specialization whose first argument is the
    // derived class. The transform has only its implicit default constructor.
    template <typename Self, typename Tr> struct M52Mapped
    {
        int result_;
        M52Mapped(Self* self, Tr) : result_(self->value_ + 1) {}
        int result() const noexcept { return result_; }
    };

    template <typename Self> struct M52BatchBase
    {
        template <typename Tr> M52Mapped<Self, Tr> map(Tr t)
        {
            return M52Mapped<Self, Tr>(static_cast<Self*>(this), t);
        }
    };

    struct M52Dataset : M52BatchBase<M52Dataset>
    {
        int value_;
        explicit M52Dataset(int value) : value_(value) {}
    };

    template <typename T> struct M52Transform
    {
        int marker_;
        int tag() const noexcept { return 2; }
    };

    // M53: a move-only nontrivial result by value. Its counted members make the result's contents
    // observable, and the caller must move-construct its auto local before the sret temporary is
    // destroyed.
    inline int m53_dtor_counter = 0;
    inline void m53_reset() noexcept { m53_dtor_counter = 0; }
    inline int m53_dtor_count() noexcept { return m53_dtor_counter; }

    struct M53Result
    {
        int first_;
        int second_;
        cppi::Tracked* marker = nullptr;
        M53Result() : first_(31), second_(47) {}
        M53Result(const M53Result&) = delete;
        M53Result(M53Result&&) noexcept = default;
        ~M53Result() { ++m53_dtor_counter; }
        std::size_t size() const noexcept { return 2; }
        int first() const noexcept { return first_; }
        int second() const noexcept { return second_; }
    };

    namespace lo
    {
        using Elem = int;
        template <typename T> struct Tr2
        {
            int marker_;
        };
        using TrA = Tr2<Elem>;

        template <typename S, typename Tr> struct Mapped2
        {
            int r_;
            Mapped2(S* s, Tr) : r_(s->v_ + 2) {}
            int r() const noexcept { return r_; }
        };

        template <typename Self> struct Base2
        {
            template <typename Tr> Mapped2<Self, Tr> map(Tr t)
            {
                return Mapped2<Self, Tr>(static_cast<Self*>(this), t);
            }
        };

        struct Ds2 : Base2<Ds2>
        {
            int v_;
            explicit Ds2(int v) : v_(v) {}
        };

        template <typename T> struct BatchTr2
        {
            Ex2<cppi::Tracked, void> apply_batch(
                std::vector<Ex2<cppi::Tracked, void>> batch) noexcept
            {
                return batch[0];
            }
        };

        template <typename S, typename Tr> struct BatchMapped2
        {
            int r_;
            Tr transform_;
            BatchMapped2(S* s, Tr t) : r_(s->v_ + 3), transform_(t) {}
            int r() const noexcept { return r_; }
            Ex2<cppi::Tracked, void> apply_batch(
                std::vector<Ex2<cppi::Tracked, void>> batch) noexcept
            {
                return transform_.apply_batch(std::move(batch));
            }
        };

        template <typename Self> struct BatchBase2
        {
            template <typename Tr> BatchMapped2<Self, Tr> map_batch(Tr t)
            {
                return BatchMapped2<Self, Tr>(static_cast<Self*>(this), t);
            }
        };

        struct BatchDs2 : BatchBase2<BatchDs2>
        {
            int v_;
            explicit BatchDs2(int v) : v_(v) {}
        };

        using BatchTrA = BatchTr2<Elem>;
    }

    // M49: a small loader-shaped fixture. The function's Dataset parameter is a nontrivial
    // class by value, its result is move-only, and the second parameter reaches a default
    // wrapper through LoaderOptions(size_t).
    struct LoaderDataset
    {
        std::vector<int> values;
        LoaderDataset() = default;
        void add(int value) noexcept { values.push_back(value); }
        std::size_t size() const noexcept { return values.size(); }
        int first() const noexcept { return values[0]; }
        // M53: return a move-only nontrivial result by value.
        M53Result tracked_values() const { return M53Result(); }
    };

    // M53: the same return through a holder that forwards member calls with operator->.
    struct M53Holder
    {
        LoaderDataset* target;
        LoaderDataset* operator->() const noexcept { return target; }
    };

    struct LoaderOptions
    {
        std::size_t batch_size_;
        LoaderOptions() : batch_size_(1) {}
        LoaderOptions(std::size_t batch_size) : batch_size_(batch_size) {}
        std::size_t batch_size() const noexcept { return batch_size_; }
    };

    struct LoaderSampler
    {
        std::size_t batch_count;
        explicit LoaderSampler(std::size_t count) : batch_count(count) {}
        std::size_t count() const noexcept { return batch_count; }
    };

    template <typename Dataset, typename Sampler>
    struct Loader
    {
        Dataset dataset;
        Sampler sampler;
        std::size_t dataset_count;
        int first_value;
        int last_value;
        std::size_t batch_count;

        Loader(Dataset dataset_, Sampler sampler_)
            : dataset(std::move(dataset_)), sampler(std::move(sampler_))
        {
            dataset_count = dataset.size();
            first_value = dataset.first();
            last_value = dataset.values.back();
            batch_count = sampler.count();
        }
    };

    template <typename Dataset, typename Sampler>
    std::unique_ptr<Loader<Dataset, Sampler>> make_loader(
        Dataset dataset, LoaderOptions options = {})
    {
        const std::size_t batch_size = options.batch_size();
        const std::size_t batch_count = batch_size == 0
            ? 0 : (dataset.size() + batch_size - 1) / batch_size;
        return std::make_unique<Loader<Dataset, Sampler>>(
            std::move(dataset), Sampler(batch_count));
    }
}

namespace cppt_inner
{
    inline long via_directive(long v) noexcept { return v * 3; }
    struct DirTag { long v; };
    template <typename T> T dir_unit() noexcept { return (T)1; }
}

namespace cppt
{
    using namespace cppt_inner;
}

namespace cppt_ambig_a
{
    inline long same(long v) noexcept { return v; }
}

namespace cppt_ambig_b
{
    inline long same(long v) noexcept { return v + 1; }
}

namespace cppt_ambig
{
    using namespace cppt_ambig_a;
    using namespace cppt_ambig_b;
}

namespace cppt
{
    // M55: a nontrivial class whose operators return it by value. `OpVec x = make_opvec(1) * 4;`
    // and `(a + 1) * 3` must move the OUTERMOST call's result into the local; live() proves the
    // temporaries are destroyed and the local exactly once.
    struct OpVec
    {
        std::vector<double> d;
        OpVec() { ++live_; }
        explicit OpVec(double x) : d{x, x} { ++live_; }
        OpVec(const OpVec& o) : d(o.d) { ++live_; }
        OpVec(OpVec&& o) noexcept : d(std::move(o.d)) { ++live_; }
        ~OpVec() { --live_; }
        OpVec operator*(double s) const { OpVec r; for (double x : d) r.d.push_back(x * s); return r; }
        double sum() const noexcept { double t = 0; for (double x : d) t += x; return t; }
        static int live() noexcept { return live_; }
        static inline int live_ = 0;
    };
    inline OpVec make_opvec(double x) { return OpVec(x); }
    inline OpVec operator+(const OpVec& a, double s) { OpVec r; for (double x : a.d) r.d.push_back(x + s); return r; }
}

namespace cppt
{
    // M58: NON-TYPE template arguments on function templates. A free template over an int,
    // an instance member template over an int, a mixed <int, typename> template, and a factory
    // returning a real std::tuple by value so std.get<0>/std.get<1> have something to index.
    template <int N> inline int nth_of(const std::vector<int>& v) { return v[N]; }

    struct SlotBox
    {
        explicit SlotBox(int base) noexcept { for (int i = 0; i < 4; ++i) slots[i] = base + i; }
        template <int N> int slot() const noexcept { return slots[N]; }
        int slots[4];
    };

    template <int N, typename T> inline T scaled(T v) { return v * N; }

    inline std::tuple<cppi::Tracked, int> make_tracked_pair(int payload)
    {
        return std::tuple<cppi::Tracked, int>(cppi::Tracked(payload), payload * 2);
    }
}

namespace cppk
{
    enum class Kind : int { A = 1, B = 2 };
    constexpr Kind kB = Kind::B;
    constexpr auto kAlias = kB;
    constexpr int kAnswer = 42;
    constexpr bool kOn = true;
    inline const int kInlineAnswer = 43;
    static constexpr char kChar = 'q';
    inline Kind kind_of(Kind k) noexcept { return k; }
}

namespace cppt
{
    // M63: two plain derived classes use different specializations of the same holder base. The
    // implementation types are nontrivial so the inherited constructors and complete-object
    // destruction remain part of the regression, not just the member-call spelling.
    namespace m63
    {
        inline int live = 0;

        struct EmbImpl
        {
            int tag;
            explicit EmbImpl(int n) noexcept : tag(100 + n) { ++live; }
            EmbImpl(const EmbImpl& other) noexcept : tag(other.tag) { ++live; }
            ~EmbImpl() noexcept { --live; }
            int forward(int x) const noexcept { return tag * 10 + x; }
        };

        struct LinImpl
        {
            int tag;
            explicit LinImpl(int n) noexcept : tag(200 + n) { ++live; }
            LinImpl(const LinImpl& other) noexcept : tag(other.tag) { ++live; }
            ~LinImpl() noexcept { --live; }
            int forward(int x) const noexcept { return tag * 10 + x; }
        };

        template <typename Impl>
        class Holder
        {
        public:
            explicit Holder(int n) noexcept : impl_(n) {}
            Impl* operator->() noexcept { return &impl_; }
            const Impl* operator->() const noexcept { return &impl_; }
            int forward(int x) const noexcept { return impl_.forward(x); }

        private:
            Impl impl_;
        };

        class Embedding : public Holder<EmbImpl>
        {
        public:
            using Holder<EmbImpl>::Holder;
        };

        class Linear : public Holder<LinImpl>
        {
        public:
            using Holder<LinImpl>::Holder;
        };

        inline int live_count() noexcept { return live; }
    }

    // M65: a class-template specialization exposes its nested Item through reference-returning
    // operator[] and front(). The counted value keeps the alias path honest for a nontrivial V.
    template <typename K, typename V>
    class Dict
    {
    public:
        class Item
        {
        public:
            Item(K k, V v) : key_(std::move(k)), value_(std::move(v)) {}
            const K& key() const noexcept { return key_; }
            V& value() noexcept { return value_; }

        private:
            K key_;
            V value_;
        };

        void insert(K k, V v) { items_.emplace_back(std::move(k), std::move(v)); }
        Item& operator[](size_t index) noexcept { return items_[index]; }
        Item& front() noexcept { return items_.front(); }
        size_t size() const noexcept { return items_.size(); }

    private:
        std::vector<Item> items_;
    };

    struct M65Value
    {
        int value;
        inline static int live_ = 0;

        explicit M65Value(int value_) noexcept : value(value_) { ++live_; }
        M65Value(const M65Value& other) noexcept : value(other.value) { ++live_; }
        M65Value(M65Value&& other) noexcept : value(other.value) { ++live_; }
        ~M65Value() noexcept { --live_; }

        int get() const noexcept { return value; }
        static int live() noexcept { return live_; }
    };

    inline Dict<std::string, double> make_m65_text() noexcept
    {
        Dict<std::string, double> result;
        result.insert("w", 1.5);
        result.insert("b", 2.5);
        return result;
    }

    inline Dict<int, double> make_m65_int() noexcept
    {
        Dict<int, double> result;
        result.insert(7, 3.5);
        result.insert(9, 4.5);
        return result;
    }

    inline Dict<int, M65Value> make_m65_counted() noexcept
    {
        Dict<int, M65Value> result;
        result.insert(4, M65Value(9));
        result.insert(8, M65Value(12));
        return result;
    }

    // M59: members inherited from a CLASS-TEMPLATE-SPECIALIZATION base. NormBase is a CRTP
    // template over a non-type argument; Norm1/Norm2 are plain (non-template) derived classes,
    // so dims()/twice() only have a body once the specific specialization is ODR-used.
    template <int D, typename Derived> struct NormBase
    {
        int dims() const noexcept { return D; }
        int twice(int v) const noexcept { return static_cast<const Derived*>(this)->scale() * v; }
    };

    struct Norm1 : NormBase<1, Norm1>
    {
        Norm1() noexcept : factor(2) {}
        int scale() const noexcept { return factor; }
        int factor;
    };

    struct Norm2 : NormBase<2, Norm2>
    {
        Norm2() noexcept : factor(3) {}
        int scale() const noexcept { return factor; }
        int factor;
    };

    struct NormHolder
    {
        NormHolder() noexcept {}
        Norm1* operator->() noexcept { return &n; }
        Norm1 n;
    };

    // M61: a libtorch-LSTM-shaped member. The element has a user destructor so the tuple result
    // exercises C++ construction/destruction at every nested level, including the defaulted
    // optional tuple parameter.
    struct LstmishX
    {
        int value;
        inline static int live_ = 0;

        explicit LstmishX(int value_ = 0) noexcept : value(value_) { ++live_; }
        LstmishX(const LstmishX& other) noexcept : value(other.value) { ++live_; }
        LstmishX(LstmishX&& other) noexcept : value(other.value) { ++live_; }
        ~LstmishX() noexcept { --live_; }
        static int live() noexcept { return live_; }
    };

    struct Lstmish
    {
        int calls = 0;

        std::tuple<LstmishX, std::tuple<LstmishX, LstmishX>> forward(
            const LstmishX& x,
            std::optional<std::tuple<LstmishX, LstmishX>> hx = {})
        {
            calls++;
            const int h = hx ? std::get<0>(*hx).value : 2;
            return std::make_tuple(
                LstmishX(x.value + 1),
                std::make_tuple(LstmishX(h), LstmishX(x.value + h)));
        }
    };
}

namespace cppt
{
    // M62: argument conversions are selected by a generated C++ forwarding wrapper.
    struct CountedModule
    {
        explicit CountedModule(int value) noexcept : n(value) { ++live_; }
        CountedModule(const CountedModule& other) noexcept : n(other.n) { ++live_; }
        CountedModule(CountedModule&& other) noexcept : n(other.n) { ++live_; }
        ~CountedModule() noexcept { --live_; }

        static int live() noexcept { return live_; }
        int n;
        inline static int live_ = 0;
    };

    // M70: non-explicit conversion into a nontrivial class by-value parameter and local.
    struct M70Counted
    {
        int n;
        M70Counted(int value) noexcept : n(value) { ++live_; ++ctors_; }
        M70Counted(const M70Counted& other) noexcept : n(other.n) { ++live_; ++ctors_; }
        M70Counted(M70Counted&& other) noexcept : n(other.n) {
            other.n = -1; ++live_; ++ctors_;
        }
        ~M70Counted() noexcept { --live_; ++dtors_; }

        static void reset() noexcept { live_ = 0; ctors_ = 0; dtors_ = 0; }
        static int live() noexcept { return live_; }
        static int ctors() noexcept { return ctors_; }
        static int dtors() noexcept { return dtors_; }
        int value() const noexcept { return n; }

    private:
        inline static int live_ = 0;
        inline static int ctors_ = 0;
        inline static int dtors_ = 0;
    };

    inline int m70_take(M70Counted value) noexcept { return value.value(); }
    inline M70Counted m70_make(int value) noexcept { return value; }

    struct AnyLike
    {
        template <typename M>
        AnyLike(M value) noexcept : n(value.n) {}

        int n;
    };

    struct SeqLike
    {
        SeqLike& push_back(std::string name, AnyLike value) noexcept
        {
            total += value.n;
            names += (int)name.size();
            return *this;
        }
        SeqLike& push_back(AnyLike value) noexcept
        {
            total += value.n;
            return *this;
        }

        int total = 0;
        int names = 0;
    };
}

namespace cppt
{
    // M64: overloads with brace arguments at different positions and public base conversions.
    struct M64Opts
    {
        int value;
        M64Opts() noexcept : value(0) {}
        explicit M64Opts(int v) noexcept : value(v) {}
    };

    struct M64ArrayRef
    {
        const long long* data;
        std::size_t count;
        M64ArrayRef(const long long* p, std::size_t n) noexcept : data(p), count(n) {}
        M64ArrayRef(const std::initializer_list<long long>& list) noexcept
            : data(list.begin()), count(list.size()) {}
        template <std::size_t N>
        M64ArrayRef(const long long (&array)[N]) noexcept : data(array), count(N) {}
    };

    inline long long m64_brace(long long high, M64ArrayRef size,
                               M64Opts options = {}) noexcept
    {
        return high * 10 + (long long)size.count + options.value;
    }
    inline long long m64_brace(long long low, long long high, M64ArrayRef size,
                               M64Opts options = {}) noexcept
    {
        return low * 100 + high * 10 + (long long)size.count + options.value;
    }

    struct M64Pad
    {
        long long value;
        M64Pad() noexcept : value(91) {}
    };

    struct M64Base
    {
        int value;
        explicit M64Base(int v) noexcept : value(v) {}
        virtual ~M64Base() = default;
        virtual int kind() const noexcept { return 1; }
    };

    struct M64Mid : M64Pad, M64Base
    {
        explicit M64Mid(int v) noexcept : M64Pad(), M64Base(v) {}
    };

    struct M64Leaf : M64Mid
    {
        explicit M64Leaf(int v) noexcept : M64Mid(v) {}
        int kind() const noexcept override { return 3; }
    };

    struct M64CtorConsumer
    {
        M64Base* base;
        int step;
        M64CtorConsumer(M64Base& b, int s) noexcept : base(&b), step(s) {}
        int probe() const noexcept { return base->kind() * 1000 + base->value * 10 + step; }
    };

    struct M64MemberConsumer
    {
        int marker;
        M64MemberConsumer() noexcept : marker(0) {}
        int probe(M64Base* base) const noexcept
        {
            return base->kind() * 1000 + base->value * 10;
        }
    };

    inline int m64_free(const M64Base& base) noexcept { return base.kind() + base.value; }
    inline int m64_virtual_ref(const M64Base& base) noexcept { return base.kind(); }
}

namespace cppt
{
    // M66: heterogeneous brace elements are converted by C++ into one destination class. The
    // re-exported slice also keeps the namespace-level using-directive path in the same call.
    namespace m66_base
    {
        struct M66Slice
        {
            long long start;
            long long stop;
            M66Slice(long long start_ = 0, long long stop_ = 1000) noexcept
                : start(start_), stop(stop_) {}
        };
    }

    namespace m66_reexport
    {
        using namespace m66_base;
    }

    struct M66None {};

    struct M66Index
    {
        int kind;
        long long value;
        inline static int live_ = 0;

        M66Index(long long value_) noexcept : kind(1), value(value_) { ++live_; }
        M66Index(m66_base::M66Slice slice) noexcept : kind(2), value(slice.stop) { ++live_; }
        M66Index(M66None) noexcept : kind(3), value(0) { ++live_; }
        M66Index(const M66Index& other) noexcept : kind(other.kind), value(other.value) { ++live_; }
        M66Index(M66Index&& other) noexcept : kind(other.kind), value(other.value) { ++live_; }
        ~M66Index() noexcept { --live_; }

        static int live() noexcept { return live_; }
    };

    inline int m66_collect(std::initializer_list<M66Index> values) noexcept
    {
        int result = 0;
        for (const auto& value : values) result += value.kind * 100 + (int)value.value;
        return result;
    }
}

namespace cppc
{
    // M67: namespace-scope floating and integer constexpr values, including long double's
    // target-specific conversion to CFlat double.
    constexpr double m67Double = 6.5;
    constexpr float m67Float = 0.5f;
    constexpr long double m67LongDouble = 1.25L;
    constexpr int m67Int = 4;

    struct M67Statics
    {
        static constexpr double kScale = 4.0;
        static constexpr float kHalf = 0.5f;
        static constexpr int kCount = 3;
    };

    inline float m67_take_float(float value) noexcept { return value; }
}

namespace cppt
{
    // M69: one foreign identity spelling across extractor and backend paths. The two CRTP
    // specializations deliberately have different non-type values, and the byte view exercises
    // a multi-word primitive template argument through both inheritance and a free function.
    namespace m69
    {
        template <int N, class T>
        struct M69NormBase
        {
            int n() const noexcept { return N; }
            T tag() const;
        };

        struct M69Pos : M69NormBase<1, M69Pos>
        {
            int marker;
            M69Pos() noexcept : marker(11) {}
        };

        struct M69Neg : M69NormBase<-1, M69Neg>
        {
            int marker;
            M69Neg() noexcept : marker(22) {}
        };

        template <class T>
        struct M69ArrayRef
        {
            const T* data;
            std::size_t count;
            std::size_t size() const noexcept { return count; }
        };

        struct M69Bytes : M69ArrayRef<unsigned char>
        {
            int marker;
            M69Bytes(const unsigned char* p, std::size_t n) noexcept
                : marker(33)
            {
                data = p;
                count = n;
            }
        };

        inline int norm_pos(const M69NormBase<1, M69Pos>& value) noexcept { return value.n(); }
        inline int norm_neg(const M69NormBase<-1, M69Neg>& value) noexcept { return value.n(); }
        inline int bytes_as_base(const M69ArrayRef<unsigned char>& value) noexcept
        { return (int)value.size(); }
    }

    inline Box<unsigned long> m72_make_unsigned_long(unsigned long value) noexcept
    { return Box<unsigned long>(value); }
    inline unsigned long m72_take_unsigned_long(const Box<unsigned long>& value) noexcept
    { return value.get(); }
    inline Box<size_t> m72_make_size_t(size_t value) noexcept
    { return Box<size_t>(value); }
    inline Box<unsigned> m72_make_unsigned(unsigned value) noexcept
    { return Box<unsigned>(value); }
    inline Box<long double> m72_make_long_double(long double value) noexcept
    { return Box<long double>(value); }
    inline std::vector<size_t> m72_size_t_vector_roundtrip(
        const std::vector<size_t>& value) noexcept
    { return value; }

    // M77: inherited member-template lookup from a CFlat-defined receiver.
    class Registry
    {
    public:
        Registry() noexcept {}
        template <typename T>
        int reg(const char* name, T value) noexcept
        { return (int)value + (name[0] == 'x' ? 1 : 2); }
        int plain() const noexcept { return 7; }
    };

    // M76: polymorphic class templates used as CFlat-defined struct bases.
    namespace m76
    {
    template <typename T>
    class Holder
    {
    public:
        Holder() noexcept : value(T(9)) {}
        explicit Holder(T v) noexcept : value(v) {}
        virtual ~Holder() noexcept { ++dtors_; }
        virtual T doubled() const noexcept { return value + value; }
        T value;

        static int dtors() noexcept { return dtors_; }
        static void reset() noexcept { dtors_ = 0; }

    private:
        inline static int dtors_ = 0;
    };

    template <typename T>
    inline T call_doubled(const Holder<T>* h) noexcept
    { return h->doubled(); }

    inline int holder_int_dtors() noexcept { return Holder<int>::dtors(); }
    inline int holder_double_dtors() noexcept { return Holder<double>::dtors(); }
    inline void reset_holder_int_dtors() noexcept { Holder<int>::reset(); }
    inline void reset_holder_double_dtors() noexcept { Holder<double>::reset(); }

    template <typename T>
    class Shape2
    {
    public:
        virtual ~Shape2() noexcept = default;
        virtual T area() const noexcept = 0;
    };

    template <typename T>
    inline T call_area2(const Shape2<T>* shape) noexcept
    { return shape->area(); }
    }
}

// Section M83: a class-template specialization named through a GLOBAL-scope alias. The undotted
// spelling is the one that never reached the C++ type request; cppt covers the namespaced forms.
template <class T> struct M83Global { T value; };
using M83GlobalAlias = M83Global<int>;
using M83GlobalAlias2 = M83Global<int>;
typedef M83Global<double> M83GlobalTypedef;
inline int m83_take_global(const M83Global<int>& v) noexcept { return v.value + 1; }
