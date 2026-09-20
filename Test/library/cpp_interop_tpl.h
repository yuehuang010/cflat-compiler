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
#include <atomic>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// This dependent callback alias is intentionally never instantiated. Its canonical function
// prototype is dependent and must not be handed to Clang CodeGen for ABI arrangement.

namespace cppt
{
    inline int constref_string_param(std::string *const &p)
    { return (int)p->size() * 10 + 5; }

    inline int constref_int_param(int *const &p)
    { return *p * 10 + 6; }

    inline std::string *const &constref_string_return()
    {
        static std::string text = "hello";
        static std::string *value = &text;
        return value;
    }

    inline int *const &constref_int_return()
    {
        static int value = 8;
        static int *slot = &value;
        return slot;
    }

    struct ConstRefStringHolder
    {
        std::string *slot;
        ConstRefStringHolder() : slot(nullptr) {}
        int param(std::string *const &p) { return (int)p->size() * 10 + 5; }
        std::string *const &out() const { return slot; }
        void set(std::string *p) { slot = p; }
    };

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
    inline int shared_pick_string(std::string value) noexcept { return (int)value.size() + 10; }
    inline int shared_pick_string(const std::string& value) noexcept { return (int)value.size() + 20; }
    inline int shared_pick_pointer(int*) noexcept { return 11; }
    inline int shared_pick_pointer(char*) noexcept { return 22; }

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
        // Namespace-scope OBJECT and function: the object is the spelling a NESTED alias lost.
        inline long payload_obj = 57;
        inline long payload_fn() noexcept { return 59; }
    }
    namespace via_alias = impl_detail;

    template <typename T>
    class Box
    {
    public:
        Box() noexcept : v_() { ++live_; ++ctors_; }
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

    template <typename T>
    class CtorPair
    {
    public:
        CtorPair(T first, int second) : first_(first), second_(second) {}
        T first() const noexcept { return first_; }
        int second() const noexcept { return second_; }
    private:
        T first_;
        int second_;
    };

    inline int take_box_arg(Box<int> value) noexcept { return value.get(); }

    class CtorTempSink
    {
    public:
        CtorTempSink() noexcept {}
        int take(Box<int> value) noexcept { return value.get() + 1; }
        template <typename T>
        int take_any(T value) noexcept { return value.get() + 2; }
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
    template <typename Self, typename Tr> struct CrtpMapped
    {
        int result_;
        CrtpMapped(Self* self, Tr) : result_(self->value_ + 1) {}
        int result() const noexcept { return result_; }
    };

    template <typename Self> struct CrtpBatchBase
    {
        template <typename Tr> CrtpMapped<Self, Tr> map(Tr t)
        {
            return CrtpMapped<Self, Tr>(static_cast<Self*>(this), t);
        }
    };

    struct CrtpDataset : CrtpBatchBase<CrtpDataset>
    {
        int value_;
        explicit CrtpDataset(int value) : value_(value) {}
    };

    template <typename T> struct CrtpTransform
    {
        int marker_;
        int tag() const noexcept { return 2; }
    };

    // M53: a move-only nontrivial result by value. Its counted members make the result's contents
    // observable, and the caller must move-construct its auto local before the sret temporary is
    // destroyed.
    inline int autosret_dtor_counter = 0;
    inline void autosret_reset() noexcept { autosret_dtor_counter = 0; }
    inline int autosret_dtor_count() noexcept { return autosret_dtor_counter; }

    struct AutoSretResult
    {
        int first_;
        int second_;
        cppi::Tracked* marker = nullptr;
        AutoSretResult() : first_(31), second_(47) {}
        AutoSretResult(const AutoSretResult&) = delete;
        AutoSretResult(AutoSretResult&&) noexcept = default;
        ~AutoSretResult() { ++autosret_dtor_counter; }
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
        AutoSretResult tracked_values() const { return AutoSretResult(); }
    };

    // M53: the same return through a holder that forwards member calls with operator->.
    struct AutoSretHolder
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

// Top-level alias of the SAME target as cppt.via_alias: the shape that already resolved
// objects, kept here as the accept half of the nested-alias pair.
namespace cppt_via_top = cppt::impl_detail;

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
    namespace sibbase
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

    struct NestedItemValue
    {
        int value;
        inline static int live_ = 0;

        explicit NestedItemValue(int value_) noexcept : value(value_) { ++live_; }
        NestedItemValue(const NestedItemValue& other) noexcept : value(other.value) { ++live_; }
        NestedItemValue(NestedItemValue&& other) noexcept : value(other.value) { ++live_; }
        ~NestedItemValue() noexcept { --live_; }

        int get() const noexcept { return value; }
        static int live() noexcept { return live_; }
    };

    inline Dict<std::string, double> make_nesteditem_text() noexcept
    {
        Dict<std::string, double> result;
        result.insert("w", 1.5);
        result.insert("b", 2.5);
        return result;
    }

    inline Dict<int, double> make_nesteditem_int() noexcept
    {
        Dict<int, double> result;
        result.insert(7, 3.5);
        result.insert(9, 4.5);
        return result;
    }

    inline Dict<int, NestedItemValue> make_nesteditem_counted() noexcept
    {
        Dict<int, NestedItemValue> result;
        result.insert(4, NestedItemValue(9));
        result.insert(8, NestedItemValue(12));
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
    struct BraceTmpCounted
    {
        int n;
        BraceTmpCounted(int value) noexcept : n(value) { ++live_; ++ctors_; }
        BraceTmpCounted(const BraceTmpCounted& other) noexcept : n(other.n) { ++live_; ++ctors_; }
        BraceTmpCounted(BraceTmpCounted&& other) noexcept : n(other.n) {
            other.n = -1; ++live_; ++ctors_;
        }
        ~BraceTmpCounted() noexcept { --live_; ++dtors_; }

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

    inline int bracetmp_take(BraceTmpCounted value) noexcept { return value.value(); }
    inline BraceTmpCounted bracetmp_make(int value) noexcept { return value; }

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
    struct BracePosOpts
    {
        int value;
        BracePosOpts() noexcept : value(0) {}
        explicit BracePosOpts(int v) noexcept : value(v) {}
    };

    struct BracePosArrayRef
    {
        const long long* data;
        std::size_t count;
        BracePosArrayRef(const long long* p, std::size_t n) noexcept : data(p), count(n) {}
        BracePosArrayRef(const std::initializer_list<long long>& list) noexcept
            : data(list.begin()), count(list.size()) {}
        template <std::size_t N>
        BracePosArrayRef(const long long (&array)[N]) noexcept : data(array), count(N) {}
    };

    inline long long bracepos_brace(long long high, BracePosArrayRef size,
                               BracePosOpts options = {}) noexcept
    {
        return high * 10 + (long long)size.count + options.value;
    }
    inline long long bracepos_brace(long long low, long long high, BracePosArrayRef size,
                               BracePosOpts options = {}) noexcept
    {
        return low * 100 + high * 10 + (long long)size.count + options.value;
    }

    struct BracePosPad
    {
        long long value;
        BracePosPad() noexcept : value(91) {}
    };

    struct BracePosBase
    {
        int value;
        explicit BracePosBase(int v) noexcept : value(v) {}
        virtual ~BracePosBase() = default;
        virtual int kind() const noexcept { return 1; }
    };

    struct BracePosMid : BracePosPad, BracePosBase
    {
        explicit BracePosMid(int v) noexcept : BracePosPad(), BracePosBase(v) {}
    };

    struct BracePosLeaf : BracePosMid
    {
        explicit BracePosLeaf(int v) noexcept : BracePosMid(v) {}
        int kind() const noexcept override { return 3; }
    };

    struct BracePosCtorConsumer
    {
        BracePosBase* base;
        int step;
        BracePosCtorConsumer(BracePosBase& b, int s) noexcept : base(&b), step(s) {}
        int probe() const noexcept { return base->kind() * 1000 + base->value * 10 + step; }
    };

    struct BracePosMemberConsumer
    {
        int marker;
        BracePosMemberConsumer() noexcept : marker(0) {}
        int probe(BracePosBase* base) const noexcept
        {
            return base->kind() * 1000 + base->value * 10;
        }
    };

    inline int bracepos_free(const BracePosBase& base) noexcept { return base.kind() + base.value; }
    inline int bracepos_virtual_ref(const BracePosBase& base) noexcept { return base.kind(); }
}

namespace cppt
{
    // M66: heterogeneous brace elements are converted by C++ into one destination class. The
    // re-exported slice also keeps the namespace-level using-directive path in the same call.
    namespace hetbrace_base
    {
        struct HetBraceSlice
        {
            long long start;
            long long stop;
            HetBraceSlice(long long start_ = 0, long long stop_ = 1000) noexcept
                : start(start_), stop(stop_) {}
        };
    }

    namespace hetbrace_reexport
    {
        using namespace hetbrace_base;
    }

    struct HetBraceNone {};

    struct HetBraceIndex
    {
        int kind;
        long long value;
        inline static int live_ = 0;

        HetBraceIndex(long long value_) noexcept : kind(1), value(value_) { ++live_; }
        HetBraceIndex(hetbrace_base::HetBraceSlice slice) noexcept : kind(2), value(slice.stop) { ++live_; }
        HetBraceIndex(HetBraceNone) noexcept : kind(3), value(0) { ++live_; }
        HetBraceIndex(const HetBraceIndex& other) noexcept : kind(other.kind), value(other.value) { ++live_; }
        HetBraceIndex(HetBraceIndex&& other) noexcept : kind(other.kind), value(other.value) { ++live_; }
        ~HetBraceIndex() noexcept { --live_; }

        static int live() noexcept { return live_; }
    };

    inline int hetbrace_collect(std::initializer_list<HetBraceIndex> values) noexcept
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
    constexpr double constexprDouble = 6.5;
    constexpr float constexprFloat = 0.5f;
    constexpr long double constexprLongDouble = 1.25L;
    constexpr int constexprInt = 4;

    struct FltConstStatics
    {
        static constexpr double kScale = 4.0;
        static constexpr float kHalf = 0.5f;
        static constexpr int kCount = 3;
    };

    inline float fltconst_take_float(float value) noexcept { return value; }
}

namespace cppt
{
    // M69: one foreign identity spelling across extractor and backend paths. The two CRTP
    // specializations deliberately have different non-type values, and the byte view exercises
    // a multi-word primitive template argument through both inheritance and a free function.
    namespace unified
    {
        template <int N, class T>
        struct UnifiedNormBase
        {
            int n() const noexcept { return N; }
            T tag() const;
        };

        struct UnifiedPos : UnifiedNormBase<1, UnifiedPos>
        {
            int marker;
            UnifiedPos() noexcept : marker(11) {}
        };

        struct UnifiedNeg : UnifiedNormBase<-1, UnifiedNeg>
        {
            int marker;
            UnifiedNeg() noexcept : marker(22) {}
        };

        template <class T>
        struct UnifiedArrayRef
        {
            const T* data;
            std::size_t count;
            std::size_t size() const noexcept { return count; }
        };

        struct UnifiedBytes : UnifiedArrayRef<unsigned char>
        {
            int marker;
            UnifiedBytes(const unsigned char* p, std::size_t n) noexcept
                : marker(33)
            {
                data = p;
                count = n;
            }
        };

        inline int norm_pos(const UnifiedNormBase<1, UnifiedPos>& value) noexcept { return value.n(); }
        inline int norm_neg(const UnifiedNormBase<-1, UnifiedNeg>& value) noexcept { return value.n(); }
        inline int bytes_as_base(const UnifiedArrayRef<unsigned char>& value) noexcept
        { return (int)value.size(); }
    }

    inline Box<unsigned long> primid_make_unsigned_long(unsigned long value) noexcept
    { return Box<unsigned long>(value); }
    inline unsigned long primid_take_unsigned_long(const Box<unsigned long>& value) noexcept
    { return value.get(); }
    inline Box<size_t> primid_make_size_t(size_t value) noexcept
    { return Box<size_t>(value); }
    inline Box<unsigned> primid_make_unsigned(unsigned value) noexcept
    { return Box<unsigned>(value); }
    inline Box<long double> primid_make_long_double(long double value) noexcept
    { return Box<long double>(value); }
    inline std::vector<size_t> primid_size_t_vector_roundtrip(
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
    namespace tplbase
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
template <class T> struct GlobalBox { T value; };
using GlobalBoxAlias = GlobalBox<int>;
using GlobalBoxAlias2 = GlobalBox<int>;
typedef GlobalBox<double> GlobalBoxTypedef;
inline int globalbox_take_global(const GlobalBox<int>& v) noexcept { return v.value + 1; }

// An ALIAS TEMPLATE (`template<class T> using ... = ...;`), at global scope and inside a
// namespace. The DECLARATION spelling always resolved through the type request; the CONSTRUCTOR
// spelling (`GCellAlias<int>()`) reached function lookup instead, where an alias template has no
// entry, and was refused as an unknown function.
// NOT covered here: an alias template whose TARGET template is not reachable as a
// namespace-qualified name (the target in the alias's OWN namespace, or a global-scope target).
// Clang prints such a pattern unqualified, so the harvested target base carries no namespace and
// the DECLARATION spelling fails too - a different root cause from the one below.
namespace cpptacell
{
    template <class T> struct AliasCell { T value; AliasCell() noexcept : value(T(9)) {} };
}
template <class T> using GCellAlias = cpptacell::AliasCell<T>;
namespace cppta
{
    template <class T> using CellAlias = cpptacell::AliasCell<T>;
    namespace deep { template <class T> using DeepCellAlias = cpptacell::AliasCell<T>; }
}
inline int gcellalias_take(const GCellAlias<int>& c) noexcept { return c.value + 2; }
inline int cellalias_take(const cppta::CellAlias<int>& c) noexcept { return c.value + 3; }

// Section M84: a class used as a std::map key and as a by-reference parameter. A named lvalue
// index has to reach overload matching with its class identity intact.
struct SubscriptKey
{
    int v;
    SubscriptKey() noexcept : v(0) {}
    explicit SubscriptKey(int a) noexcept : v(a) {}
    bool operator<(const SubscriptKey& o) const noexcept { return v < o.v; }
};
inline int take_subscript_key(const SubscriptKey& k) noexcept { return k.v + 1; }
namespace cppt
{
    // M81: ref-qualified member overloads keep the receiver's C++ value category.
    class RefQualAssignBox
    {
    public:
        int value;
        int tag;

        RefQualAssignBox() noexcept : value(0), tag(0) {}
        explicit RefQualAssignBox(int v) noexcept : value(v), tag(0) {}
        RefQualAssignBox(const RefQualAssignBox&) = default;
        RefQualAssignBox(RefQualAssignBox&&) noexcept = default;
        ~RefQualAssignBox() noexcept {}

        RefQualAssignBox& operator=(const RefQualAssignBox& other) & noexcept
        { value = other.value; tag = 1; return *this; }
        RefQualAssignBox& operator=(const RefQualAssignBox& other) && noexcept
        { value = other.value + 100; tag = 2; return *this; }
        RefQualAssignBox& operator=(RefQualAssignBox&& other) & noexcept
        { value = other.value + 200; tag = 3; return *this; }
        RefQualAssignBox& operator=(RefQualAssignBox&& other) && noexcept
        { value = other.value + 300; tag = 4; return *this; }
    };

    struct RefQualFieldHolder
    {
        RefQualAssignBox box;
        RefQualFieldHolder() noexcept : box(0) {}
    };

    class RefQualMethods
    {
    public:
        // M81: ref-qualified overload fixture.
        int marker;

        RefQualMethods() noexcept : marker(0) {}
        explicit RefQualMethods(int v) noexcept : marker(v) {}
        explicit RefQualMethods(std::initializer_list<int> values) noexcept
            : marker(values.size() == 0 ? 0 : *values.begin()) {}
        int both() & noexcept { return 11; }
        int both() && noexcept { return 22; }
        int onlyR() && noexcept { return 33; }
        int onlyL() & noexcept { return 44; }
        int plain() const noexcept { return 55; }
        RefQualMethods& with(int v) & noexcept { marker += v; return *this; }
        RefQualMethods&& with(int v) && noexcept
        { marker += v + 100; return static_cast<RefQualMethods&&>(*this); }
        int operator[](int i) & noexcept { return 100 + i; }
        int operator[](int i) && noexcept { return 200 + i; }
    };

    struct RefQualSlotHolder
    {
        RefQualAssignBox slots[2];
        RefQualSlotHolder() noexcept : slots{RefQualAssignBox(0), RefQualAssignBox(0)} {}
        RefQualAssignBox& operator[](int i) & noexcept { return slots[i]; }
    };

    inline RefQualMethods refqual_make_methods(int value) noexcept
    { return RefQualMethods(value); }
}

namespace cppt
{
    // M95 - STATIC members of a class TEMPLATE, reached through the qualified
    // specialization spelling `cppt.TplStatics<int>.member`.
    template <class T>
    struct TplStatics
    {
        static int rank() noexcept { return 7; }
        static T twice(T v) noexcept { return v + v; }
        static T combine(T a, T b) noexcept { return a * 10 + b; }
        static constexpr int kTag = 41;
        static int counter;
        T x;
        TplStatics() noexcept : x(T()) {}
    };
    template <class T> int TplStatics<T>::counter = 0;

    class TplStaticPayload
    {
    public:
        int v;
        TplStaticPayload() noexcept : v(0) {}
        explicit TplStaticPayload(int value) noexcept : v(value) {}
    };

    // Specialization over a user C++ class, and over another specialization.
    template <class T>
    struct TplStaticWrap
    {
        static int rank() noexcept { return 3; }
        static int score(const T& t) noexcept { return t.v + 5; }
        static int tally;
    };
    template <class T> int TplStaticWrap<T>::tally = 0;

    template <class T>
    struct TplStaticNest
    {
        static int depth() noexcept { return 2; }
        static int fromInner(const T& t) noexcept { return t.x + 1; }
    };

    inline int tplstatic_take(int v) noexcept { return v + 1000; }
}

namespace cppt
{
    // Structural iterator-to-const-iterator coverage: the SAME class template with 'const'
    // added on a pointer template argument is layout-identical, so it converts implicitly.
    template <class P> struct ConstIt { P p; };

    inline ConstIt<int*> constit_make(int* p) noexcept { return ConstIt<int*>{ p }; }
    inline ConstIt<const int*> constit_make_const(const int* p) noexcept
    { return ConstIt<const int*>{ p }; }
    inline ConstIt<const double*> constit_make_cdouble(const double* p) noexcept
    { return ConstIt<const double*>{ p }; }

    // Overloaded on purpose: the arity split is what the single-candidate path never exercised.
    inline int constit_take(ConstIt<const int*> it) noexcept { return *it.p + 100; }
    inline int constit_take(ConstIt<const int*> it, int bump) noexcept { return *it.p + bump; }

    // Exact-typed sibling next to the const-added one: identity must still win.
    inline int constit_pick(ConstIt<int*> it) noexcept { return *it.p + 1; }
    inline int constit_pick(ConstIt<const int*> it) noexcept { return *it.p + 2; }

    // Value vs reference, both reached through the const-added conversion. BOTH declaration
    // orders are spelled: an lvalue argument must pick the reference either way, never the
    // last-declared one.
    inline int constit_tie(ConstIt<const int*> it) noexcept { return *it.p + 10; }
    inline int constit_tie(const ConstIt<const int*>& it) noexcept { return *it.p + 20; }
    inline int constit_tie2(const ConstIt<const int*>& it) noexcept { return *it.p + 20; }
    inline int constit_tie2(ConstIt<const int*> it) noexcept { return *it.p + 10; }

    // Negative direction: const REMOVED is not a conversion.
    inline int constit_strip(ConstIt<int*> it) noexcept { return *it.p + 5; }
    inline int constit_strip(ConstIt<int*> it, int bump) noexcept { return *it.p + bump; }

    // Negative direction: a partial specialization KEYED ON CONSTNESS gives the const-added
    // spelling a different layout, so the bitwise binding would read past the argument object.
    template <class P> struct SzIt { P p; };
    template <class P> struct SzIt<const P*>
    { const P* p; long long pad0; long long pad1; int tag; };
    inline SzIt<int*> szit_make(int* p) noexcept { return SzIt<int*>{ p }; }
    inline int szit_take(SzIt<const int*> it) noexcept { return it.tag; }
    inline int szit_take(SzIt<const int*> it, int bump) noexcept { return it.tag + bump; }

    // Negative direction: a DIFFERENT pointee type is not a conversion.
    inline int constit_other(ConstIt<const double*> it) noexcept { return (int)*it.p + 5; }
    inline int constit_other(ConstIt<const double*> it, int bump) noexcept
    { return (int)*it.p + bump; }
}

// ACCEPT SET for the volatile-twin overload election, NOT a reproduction of the std::atomic bug.
// libc++ declares every __atomic_base member twice - a `volatile` overload and a non-volatile one
// - and CFlat drops `volatile` exactly as it drops `const`, so both collapse onto ONE CFlat
// signature and the collapse must elect a twin that has a reachable symbol. Here BOTH twins have
// one (an in-scope header has every inline body emitted, measured: a bodiless volatile twin in a
// user header cannot be produced, so the libc++ state is not reproducible outside libc++), which
// is the case the election must leave exactly as it was.
namespace atmrep
{
    template <class T>
    struct RepBase
    {
        mutable T v_;

        // volatile twin FIRST, the order libc++ writes it in.
        T get() const volatile noexcept { return v_; }
        T get() const noexcept { return v_; }
        void put(T d) volatile noexcept { v_ = d; }
        void put(T d) noexcept { v_ = d; }
        // Reversed declaration order, so both orders are covered.
        T peek() const noexcept { return v_; }
        T peek() const volatile noexcept { return v_; }
        bool ready() const volatile noexcept { return true; }
        bool ready() const noexcept { return static_cast<RepBase const volatile*>(this)->ready(); }
    };

    // Second template level, as atomic<T> : __atomic_base<T, true> : __atomic_base<T, false>.
    template <class T>
    struct RepMid : RepBase<T>
    {
        T bump(T d) volatile noexcept { this->v_ = this->v_ + d; return this->v_; }
        T bump(T d) noexcept { this->v_ = this->v_ + d; return this->v_; }
    };

    template <class T>
    struct Rep : RepMid<T>
    {
        Rep() noexcept { this->v_ = T(); }
    };
}

// std::memory_order has no CFlat spelling for its enumerators yet, so an explicit order reaches
// a libc++ atomic member through a C++ helper (see internal/issue p3 on scoped-enum enumerators).
namespace atmrep
{
    inline std::memory_order order_relaxed() noexcept { return std::memory_order_relaxed; }
    inline std::memory_order order_seq_cst() noexcept { return std::memory_order_seq_cst; }
}

// C++ ALIAS TEMPLATE PATTERNS. An alias template names a specialization of its target with its
// own argument PATTERN, which may fix, reorder or partially bind the target's parameters.
namespace alnp
{
    template <class T, int N = 4> struct NBox { T v; NBox() noexcept : v(T(N)) {} };
    template <class A, class B> struct APair
    {
        A a; B b;
        APair() noexcept : a(A(1)), b(B(2)) {}
        int tag() const noexcept { return (int)sizeof(A) * 100 + (int)sizeof(B); }
    };
    template <class T> struct AWrap { T inner; int mark; AWrap() noexcept : mark(3) {} };
    template <class T, int N = 4, int M = 3> struct NBox3 { int v; NBox3() noexcept : v(N * 100 + M) {} };
    template <class T> struct ACell { T value; ACell() noexcept : value(T(5)) {} };
    template <class T> using ACellSelf = ACell<T>;          // target in the alias's own namespace
}
namespace alna
{
    template <class T> using NBdef = alnp::NBox<T, 7>;              // fixed non-type argument
    // The alias's OWN default (5) differs from the target's (4), so a use site that supplies
    // nothing must see 5 - the alias default, not the target's.
    template <class T, int N = 5> using NB = alnp::NBox<T, N>;
    template <class T, int N> using NBfree = alnp::NBox<T, N>;      // no default: target's applies
    template <class T, int N = 2> using Box3Def = alnp::NBox3<T, N, 9>;   // default + later fixed
    template <class T, int N = 2> using WrapNBdef = alnp::AWrap<alnp::NBox<T, N>>;  // nested, defaulted
    template <class T, int N> using Box3Late = alnp::NBox3<T, N, 9>;  // fixed arg after an unsupplied param
    template <class T> using OneParam = alnp::NBox<T, 7>;           // one parameter only
    template <class A, class B> using Flip = alnp::APair<B, A>;     // reordered pattern
    template <class T> using IntPair = alnp::APair<int, T>;         // partially fixed pattern
    template <class T> using NBdefOfAlias = NBdef<T>;               // pattern names another alias
    template <class T> using WrapNB = alnp::AWrap<alnp::NBox<T, 9>>; // nested specialization arg
}
template <class T> using GNBdef = alnp::NBox<T, 6>;                 // global-scope alias
template <class T> struct GAlnBox { T value; GAlnBox() noexcept : value(T(8)) {} };
template <class T> using GAlnBoxAlias = GAlnBox<T>;                 // global unqualified target
inline int alna_take_nbdef(const alna::NBdef<int>& b) noexcept { return b.v + 1; }

// A C++ signature that names a std container over a std container over a POINTER to a user class
// declared in this same header. A CFlat spelling of the same type must bind to THIS record.
namespace cppnp
{
    struct Leaf
    {
        int v;
        Leaf() noexcept : v(4) {}
        Leaf(int x) noexcept : v(x) {}
        int get() const noexcept { return v; }
    };
    inline int take_grid(const std::vector<std::vector<Leaf*>>& g) noexcept
    {
        int total = 0;
        for (const std::vector<Leaf*>& row : g)
            for (Leaf* leaf : row) total += leaf->v;
        return total;
    }
    inline std::vector<std::vector<Leaf*>> make_grid(Leaf* one) noexcept
    {
        std::vector<std::vector<Leaf*>> g;
        std::vector<Leaf*> row;
        row.push_back(one);
        g.push_back(row);
        return g;
    }
}
