// C++ interop fixture, bound with `import cpp` - the .h extension is deliberate: the keyword
// selects C++ mode, the extension never does. Declarations only; definitions live in the
// sibling cpp_interop_basic.cpp. Every entry point is noexcept except may_throw, which exists
// solely to arm Test/errors/err_cpp_may_throw.cb.
#pragma once

#include <functional>
#include <array>
#include <memory>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>


namespace cppi
{
    // Overload pair. The double leg adds 1000 so the SELECTED overload is observable
    // from the return value alone.
    int add(int a, int b) noexcept;
    int add(double a, double b) noexcept;

    // Reference parameters: same machine representation as a pointer at the boundary.
    int pick_ref(int& x) noexcept;
    int read_cref(const int& x) noexcept;

    int* ptr_ident(int* p) noexcept;

    double ld_identity(long double value) noexcept;
    long double ld_add(long double a, long double b) noexcept;
    void ld_store(long double value, double* out) noexcept;

    // Reference RETURN: must arrive on the CFlat side as a plain pointer.
    int& ref_slot() noexcept;

    long long widen(int v) noexcept;
    unsigned char uc(unsigned char v) noexcept;

    enum class Mode : int { Off = 0, On = 1 };
    int mode_value(Mode m) noexcept;

    wchar_t wchar_roundtrip(wchar_t v) noexcept;
    char16_t char16_roundtrip(char16_t v) noexcept;
    char32_t char32_roundtrip(char32_t v) noexcept;

    enum class Small8 : unsigned char { Zero = 0, Value = 200 };
    struct Small8Holder { char c; Small8 e; };
    Small8 small8_roundtrip(Small8 v) noexcept;
    int small8_value(Small8 v) noexcept;

    enum Dir { Left = 1, Right = 2 };
    int dir_value(Dir d) noexcept;

    class Outer
    {
    public:
        typedef int Id;
        using Name = const char*;

        class Inner
        {
        public:
            int value;
            int twice() const noexcept;
        };
        enum class Kind : unsigned short { A = 3, B = 4 };
        static int store;
    };

    template<class T>
    struct Registry
    {
        static int count;
        static int bump() noexcept { return ++count; }
    };
    template<class T> int Registry<T>::count = 0;

    class Limits
    {
    public:
        static constexpr int kLimit = 32;
    };

    class Variadic
    {
    public:
        int sum(int count, ...) noexcept;
    };
    int registry_count(const Registry<int>& value) noexcept;

    union Number { int i; float f; };
    class AnonymousUnion
    {
    public:
        union { int i; float f; };
    };
    struct Bits { unsigned char a : 3; unsigned char b : 5; };
    struct ArrayHolder { int arr[4]; };
    int use_number(const Number* n) noexcept;
    int use_bits(const Bits* b) noexcept;
    int use_array(const ArrayHolder* a) noexcept;
    int sum4(const int (&a)[4]) noexcept;
    int sum4_ptr(int (*a)[4]) noexcept;

    template<class T, int N>
    struct Buf
    {
        T data[N];
        int cap() const noexcept { return N; }
    };

    int scaled_default(int v, int k = 3) noexcept;
    int default_double_ptr(double value = 2.5, int* marker = nullptr) noexcept;
    int helper() noexcept;
    int with_call_default(int v, int k = helper()) noexcept;

    class PrivateDefaultArg
    {
    private:
        static int secret() noexcept;
    public:
        static int call(int value = secret()) noexcept;
    };

    __int128 wide_add(__int128 a, __int128 b) noexcept;
    using IntVec = std::vector<int>;
    template<class T> using Vec = std::vector<T>;
    using IntArray4 = std::array<int, 4>;
    int take_intvec(std::vector<int>& value) noexcept;
    std::array<int, 4> make_int_array() noexcept;
    int array_total(const std::array<int, 4>& value) noexcept;
    int read_outer_store() noexcept;
    std::map<int, int> make_int_map() noexcept;
    int map_total(const std::map<int, int>& value) noexcept;
    std::string_view take_string_view(std::string_view value) noexcept;
    std::string_view return_string_view(std::string_view value) noexcept;
    int sum_varargs(int count, ...) noexcept;

    class Counter
    {
    public:
        Counter() noexcept;
        explicit Counter(int value) noexcept;
        Counter operator+=(int value) noexcept;
        bool operator<=(const Counter& other) const noexcept;
        bool operator>=(const Counter& other) const noexcept;
        int operator%(int divisor) const noexcept;
        int operator<<(int shift) const noexcept;
        int operator&(int mask) const noexcept;
        Counter operator-() const noexcept;
        int get() const noexcept;
    private:
        int value_;
    };
    int counter_add(int value, int delta) noexcept;
    int counter_le(int left, int right) noexcept;
    int counter_ge(int left, int right) noexcept;
    int counter_mod(int value, int divisor) noexcept;
    int counter_shift(int value, int shift) noexcept;
    int counter_bitand(int value, int mask) noexcept;
    int counter_neg(int value) noexcept;

    class Cursor
    {
    public:
        explicit Cursor(int value) noexcept;
        Cursor& operator++() noexcept;
        Cursor operator++(int) noexcept;
        Cursor& operator--() noexcept;
        Cursor operator--(int) noexcept;
        int pos;
    };

    class Truthy
    {
    public:
        Truthy(int value) noexcept;
        operator bool() const noexcept;
        int v;
    };

    // Round 10 - member operator(), unary ~/-, and conversion operators.
    class Functor
    {
    public:
        explicit Functor(int base) noexcept;
        int operator()(int a) const noexcept;            // arity 1
        int operator()(int a, int b) const noexcept;     // arity 2
        double operator()(double a) const noexcept;      // same arity, different type
        int base;
    };

    class Mask
    {
    public:
        explicit Mask(int bits) noexcept;
        int operator~() const noexcept;
        int operator-() const noexcept;
        int operator+() const noexcept;
        int bits;
    };

    class Convertible
    {
    public:
        explicit Convertible(int v) noexcept;
        operator int() const noexcept;                   // non-explicit
        explicit operator double() const noexcept;       // explicit
        operator unsigned char() const noexcept;
        int v;
    };

    // Its conversion target is a pointer to member, which cflat has no spelling for: the
    // member is recorded as refused and no cast can reach it. The class itself still binds.
    struct ConvHolder { int slot; };
    class ConvRefused
    {
    public:
        explicit ConvRefused(int v) noexcept;
        operator int ConvHolder::*() const noexcept;
        int get() const noexcept;
        int v;
    };

    // A conversion that exists but cannot be bound: the cast reports the recorded refusal.
    class ConvDeleted
    {
    public:
        explicit ConvDeleted(int v) noexcept;
        operator float() const noexcept = delete;
        int get() const noexcept;
        int v;
    };

    class DefaultCtorCounter
    {
    public:
        DefaultCtorCounter() noexcept;
        int get() const noexcept;
    private:
        int value_;
    };

    void unsupported_param(_Float16 value) noexcept;
    struct MemberPointerOwner { int value; };
    int member_pointer_param(int MemberPointerOwner::* value) noexcept;
    struct ReferenceField { int& value; };

    namespace inner
    {
        int twice(int v) noexcept;
    }

    // Deliberately NOT noexcept - calling it must be refused.
    int may_throw(int v);

    // A record reached by POINTER is always legal.
    struct Pair { int a; int b; };
    int read_pair(const Pair* p) noexcept;

    // ---- M3: trivially copyable records by value -------------------------------
    // Each shape lands on a DIFFERENT AArch64 arrangement, and every round trip adds 1 to
    // every field, so a wrong register assignment shows up as a wrong value, not just a
    // wrong type. sizeof/alignof are exported so the CFlat layout can be checked against
    // the C++ one rather than assumed.

    // 2 bytes: coerced to a single small integer.
    struct Small { signed char a; unsigned char b; };
    // 16 bytes, mixed int/FP: not an HFA, so two 8-byte chunks.
    struct Mixed { double d; int i; };
    // 24 bytes: too large for registers - indirect in, sret out.
    struct Large { long long x; long long y; long long z; };
    // Homogeneous float aggregate: 4 separate float registers under AAPCS64.
    struct Hfa { float a; float b; float c; float d; };
    // Packed: 5 bytes, alignment 1, `n` at byte offset 1.
    struct __attribute__((packed)) Packed { unsigned char c; int n; };
    // Over-aligned: sizeof is padded up to 32.
    struct alignas(32) Aligned { long long x; long long y; };

    Small  small_bump(Small v) noexcept;
    Mixed  mixed_bump(Mixed v) noexcept;
    Large  large_bump(Large v) noexcept;
    Hfa    hfa_bump(Hfa v) noexcept;
    Packed packed_bump(Packed v) noexcept;
    Aligned aligned_bump(Aligned v) noexcept;

    // Two aggregates with a scalar WEDGED BETWEEN them: proves the LLVM argument index
    // keeps up with slots that consume a different number of registers than they declare.
    long long mixed_args(Small s, int k, Large l) noexcept;

    // A by-value record alongside a by-pointer one in the same signature.
    int pair_and_small(const Pair* p, Small s) noexcept;

    unsigned long long size_of(int which) noexcept;
    unsigned long long align_of(int which) noexcept;

    // ---- M4: classes, members and access control -------------------------------
    // Trivially copyable class WITH members. Because copying it is raw bytes, it isolates the
    // member-call / static / access-control legs from construction and destruction.
    class Point
    {
    public:
        int x;
        int y;

        int sum() const noexcept;            // const instance method
        void bump(int d) noexcept;           // mutating instance method
        int scaled(int f, int off) noexcept;  // two arguments
        int mode_scaled(Mode mode = Mode::On) noexcept;
        void set_out(char*& out) noexcept;
        // const/non-const overload pair. CFlat drops const, so the RULING applies: an lvalue
        // picks the NON-const leg. The two legs return different values, so the selection is
        // observable from the result alone.
        int probe() noexcept;                // returns 1
        int probe() const noexcept;          // returns 2 - never selected for an lvalue

        static int origin_sum() noexcept;    // static method
        static int s_calls;                  // static data member, defined out of line

        int hidden() const noexcept;         // reads the private field below

    private:
        int hidden_;
    };

    // ---- M4b: nontrivial class lifetime instrumentation -------------------------
    // Every special member bumps an exported counter, so a CFlat test can assert the EXACT
    // number of constructions, copies, moves and destructions a construct performs. Bodies are
    // out of line so nothing needs C++ inline emission on the CFlat side.
    class Tracked
    {
    public:
        explicit Tracked(int payload) noexcept;
        Tracked(const Tracked& other) noexcept;
        Tracked(Tracked&& other) noexcept;
        ~Tracked() noexcept;
        Tracked& operator=(const Tracked& other) noexcept;
        Tracked& operator=(Tracked&& other) noexcept;

        int value() const noexcept;
        // Lets a class template over Tracked instantiate an equality member (M5b).
        bool operator==(const Tracked& other) const noexcept;

        int payload;

    private:
    };

    typedef int (*IntOp)(int, int);
    typedef Mixed (*MixedOp)(Mixed);
    typedef Large (*LargeOp)(Large);
    typedef Hfa (*HfaOp)(Hfa);
    typedef int (*TrackedVisitor)(const Tracked& t, void* ctx);
    typedef int (*TrackedByValueCb)(Tracked t);
    typedef int (*TrackedRvalueCb)(Tracked&& t);
    typedef int (*MixedOut)(Mixed, char**);

    int apply_int(IntOp op, int a, int b) noexcept;
    Mixed apply_mixed(MixedOp op, Mixed v) noexcept;
    Large apply_large(LargeOp op, Large v) noexcept;
    Hfa apply_hfa(HfaOp op, Hfa v) noexcept;
    int visit_tracked(TrackedVisitor cb, void* ctx, int payload) noexcept;
    int apply_fn(const std::function<int(int)>& f, int v) noexcept;
    int apply_mixed_out(MixedOut cb, Mixed v, char** out) noexcept;
    int apply_tracked_by_value(TrackedByValueCb cb) noexcept;
    int apply_tracked_rvalue(TrackedRvalueCb cb) noexcept;

    class Sink
    {
    public:
        int put(const Tracked& t) noexcept;
        int put(Tracked&& t) noexcept;

    private:
        int pad_;
    };

    void reset_counts() noexcept;
    int ctor_count() noexcept;
    int copy_count() noexcept;
    int move_count() noexcept;
    int dtor_count() noexcept;
    int copy_assign_count() noexcept;
    int move_assign_count() noexcept;

    // A move-only class: the copy constructor is deleted, so a copy-init must be diagnosed.
    class NoCopy
    {
    public:
        explicit NoCopy(int v) noexcept;
        NoCopy(const NoCopy&) = delete;
        NoCopy(NoCopy&& other) noexcept;
        ~NoCopy() noexcept;
        int value() const noexcept;

    private:
        int v_;
    };

    // Nontrivial by value: return by sret, argument by caller-owned pointer.
    Tracked make_tracked(int payload) noexcept;
    int take_tracked(Tracked t) noexcept;
    int take_moved(Tracked t) noexcept;
    int take_rvalue(Tracked&& t) noexcept;
    int take_ref(const Tracked& t) noexcept;
    int take_either(const Tracked& t) noexcept;
    int take_either(Tracked&& t) noexcept;
    std::unique_ptr<Tracked> make_tracked_ptr(int payload) noexcept;
    int tracked_ptr_payload(const std::unique_ptr<Tracked>& p) noexcept;
    int consume_tracked_ptr(std::unique_ptr<Tracked>&& p) noexcept;
    std::shared_ptr<Tracked> make_tracked_shared(int payload) noexcept;
    int shared_payload(const std::shared_ptr<Tracked>& p) noexcept;
    int consume_tracked_shared(std::shared_ptr<Tracked>&& p) noexcept;
    std::pair<int, double> make_pair_value(int first, double second) noexcept;
    std::optional<int> make_opt(int value, bool present) noexcept;
}

extern "C" int cppi_c_linkage(int v) noexcept;
