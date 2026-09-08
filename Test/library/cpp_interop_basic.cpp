// Definitions for cpp_interop_basic.h. Compiled by the host C++ driver and linked into
// the final image; the CFlat side never parses this file.
#include <cstdarg>
#include "cpp_interop_basic.h"

namespace cppi
{
    int add(int a, int b) noexcept { return a + b; }
    int add(double a, double b) noexcept { return (int)(a + b) + 1000; }

    int pick_ref(int& x) noexcept { int old = x; x = 7; return old; }
    int read_cref(const int& x) noexcept { return x + 1; }

    int* ptr_ident(int* p) noexcept { return p; }

    double ld_identity(long double value) noexcept { return (double)value; }
    long double ld_add(long double a, long double b) noexcept { return a + b; }
    void ld_store(long double value, double* out) noexcept { *out = (double)value; }

    static int g_slot = 99;
    int& ref_slot() noexcept { return g_slot; }

    long long widen(int v) noexcept { return (long long)v * 1000000000LL; }
    unsigned char uc(unsigned char v) noexcept { return (unsigned char)(v + 1); }

    int mode_value(Mode m) noexcept { return (int)m * 10; }

    wchar_t wchar_roundtrip(wchar_t v) noexcept { return v; }
    char16_t char16_roundtrip(char16_t v) noexcept { return v; }
    char32_t char32_roundtrip(char32_t v) noexcept { return v; }
    Small8 small8_roundtrip(Small8 v) noexcept { return v; }
    int small8_value(Small8 v) noexcept { return (int)v; }
    int dir_value(Dir d) noexcept { return (int)d; }

    int Outer::store = 0;
    int Outer::Inner::twice() const noexcept { return value * 2; }
    template struct Registry<int>;
    int registry_count(const Registry<int>& value) noexcept { return value.count; }
    int Variadic::sum(int count, ...) noexcept { return count; }
    int use_number(const Number* n) noexcept { return n->i; }
    int use_bits(const Bits* b) noexcept { return (int)b->a * 10 + (int)b->b; }
    int use_array(const ArrayHolder* a) noexcept
    {
        return a->arr[0] + a->arr[1] + a->arr[2] + a->arr[3];
    }
    int sum4(const int (&a)[4]) noexcept { return a[0] + a[1] + a[2] + a[3]; }
    int sum4_ptr(int (*a)[4]) noexcept { return (*a)[0] + (*a)[1] + (*a)[2] + (*a)[3]; }

    int scaled_default(int v, int k) noexcept { return v * k; }
    int default_double_ptr(double value, int* marker) noexcept
    {
        return (int)(value * 10.0) + (marker == nullptr ? 0 : *marker);
    }
    int helper() noexcept { return 9; }
    int with_call_default(int v, int k) noexcept { return v * k; }
    int PrivateDefaultArg::secret() noexcept { return 11; }
    int PrivateDefaultArg::call(int value) noexcept { return value; }
    __int128 wide_add(__int128 a, __int128 b) noexcept { return a + b; }
    int take_intvec(std::vector<int>& value) noexcept { return static_cast<int>(value.size()); }
    std::array<int, 4> make_int_array() noexcept { return std::array<int, 4>{1, 2, 3, 4}; }
    int array_total(const std::array<int, 4>& value) noexcept
    {
        return value[0] + value[1] + value[2] + value[3];
    }
    int read_outer_store() noexcept { return Outer::store; }
    std::map<int, int> make_int_map() noexcept
    {
        return std::map<int, int>{{3, 7}, {5, 11}};
    }
    int map_total(const std::map<int, int>& value) noexcept
    {
        return value.at(3) + value.at(5);
    }
    std::string_view take_string_view(std::string_view value) noexcept { return value; }
    std::string_view return_string_view(std::string_view value) noexcept { return value; }
    int sum_varargs(int count, ...) noexcept
    {
        va_list args;
        va_start(args, count);
        int total = 0;
        for (int i = 0; i < count; ++i) total += va_arg(args, int);
        va_end(args);
        return total;
    }

    Counter::Counter() noexcept : value_(0) {}
    Counter::Counter(int value) noexcept : value_(value) {}
    Counter Counter::operator+=(int value) noexcept { value_ += value; return *this; }
    bool Counter::operator<=(const Counter& other) const noexcept { return value_ <= other.value_; }
    bool Counter::operator>=(const Counter& other) const noexcept { return value_ >= other.value_; }
    int Counter::operator%(int divisor) const noexcept { return value_ % divisor; }
    int Counter::operator<<(int shift) const noexcept { return value_ << shift; }
    int Counter::operator&(int mask) const noexcept { return value_ & mask; }
    Counter Counter::operator-() const noexcept { return Counter(-value_); }
    int Counter::get() const noexcept { return value_; }
    int counter_add(int value, int delta) noexcept { return (Counter(value) += delta).get(); }
    int counter_le(int left, int right) noexcept { return Counter(left) <= Counter(right); }
    int counter_ge(int left, int right) noexcept { return Counter(left) >= Counter(right); }
    int counter_mod(int value, int divisor) noexcept { return Counter(value) % divisor; }
    int counter_shift(int value, int shift) noexcept { return Counter(value) << shift; }
    int counter_bitand(int value, int mask) noexcept { return Counter(value) & mask; }
    int counter_neg(int value) noexcept { return (-Counter(value)).get(); }

    Cursor::Cursor(int value) noexcept : pos(value) {}
    Cursor& Cursor::operator++() noexcept { ++pos; return *this; }
    Cursor Cursor::operator++(int) noexcept { Cursor old(*this); pos += 100; return old; }
    Cursor& Cursor::operator--() noexcept { --pos; return *this; }
    Cursor Cursor::operator--(int) noexcept { Cursor old(*this); pos -= 100; return old; }

    Truthy::Truthy(int value) noexcept : v(value) {}
    Truthy::operator bool() const noexcept { return v != 0; }

    Functor::Functor(int base) noexcept : base(base) {}
    int Functor::operator()(int a) const noexcept { return base + a; }
    int Functor::operator()(int a, int b) const noexcept { return base + a + b; }
    double Functor::operator()(double a) const noexcept { return (double)base + a + 0.5; }

    Mask::Mask(int bits) noexcept : bits(bits) {}
    int Mask::operator~() const noexcept { return ~bits; }
    int Mask::operator-() const noexcept { return -bits; }
    int Mask::operator+() const noexcept { return bits + 100; }

    Convertible::Convertible(int v) noexcept : v(v) {}
    Convertible::operator int() const noexcept { return v + 1; }
    Convertible::operator double() const noexcept { return (double)v + 0.25; }
    Convertible::operator unsigned char() const noexcept { return (unsigned char)(v + 2); }

    ConvRefused::ConvRefused(int v) noexcept : v(v) {}
    ConvRefused::operator int ConvHolder::*() const noexcept { return &ConvHolder::slot; }
    int ConvRefused::get() const noexcept { return v + 3; }

    ConvDeleted::ConvDeleted(int v) noexcept : v(v) {}
    int ConvDeleted::get() const noexcept { return v + 4; }

    DefaultCtorCounter::DefaultCtorCounter() noexcept : value_(37) {}
    int DefaultCtorCounter::get() const noexcept { return value_; }

    namespace inner
    {
        int twice(int v) noexcept { return v * 2; }
    }

    int may_throw(int v) { return v; }

    int read_pair(const Pair* p) noexcept { return p->a + p->b; }

    Small small_bump(Small v) noexcept
    {
        Small r; r.a = (signed char)(v.a + 1); r.b = (unsigned char)(v.b + 1); return r;
    }
    Mixed mixed_bump(Mixed v) noexcept
    {
        Mixed r; r.d = v.d + 1.0; r.i = v.i + 1; return r;
    }
    Large large_bump(Large v) noexcept
    {
        Large r; r.x = v.x + 1; r.y = v.y + 1; r.z = v.z + 1; return r;
    }
    Hfa hfa_bump(Hfa v) noexcept
    {
        Hfa r; r.a = v.a + 1.0f; r.b = v.b + 1.0f; r.c = v.c + 1.0f; r.d = v.d + 1.0f; return r;
    }
    Packed packed_bump(Packed v) noexcept
    {
        Packed r; r.c = (unsigned char)(v.c + 1); r.n = v.n + 1; return r;
    }
    Aligned aligned_bump(Aligned v) noexcept
    {
        Aligned r; r.x = v.x + 1; r.y = v.y + 1; return r;
    }

    long long mixed_args(Small s, int k, Large l) noexcept
    {
        return (long long)s.a * 1000000 + (long long)s.b * 10000 + (long long)k * 100
             + (l.x + l.y + l.z);
    }

    int pair_and_small(const Pair* p, Small s) noexcept
    {
        return p->a + p->b + s.a + s.b;
    }

    int apply_int(IntOp op, int a, int b) noexcept { return op(a, b); }
    Mixed apply_mixed(MixedOp op, Mixed v) noexcept { return op(v); }
    Large apply_large(LargeOp op, Large v) noexcept { return op(v); }
    Hfa apply_hfa(HfaOp op, Hfa v) noexcept { return op(v); }
    int visit_tracked(TrackedVisitor cb, void* ctx, int payload) noexcept
    {
        Tracked local(payload);
        return cb(local, ctx) + cb(local, ctx);
    }
    int apply_fn(const std::function<int(int)>& f, int v) noexcept { return f(v); }
    int apply_mixed_out(MixedOut cb, Mixed v, char** out) noexcept { return cb(v, out); }
    int apply_tracked_by_value(TrackedByValueCb cb) noexcept
    {
        Tracked local(1);
        return cb(local);
    }
    int apply_tracked_rvalue(TrackedRvalueCb cb) noexcept
    {
        Tracked local(1);
        return cb(static_cast<Tracked&&>(local));
    }

    unsigned long long size_of(int which) noexcept
    {
        switch (which)
        {
        case 0: return sizeof(Small);
        case 1: return sizeof(Mixed);
        case 2: return sizeof(Large);
        case 3: return sizeof(Hfa);
        case 4: return sizeof(Packed);
        case 5: return sizeof(Aligned);
        case 6: return sizeof(Pair);
        }
        return 0;
    }
    unsigned long long align_of(int which) noexcept
    {
        switch (which)
        {
        case 0: return alignof(Small);
        case 1: return alignof(Mixed);
        case 2: return alignof(Large);
        case 3: return alignof(Hfa);
        case 4: return alignof(Packed);
        case 5: return alignof(Aligned);
        case 6: return alignof(Pair);
        }
        return 0;
    }

    int Point::s_calls = 0;

    int  Point::sum() const noexcept          { return x + y; }
    void Point::bump(int d) noexcept           { x += d; y += d; ++s_calls; }
    int  Point::scaled(int f, int off) noexcept { return (x + y) * f + off; }
    int  Point::mode_scaled(Mode mode) noexcept { return (x + y) * (int)mode; }
    void Point::set_out(char*& out) noexcept    { out = "set-out"; }
    int  Point::probe() noexcept               { return 1; }
    int  Point::probe() const noexcept         { return 2; }
    int  Point::origin_sum() noexcept          { return 0; }
    int  Point::hidden() const noexcept        { return hidden_; }

    // ---- M4b: nontrivial class lifetime instrumentation -------------------------
    static int g_ctor = 0;
    static int g_copy = 0;
    static int g_move = 0;
    static int g_dtor = 0;
    static int g_copy_assign = 0;
    static int g_move_assign = 0;

    Tracked::Tracked(int payload) noexcept : payload(payload) { ++g_ctor; }
    Tracked::Tracked(const Tracked& other) noexcept : payload(other.payload) { ++g_copy; }
    Tracked::Tracked(Tracked&& other) noexcept : payload(other.payload) { other.payload = -1; ++g_move; }
    Tracked::~Tracked() noexcept { ++g_dtor; }
    Tracked& Tracked::operator=(const Tracked& other) noexcept
    {
        payload = other.payload; ++g_copy_assign; return *this;
    }
    Tracked& Tracked::operator=(Tracked&& other) noexcept
    {
        payload = other.payload; other.payload = -1; ++g_move_assign; return *this;
    }
    int Tracked::value() const noexcept { return payload; }
    bool Tracked::operator==(const Tracked& other) const noexcept { return payload == other.payload; }

    int Sink::put(const Tracked&) noexcept { return 1; }
    int Sink::put(Tracked&&) noexcept { return 2; }

    void reset_counts() noexcept
    {
        g_ctor = 0; g_copy = 0; g_move = 0; g_dtor = 0; g_copy_assign = 0; g_move_assign = 0;
    }
    int ctor_count() noexcept        { return g_ctor; }
    int copy_count() noexcept        { return g_copy; }
    int move_count() noexcept        { return g_move; }
    int dtor_count() noexcept        { return g_dtor; }
    int copy_assign_count() noexcept { return g_copy_assign; }
    int move_assign_count() noexcept { return g_move_assign; }

    NoCopy::NoCopy(int v) noexcept : v_(v) {}
    NoCopy::NoCopy(NoCopy&& other) noexcept : v_(other.v_) { other.v_ = -1; }
    NoCopy::~NoCopy() noexcept {}
    int NoCopy::value() const noexcept { return v_; }

    Tracked make_tracked(int payload) noexcept { return Tracked(payload); }
    int take_tracked(Tracked t) noexcept { return t.value(); }
    int take_moved(Tracked t) noexcept { return t.value() * 10; }
    int take_rvalue(Tracked&& t) noexcept
    {
        Tracked local = static_cast<Tracked&&>(t);
        return local.value();
    }
    int take_ref(const Tracked& t) noexcept { return t.value(); }
    int take_either(const Tracked& t) noexcept { return 1; }
    int take_either(Tracked&& t) noexcept
    {
        Tracked local = static_cast<Tracked&&>(t);
        return 2;
    }
    std::unique_ptr<Tracked> make_tracked_ptr(int payload) noexcept
    {
        return std::unique_ptr<Tracked>(new Tracked(payload));
    }
    int tracked_ptr_payload(const std::unique_ptr<Tracked>& p) noexcept
    {
        return p.get()->value();
    }
    int consume_tracked_ptr(std::unique_ptr<Tracked>&& p) noexcept
    {
        return p.get()->value();
    }
    std::shared_ptr<Tracked> make_tracked_shared(int payload) noexcept
    {
        return std::make_shared<Tracked>(payload);
    }
    int shared_payload(const std::shared_ptr<Tracked>& p) noexcept
    {
        return p.get()->value();
    }
    int consume_tracked_shared(std::shared_ptr<Tracked>&& p) noexcept
    {
        return p.get()->value();
    }
    std::pair<int, double> make_pair_value(int first, double second) noexcept
    {
        return { first, second };
    }
    std::optional<int> make_opt(int value, bool present) noexcept
    {
        return present ? std::optional<int>(value) : std::nullopt;
    }
}

extern "C" int cppi_c_linkage(int v) noexcept { return v + 5; }
