// C++ interop fixture, bound with `import cpp` - the .h extension is deliberate: the keyword
// selects C++ mode, the extension never does. Declarations only; definitions live in the
// sibling cpp_interop_basic.cpp. Every entry point is noexcept except may_throw, which exists
// solely to arm Test/errors/err_cpp_may_throw.cb.
#pragma once

#include <functional>
#include <array>
#include <compare>
#include <cstddef>
#include <initializer_list>
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
    unsigned long unsigned_long_value() noexcept;
    long long_vector_sum(std::vector<long>& value) noexcept;
    unsigned long ulong_vector_sum(std::vector<unsigned long>& value) noexcept;
    long long long_long_vector_sum(std::vector<long long>& value) noexcept;
    unsigned long long ulong_long_vector_sum(std::vector<unsigned long long>& value) noexcept;
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
            int i64() const noexcept;
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

    struct AnonEnumHolder
    {
        enum { kAnon = 5, kOther = 9 };
        int v;
        AnonEnumHolder() noexcept : v(kAnon) {}
        int get() const noexcept { return v; }
    };

    struct NullDefaulted
    {
        long v;
        NullDefaulted(std::nullptr_t = nullptr) noexcept : v(7) {}
        long get() const noexcept { return v; }
    };

    struct IntPick
    {
        long which;
        explicit IntPick(int) noexcept : which(1) {}
        explicit IntPick(long) noexcept : which(2) {}
        explicit IntPick(unsigned) noexcept : which(3) {}
        explicit IntPick(long long) noexcept : which(4) {}
        explicit IntPick(double) noexcept : which(5) {}
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

    // M87: imported C++ bitfields follow the target ABI. Itanium keeps mixed base types in
    // one allocation unit while MSVC starts a new unit on a type change.
    struct BitsFit
    {
        int first : 20;
        unsigned int second : 10;
        unsigned int third : 1;
    };
    struct BitsOverflow
    {
        int first : 20;
        unsigned int second : 20;
    };
    struct BitsShortInt
    {
        short first : 7;
        int middle : 10;
        short last : 5;
    };
    struct BitsBoolInt
    {
        bool ready : 1;
        int value : 7;
        bool done : 1;
    };
    struct BitsSame
    {
        unsigned int first : 3;
        unsigned int second : 5;
        unsigned int third : 7;
    };
    inline int bits_size_fit() noexcept { return (int)sizeof(BitsFit); }
    inline int bits_size_overflow() noexcept { return (int)sizeof(BitsOverflow); }
    inline int bits_size_short_int() noexcept { return (int)sizeof(BitsShortInt); }
    inline int bits_size_bool_int() noexcept { return (int)sizeof(BitsBoolInt); }
    inline int bits_size_same() noexcept { return (int)sizeof(BitsSame); }
    inline int bits_fit_first(const BitsFit* v) noexcept { return v->first; }
    inline int bits_fit_second(const BitsFit* v) noexcept { return (int)v->second; }
    inline int bits_fit_third(const BitsFit* v) noexcept { return (int)v->third; }
    inline int bits_overflow_first(const BitsOverflow* v) noexcept { return v->first; }
    inline int bits_overflow_second(const BitsOverflow* v) noexcept { return (int)v->second; }
    inline int bits_short_int_first(const BitsShortInt* v) noexcept { return v->first; }
    inline int bits_short_int_middle(const BitsShortInt* v) noexcept { return v->middle; }
    inline int bits_short_int_last(const BitsShortInt* v) noexcept { return v->last; }
    inline int bits_bool_int_ready(const BitsBoolInt* v) noexcept { return v->ready ? 1 : 0; }
    inline int bits_bool_int_value(const BitsBoolInt* v) noexcept { return v->value; }
    inline int bits_bool_int_done(const BitsBoolInt* v) noexcept { return v->done ? 1 : 0; }
    inline int bits_same_first(const BitsSame* v) noexcept { return (int)v->first; }
    inline int bits_same_second(const BitsSame* v) noexcept { return (int)v->second; }
    inline int bits_same_third(const BitsSame* v) noexcept { return (int)v->third; }

    struct ZeroWidthSame
    {
        unsigned int first : 3;
        int : 0;
        unsigned int second : 3;
    };
    struct ZeroWidthDifferent
    {
        unsigned short first : 3;
        int : 0;
        unsigned int second : 3;
    };
    inline int zerowidth_same_size() noexcept { return (int)sizeof(ZeroWidthSame); }
    inline int zerowidth_different_size() noexcept { return (int)sizeof(ZeroWidthDifferent); }
    inline int zerowidth_same_first(const ZeroWidthSame* v) noexcept { return (int)v->first; }
    inline int zerowidth_same_second(const ZeroWidthSame* v) noexcept { return (int)v->second; }
    inline int zerowidth_different_first(const ZeroWidthDifferent* v) noexcept { return (int)v->first; }
    inline int zerowidth_different_second(const ZeroWidthDifferent* v) noexcept { return (int)v->second; }

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
    int default_extra() noexcept;
    int default_callback(int value) noexcept;
    int default_fn_arg(int value, int (*fn)(int) = default_callback,
                       int extra = default_extra()) noexcept;
    int default_array_arg(int bias, const int (&a)[3] = {1, 2, 3},
                          int extra = default_extra()) noexcept;

    // M27: a default-argument wrapper for a function that RETURNS A RECORD BY VALUE. The wrapper
    // reuses clang's arrangement for the full call, truncated to the parameters it keeps, so the
    // record result travels the same way it would in a direct call.
    struct DefaultPair { int a; int b; };
    DefaultPair default_pair(int a, int extra = default_extra()) noexcept;

    struct IndexLike
    {
        IndexLike(int value) noexcept : value(value) {}
        int value;
    };

    class DefaultArgs
    {
    public:
        int state;
        int scaled(int value, int extra = default_extra()) const noexcept;
        int bool_scaled(bool enabled = true) const noexcept;
        static int static_scaled(int value, int extra = default_extra()) noexcept;
        static int multi_defaults(const std::optional<int>& value = std::nullopt,
                                  bool create_graph = false, bool retain_graph = false,
                                  const std::optional<std::vector<int>>& values = std::nullopt) noexcept;
        int brace_sum(std::initializer_list<IndexLike> values) const noexcept;
    };

    // The LayoutPair specialization is deliberately not requested as a CFlat type. Its field
    // still needs Clang's size/alignment so the enclosing union can cross the boundary.
    template<class T> struct alignas(16) LayoutPair { T first; T second; };
    union LayoutUnion { int marker; LayoutPair<double> pair; };
    struct LayoutHolder { int sibling; LayoutUnion payload; };
    LayoutHolder make_layout_holder(int value) noexcept;

    // simdjson's logger pattern: a `static inline` declaration with a non-constant default,
    // defined later by a plain `inline` redeclaration. Internal linkage - never bound, and no
    // default-argument wrapper may reference it.
    static inline int internal_default(int v, int k = default_extra()) noexcept;
    inline int internal_default(int v, int k) noexcept { return v * 10 + k; }

    // ImGui's pattern: a DEFAULTED copy assignment over an array member. Defining it after
    // parsing makes clang look up __builtin_memcpy, which needs a translation-unit scope.
    struct Grid
    {
        int cells[4];
        Grid() noexcept;
        Grid& operator=(const Grid&) = default;
        int sum() const noexcept;
    };

    // simdjson's shape: a member returning a class defined LATER in the header by value
    // (`padded_string::operator padded_string_view()`). The member's ABI recipe needs the
    // later body, so a batch lays out every record before it registers any member.
    struct Later;
    struct Earlier
    {
        long a;
        Later to_later() const noexcept;
        operator Later() const noexcept;
    };
    struct Later
    {
        long x;
        long y;
        long z;
        long sum() const noexcept { return x + y + z; }
    };
    inline Later Earlier::to_later() const noexcept { return Later{a, a + 1, a + 2}; }
    inline Earlier::operator Later() const noexcept { return Later{a, a, a}; }

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

    // M26: the constructor takes 'const long long*'; a CFlat 'long*' argument is the same
    // pointer ABI under a different spelling, which is how c10::IntArrayRef is constructed.
    class PointerCount
    {
    public:
        PointerCount(const long long* values, unsigned long count) noexcept;
        ~PointerCount() noexcept;
        long long first() const noexcept;
    private:
        const long long* values_;
        unsigned long count_;
    };

    // ---- M25: a refused callback ABI plan is per callback type -----------------
    // A virtual base makes this class's layout unreproducible in CFlat, so a callback that
    // returns it by value has an arrangement cflat cannot express. That one plan must be
    // dropped from the callback ABI registry, and the rest of the header - including the
    // neighbour below - must still bind.
    struct AbiRefusedBase { int base; };
    struct AbiRefused : virtual AbiRefusedBase { int value; };
    typedef AbiRefused (*AbiRefusedCallback)(int);
    int abi_takes_refused_callback(AbiRefusedCallback cb) noexcept;
    int abi_neighbour(int value) noexcept;

    // M29. This function is DECLARED here and defined nowhere - no .cpp, no library. The inline
    // body below calls it, so the companion module clang emits for this header carries a
    // definition with an unresolvable reference. Nothing in CFlat calls that body, so it must be
    // dropped before the link; its inline neighbour, which CFlat does call, must still work.
    int missing_symbol_never_defined(int value) noexcept;
    inline int calls_missing_symbol(int value) noexcept
    { return missing_symbol_never_defined(value) + 1; }
    inline int missing_symbol_neighbour(int value) noexcept { return value * 3 + 1; }

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

    // M34: a non-explicit converting constructor makes scalar arguments legal for const-reference
    // parameters. The destructor keeps chained-return lifetime on the same nontrivial path.
    class ScalarBox
    {
    public:
        ScalarBox(double value) noexcept;
        ~ScalarBox() noexcept;
        double value() const noexcept;

    private:
        double value_;
    };

    double scalar_ref(const ScalarBox& value) noexcept;
    ScalarBox make_scalar_box(double value) noexcept;

    class ScalarOps
    {
    public:
        double add(const ScalarBox& value, double extra = 1.0) const noexcept;

    private:
        int marker_;
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
    const Tracked& tracked_ref() noexcept;
    // A MUTABLE class reference return. A CFlat pointer local binds to the REFERENT, so a write
    // through that pointer is visible to the next call - a copy would hide it.
    struct RefCell { int v; };
    inline RefCell& ref_cell() noexcept { static RefCell cell{770}; return cell; }
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

    // M30: non-member operators are found by argument-dependent lookup in the class namespace.
    struct FreeArithmetic { int value; };
    inline FreeArithmetic operator+(const FreeArithmetic& a,
                                    const FreeArithmetic& b) noexcept
    { return FreeArithmetic{a.value + b.value}; }
    inline bool operator==(const FreeArithmetic& a,
                           const FreeArithmetic& b) noexcept
    { return a.value == b.value; }

    namespace freeops
    {
        struct FreeArithmetic { int value; };
        inline FreeArithmetic operator+(const FreeArithmetic& a,
                                        const FreeArithmetic& b) noexcept
        { return FreeArithmetic{a.value + b.value}; }
        inline bool operator==(const FreeArithmetic& a,
                               const FreeArithmetic& b) noexcept
        { return a.value == b.value; }
    }

    // M40: the full C++ operator grid. Member forms live on Ops, free forms on
    // opsfree::FreeOps, so both lookup paths are covered by the same set of spellings.
    struct Ops
    {
        int value;
        int slots[2];

        Ops operator+(const Ops& o) const noexcept { return Ops{value + o.value, {0, 0}}; }
        Ops operator-(const Ops& o) const noexcept { return Ops{value - o.value, {0, 0}}; }
        Ops operator*(const Ops& o) const noexcept { return Ops{value * o.value, {0, 0}}; }
        Ops operator/(const Ops& o) const noexcept { return Ops{value / o.value, {0, 0}}; }
        Ops operator%(const Ops& o) const noexcept { return Ops{value % o.value, {0, 0}}; }
        Ops operator&(const Ops& o) const noexcept { return Ops{value & o.value, {0, 0}}; }
        Ops operator|(const Ops& o) const noexcept { return Ops{value | o.value, {0, 0}}; }
        Ops operator^(const Ops& o) const noexcept { return Ops{value ^ o.value, {0, 0}}; }
        Ops operator<<(const Ops& o) const noexcept { return Ops{value << o.value, {0, 0}}; }
        Ops operator>>(const Ops& o) const noexcept { return Ops{value >> o.value, {0, 0}}; }
        bool operator&&(const Ops& o) const noexcept { return value != 0 && o.value != 0; }
        bool operator||(const Ops& o) const noexcept { return value != 0 || o.value != 0; }

        Ops& operator+=(const Ops& o) noexcept { value += o.value; return *this; }
        Ops& operator-=(const Ops& o) noexcept { value -= o.value; return *this; }
        Ops& operator*=(const Ops& o) noexcept { value *= o.value; return *this; }
        Ops& operator/=(const Ops& o) noexcept { value /= o.value; return *this; }
        Ops& operator%=(const Ops& o) noexcept { value %= o.value; return *this; }
        Ops& operator&=(const Ops& o) noexcept { value &= o.value; return *this; }
        Ops& operator|=(const Ops& o) noexcept { value |= o.value; return *this; }
        Ops& operator^=(const Ops& o) noexcept { value ^= o.value; return *this; }
        Ops& operator<<=(const Ops& o) noexcept { value <<= o.value; return *this; }
        Ops& operator>>=(const Ops& o) noexcept { value >>= o.value; return *this; }

        // C++20: != < > <= >= are REWRITTEN from these two.
        bool operator==(const Ops& o) const noexcept { return value == o.value; }
        std::strong_ordering operator<=>(const Ops& o) const noexcept { return value <=> o.value; }
        bool operator==(int rhs) const noexcept { return value == rhs; }

        Ops operator-() const noexcept { return Ops{-value, {0, 0}}; }
        Ops operator+() const noexcept { return Ops{value, {0, 0}}; }
        bool operator!() const noexcept { return value == 0; }
        Ops operator~() const noexcept { return Ops{~value, {0, 0}}; }
        Ops& operator++() noexcept { value += 1; return *this; }
        Ops operator++(int) noexcept { Ops old{value, {0, 0}}; value += 1; return old; }
        Ops& operator--() noexcept { value -= 1; return *this; }
        Ops operator--(int) noexcept { Ops old{value, {0, 0}}; value -= 1; return old; }

        int& operator[](int index) noexcept { return slots[index]; }
        int operator[](int index) const noexcept { return slots[index] + 100; }

        int operator()() const noexcept { return value; }
        int operator()(int a) const noexcept { return value + a; }
        int operator()(int a, int b) const noexcept { return value + a * b; }

        explicit operator bool() const noexcept { return value != 0; }
        operator int() const noexcept { return value; }
        operator double() const noexcept { return (double)value + 0.5; }
    };

    // Reversed scalar comparison: only `int == Ops` exists as a free function.
    inline bool operator==(int lhs, const Ops& rhs) noexcept { return lhs == rhs.value + 1; }

    // Unary * and -> forwarding.
    struct OpsBox
    {
        Ops* target;
        Ops& operator*() const noexcept { return *target; }
        Ops* operator->() const noexcept { return target; }
    };

    // Reversed member ==: only Rev declares it, so `other == rev` needs the C++20 rewrite.
    struct RevOther { int value; };
    struct Rev
    {
        int value;
        bool operator==(const RevOther& o) const noexcept { return value == o.value; }
    };

    // A foreign left operand in another namespace, for the `OpsSink << FreeOps` case.
    struct OpsSink { int total; };

    namespace opsfree
    {
        struct FreeOps { int value; };

        inline FreeOps operator+(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value + b.value}; }
        inline FreeOps operator-(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value - b.value}; }
        inline FreeOps operator*(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value * b.value}; }
        inline FreeOps operator/(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value / b.value}; }
        inline FreeOps operator%(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value % b.value}; }
        inline FreeOps operator&(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value & b.value}; }
        inline FreeOps operator|(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value | b.value}; }
        inline FreeOps operator^(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value ^ b.value}; }
        inline FreeOps operator<<(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value << b.value}; }
        inline FreeOps operator>>(const FreeOps& a, const FreeOps& b) noexcept { return FreeOps{a.value >> b.value}; }
        inline bool operator&&(const FreeOps& a, const FreeOps& b) noexcept { return a.value != 0 && b.value != 0; }
        inline bool operator||(const FreeOps& a, const FreeOps& b) noexcept { return a.value != 0 || b.value != 0; }

        inline FreeOps& operator+=(FreeOps& a, const FreeOps& b) noexcept { a.value += b.value; return a; }
        inline FreeOps& operator-=(FreeOps& a, const FreeOps& b) noexcept { a.value -= b.value; return a; }
        inline FreeOps& operator*=(FreeOps& a, const FreeOps& b) noexcept { a.value *= b.value; return a; }
        inline FreeOps& operator/=(FreeOps& a, const FreeOps& b) noexcept { a.value /= b.value; return a; }
        inline FreeOps& operator%=(FreeOps& a, const FreeOps& b) noexcept { a.value %= b.value; return a; }
        inline FreeOps& operator&=(FreeOps& a, const FreeOps& b) noexcept { a.value &= b.value; return a; }
        inline FreeOps& operator|=(FreeOps& a, const FreeOps& b) noexcept { a.value |= b.value; return a; }
        inline FreeOps& operator^=(FreeOps& a, const FreeOps& b) noexcept { a.value ^= b.value; return a; }
        inline FreeOps& operator<<=(FreeOps& a, const FreeOps& b) noexcept { a.value <<= b.value; return a; }
        inline FreeOps& operator>>=(FreeOps& a, const FreeOps& b) noexcept { a.value >>= b.value; return a; }

        // All six spelled out: no rewriting needed on this class.
        inline bool operator==(const FreeOps& a, const FreeOps& b) noexcept { return a.value == b.value; }
        inline bool operator!=(const FreeOps& a, const FreeOps& b) noexcept { return a.value != b.value; }
        inline bool operator<(const FreeOps& a, const FreeOps& b) noexcept { return a.value < b.value; }
        inline bool operator>(const FreeOps& a, const FreeOps& b) noexcept { return a.value > b.value; }
        inline bool operator<=(const FreeOps& a, const FreeOps& b) noexcept { return a.value <= b.value; }
        inline bool operator>=(const FreeOps& a, const FreeOps& b) noexcept { return a.value >= b.value; }

        inline FreeOps operator-(const FreeOps& a) noexcept { return FreeOps{-a.value}; }
        inline FreeOps operator+(const FreeOps& a) noexcept { return FreeOps{a.value}; }
        inline bool operator!(const FreeOps& a) noexcept { return a.value == 0; }
        inline FreeOps operator~(const FreeOps& a) noexcept { return FreeOps{~a.value}; }
        inline FreeOps& operator++(FreeOps& a) noexcept { a.value += 1; return a; }
        inline FreeOps operator++(FreeOps& a, int) noexcept { FreeOps old{a.value}; a.value += 1; return old; }
        inline FreeOps& operator--(FreeOps& a) noexcept { a.value -= 1; return a; }
        inline FreeOps operator--(FreeOps& a, int) noexcept { FreeOps old{a.value}; a.value -= 1; return old; }

        // Left operand is a class from the ENCLOSING namespace: found through the right operand.
        inline cppi::OpsSink& operator<<(cppi::OpsSink& sink, const FreeOps& v) noexcept
        { sink.total += v.value; return sink; }
    }

    char16_t char16_value(char16_t value) noexcept;

    /*
     * A C++ class destroys its own subobjects: ~NestedOuter runs ~NestedInner exactly once.
     * Both destructors are bound on the CFlat side (the test constructs each class directly),
     * so a member-teardown pass synthesized around the bound destructor would destroy the
     * inner subobject a second time - a double release that only shows up as heap corruption
     * at some later allocation.
     */
    struct NestedInner
    {
        int tag;
        explicit NestedInner(int t) noexcept : tag(t) {}
        ~NestedInner();
        int get() const noexcept { return tag; }
    };

    struct NestedOuter
    {
        NestedInner inner;
        explicit NestedOuter(int t) noexcept : inner(t) {}
        ~NestedOuter();
        int get() const noexcept { return inner.get(); }
    };

    void reset_nested_counts() noexcept;
    int nested_inner_dtors() noexcept;
    int nested_outer_dtors() noexcept;

    // M40: a namespace-scope using-declaration re-exports a free function under a SECOND
    // qualified name. libtorch is built this way - `namespace torch { using at::manual_seed; }` -
    // and the alias name is the only one the documentation ever spells.
    namespace reexport_src
    {
        inline int seeded(int v) noexcept { return v + 40; }
        int linked(int v) noexcept;
    }

    namespace reexport
    {
        using reexport_src::seeded;   // inline, defined in this header
        using reexport_src::linked;   // out-of-line, defined in cpp_interop_basic.cpp
        using cppi::inner::twice;     // re-export across a sibling namespace
    }

    /*
     * Explicit instantiation DECLARATION of an all-inline polymorphic class template. The
     * specialization has no key function, but its vtable belongs to the translation unit that
     * carries the matching explicit instantiation DEFINITION (cpp_interop_basic.cpp), and Clang
     * gives such a vtable plain external linkage. Every lazily requested member of the class
     * produces its own companion module, so emitting the vtable here would put a strong
     * duplicate in two of them and the companion link would fail on it.
     */
    template <typename T>
    struct ExternPoly
    {
        T value;
        explicit ExternPoly(T v) noexcept : value(v) {}
        virtual ~ExternPoly() {}
        virtual T get() const noexcept { return value; }
        virtual T twice() const noexcept { return (T)(value + value); }
    };

    extern template struct ExternPoly<int>;

    /*
     * M85 - namespace-scope OBJECTS. The `extern` ones are defined out of line in
     * cpp_interop_basic.cpp and bind to Clang's mangled name; the header-only constants have no
     * library symbol at all, so cflat either folds them or emits their storage itself.
     */
    namespace nsobj
    {
        struct Color { int r; int g; int b; };
        struct Tracker
        {
            int seed;
            int doubled() const noexcept { return seed * 2; }
        };
        enum class Unit { Meter = 1, Mile = 7 };

        extern int counter;                 // mutable scalar, out-of-line
        extern const int kLinked;           // const scalar, out-of-line
        extern Tracker gTracker;            // class typed, out-of-line
        extern const Color kLinkedColor;    // const class typed, out-of-line

        namespace inner { extern int deep; }        // nested namespace
        inline namespace v1 { extern int versioned; }   // inline namespace

        inline constexpr int kAnswer = 42;      // folds, no storage
        constexpr Unit kUnit = Unit::Mile;      // scoped-enum constant
        static const int kOld = 7;              // internal linkage, folds
        constexpr Color kRed{1, 2, 3};          // internal linkage, needs storage
        inline const char* kName = "marker";       // header-only pointer object
    }

    // A 'consteval' (immediate) function has NO runtime symbol: clang evaluates it in the front
    // end and emits nothing, so binding it as an ordinary function failed at LINK time with a
    // mangled name and no source location. It is refused at the call site instead;
    // Test/errors/err_cpp_consteval_call.cb pins the message. The constexpr neighbour beside it
    // is the accept set - a constexpr function called at RUNTIME still binds normally.
    namespace immediate
    {
        constexpr int squared(int n) noexcept { return n * n; }
        consteval int must_fold(int n) noexcept { return n + 1; }
    }

    // M93 - class-scope `static const` with an IN-CLASS initializer and no out-of-line
    // definition. Such a member has no symbol to link, so it binds by its folded value.
    namespace statconst
    {
        enum Plain { PA = 7, PB = 8 };
        enum class Scoped { SA = 21, SB = 22 };

        struct InClass
        {
            static const int k = 41;
            static const unsigned int ku = 4000000001u;
            static const long long kll = 9000000000LL;
            static const bool kb = true;
            static const char kc = 'Q';
            static const Plain ke = PB;
            static const Scoped kse = Scoped::SB;
        };

        // An initializer that reads an earlier member of the same class.
        struct Chained { static const int base = 10; static const int derived = base + 5; };

        struct Outer { struct Inner { static const int k = 47; }; };

        // In-class initializer AND an out-of-line definition in cpp_interop_basic.cpp.
        struct Defined { static const int k = 46; };

        // Neighbours that already bound before the fold - they must keep working.
        struct Cexpr { static constexpr int k = 44; };
        struct InlineConst { static inline const int k = 45; };
        struct Linked { static const int k; };   // initializer is the out-of-line definition

        // A folded constant carries no storage, so it must not change the layout.
        struct WithField { static const int cap = 49; int v; };
    }

    // A C++ data member that is a POINTER or REFERENCE to another C++ class. Clang spells the
    // field type with no struct/union keyword, so the pointee has to be recovered from the C++
    // spelling or the field decays to an opaque void* and the member chain off it dies.
    namespace ptrmem
    {
        struct Leaf
        {
            int v;
            int get() const noexcept { return v; }
            void set(int n) noexcept { v = n; }
        };

        struct Poly
        {
            int b;
            virtual ~Poly() {}
            virtual int who() const noexcept { return 1; }
        };

        struct PolyDerived : Poly
        {
            int who() const noexcept override { return 2; }
        };

        struct Holder
        {
            Leaf* p;                 // plain pointer member
            const Leaf* cp;          // const-qualified pointee
            Leaf& r;                 // reference member (binds as a pointer field)
            Leaf byval;              // the anchor: a by-value member already chained
            Poly* poly;              // pointer to a POLYMORPHIC class: dispatch via the vtable
            Holder(Leaf* leaf, Poly* pv) noexcept
                : p(leaf), cp(leaf), r(*leaf), byval(*leaf), poly(pv) {}
        };

        // A pointer member whose pointee itself has a pointer member (chained hops).
        struct Outer
        {
            Holder* h;
            Outer(Holder* hh) noexcept : h(hh) {}
        };

        inline Holder& holder_ref() noexcept
        {
            static Leaf leaf{ 61 };
            static Poly poly;
            static Holder held(&leaf, &poly);
            return held;
        }

        inline Poly* make_derived() noexcept { static PolyDerived d; return &d; }
        inline int read_leaf(const Leaf* leaf) noexcept { return leaf->v; }
    }

    // A CFlat scalar bound to a `const T&` parameter of a FREE function. A literal, a folded
    // constant and an expression have no address, so the caller must convert to T and materialize
    // a temporary; passing the raw scalar made the callee read an integer as an address.
    namespace crefscalar
    {
        struct Folded { static constexpr int k = 41; };
        enum Plain { PlainSeven = 7 };
        enum class Scoped : int { Nine = 9 };
        struct Box { int v; };

        inline int take_cint(const int& v) { return v + 1; }
        inline int take_clonglong(const long long& v) { return (int)(v + 2); }
        inline int take_cdouble(const double& v) { return (int)(v * 2.0); }
        inline int take_cbool(const bool& v) { return v ? 5 : 6; }
        inline int take_cbox(const Box& b) { return b.v + 3; }
        // Non-const: writes through, so only an addressable lvalue may bind it.
        inline int take_ncint(int& v) { v = v + 1; return v; }
        // The address the callee actually received, so a leg can prove it is the caller's own
        // slot for an exact-width lvalue and a distinct temporary otherwise.
        inline unsigned long long addr_cint(const int& v) { return (unsigned long long)&v; }

        // Two `const T&` candidates of different referent widths. C++ picks `const int&` for an
        // `int` argument in BOTH declaration orders; rk3 is rk2 declared the other way round.
        inline int rk2(const int& v)       { return 3000 + v; }
        inline int rk2(const long long& v) { return 4000 + (int)v; }
        inline int rk3(const long long& v) { return 4000 + (int)v; }
        inline int rk3(const int& v)       { return 3000 + v; }
        inline int src_int() { return 41; }

        // Mixed shapes: a `const T&` materialization is never a perfect match, so an exact
        // by-value overload and a true `T&&` arm must both keep winning over it.
        inline int mixv(int v)        { return 1000 + v; }
        inline int mixv(const int& v) { return 2000 + v; }
        inline int mixr(const int& v) { return 100 + v; }
        inline int mixr(int&& v)      { return 200 + v; }

        // Floating referents, both declaration orders: a materialization never wins a tie.
        inline int dv(double v)        { return 1000 + (int)v; }
        inline int dv(const double& v) { return 2000 + (int)v; }
        inline int dw(const double& v) { return 2000 + (int)v; }
        inline int dw(double v)        { return 1000 + (int)v; }
        // A WIDER by-value candidate against an identity-exact const-ref one, both orders.
        inline int lv(long long v)  { return 1000 + (int)v; }
        inline int lv(const int& v) { return 2000 + v; }
        inline int lw(const int& v) { return 2000 + v; }
        inline int lw(long long v)  { return 1000 + (int)v; }
    }

    // A PRIVATE base that is a class-template SPECIALIZATION. The refusal at a pointer store has
    // to spell the destination the way source does: `cppi.BaseBox<int>`, never `cppi.BaseBox$int`.
    template <typename T>
    class BaseBox
    {
    public:
        T bv {};
        T bget() const noexcept { return bv; }
    };
    class PrivBoxDerived : private BaseBox<int> { public: int tag = 7; };
    class PubBoxDerived  : public  BaseBox<int> { public: int tag = 9; };
}

extern int cppi_nsobj_global;   // global-scope C++ object: no mangling at all

extern "C" int cppi_c_linkage(int v) noexcept;
