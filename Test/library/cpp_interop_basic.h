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
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp_interop_ternaryref.h"


namespace cppi
{
    inline std::mutex& q5_return_mutex() noexcept
    {
        static std::mutex value;
        return value;
    }
    inline bool& q5_return_after_unlock() noexcept
    {
        static bool value = false;
        return value;
    }
    inline bool& q5_return_seen_locked() noexcept
    {
        static bool value = false;
        return value;
    }
    inline void q5_return_reset() noexcept { q5_return_after_unlock() = false; q5_return_seen_locked() = false; }
    inline void q5_return_acquire() noexcept { q5_return_mutex().lock(); }
    inline void q5_return_release() noexcept { q5_return_mutex().unlock(); }
    // Sticky: one destruction while the lock is held fails the probe, whatever runs later.
    inline bool q5_return_ready() noexcept { return q5_return_after_unlock() && !q5_return_seen_locked(); }
    struct Q5ReturnProbe
    {
        ~Q5ReturnProbe() noexcept
        {
            if (q5_return_mutex().try_lock())
            {
                q5_return_after_unlock() = true;
                q5_return_mutex().unlock();
            }
            else q5_return_seen_locked() = true;
        }
    };

    struct IntrinsicMemberNames
    {
        bool is_string() const noexcept { return true; }
        int __popcount(int value) const noexcept { return value + 91; }
    };

    inline long cpp_std() noexcept { return __cplusplus; }

    struct NuaEmpty {};
    struct NuaOtherEmpty {};
    struct NuaMiddle
    {
        char prefix;
        [[no_unique_address]] NuaEmpty empty;
        int later;
    };
    struct NuaBitfield
    {
        [[no_unique_address]] NuaEmpty empty;
        unsigned bits : 3;
        int later;
    };
    struct NuaFirst
    {
        [[no_unique_address]] NuaEmpty empty;
        int later;
    };
    struct NuaLast
    {
        int later;
        [[no_unique_address]] NuaEmpty empty;
    };
    struct NuaSame
    {
        [[no_unique_address]] NuaEmpty first;
        [[no_unique_address]] NuaEmpty second;
        int later;
    };
    struct NuaDifferent
    {
        [[no_unique_address]] NuaEmpty first;
        [[no_unique_address]] NuaOtherEmpty second;
        int later;
    };
#pragma pack(push, 2)
    struct NuaPackedGuard
    {
        char lead;
        int later;
    };
#pragma pack(pop)
    struct NuaBase
    {
        [[no_unique_address]] NuaEmpty empty;
        int base_later;
    };
    struct NuaDerived : NuaBase
    {
        int later;
    };
    template<class T> struct NuaBox
    {
        [[no_unique_address]] T alloc;
        int later;
    };
    using NuaTemplate = NuaBox<NuaEmpty>;
    inline int nua_read_middle(const NuaMiddle* value) noexcept { return value->later; }
    inline int nua_read_same(const NuaSame* value) noexcept
    {
        return &value->first != &value->second ? value->later : -1;
    }
    inline int nua_read_different(const NuaDifferent* value) noexcept { return value->later; }
    inline int nua_read_derived(const NuaDerived* value) noexcept
    {
        return value->base_later + value->later;
    }
    inline int nua_read_template(const NuaTemplate* value) noexcept { return value->later; }
    inline long nua_size_middle() noexcept { return sizeof(NuaMiddle); }
    inline long nua_size_first() noexcept { return sizeof(NuaFirst); }
    inline long nua_size_last() noexcept { return sizeof(NuaLast); }
    inline long nua_size_same() noexcept { return sizeof(NuaSame); }
    inline long nua_size_different() noexcept { return sizeof(NuaDifferent); }
    inline long nua_size_derived() noexcept { return sizeof(NuaDerived); }
    inline long nua_size_template() noexcept { return sizeof(NuaTemplate); }
    inline NuaMiddle nua_return_middle(int later) noexcept
    {
        NuaMiddle value{};
        value.later = later;
        return value;
    }
    // [[no_unique_address]] next to bitfields and inside anonymous members.
    struct NuaBitUnits
    {
        [[no_unique_address]] NuaEmpty empty;
        unsigned a : 20;
        unsigned b : 20;
        int g;
        [[no_unique_address]] NuaOtherEmpty tail;
        unsigned c : 7;
    };
    struct NuaAnon
    {
        int x;
        struct { [[no_unique_address]] NuaEmpty e; int y; };
        union { [[no_unique_address]] NuaEmpty u; short s; };
        int z;
    };
    struct NuaAnonBits
    {
        int x;
        struct { [[no_unique_address]] NuaEmpty e; unsigned a : 5; };
        int z;
    };
    // The NUA member ends the bitfield run, so `b` starts at its Clang byte offset.
    struct NuaBitBetween
    {
        unsigned a : 3;
        [[no_unique_address]] NuaEmpty empty;
        unsigned b : 3;
        int y;
    };
    struct BytePackAfterChar
    {
        unsigned a : 3;
        unsigned b : 13;
        char c;
        int d : 9;
    };
    struct BytePackBeforeShort
    {
        int x;
        unsigned a : 3;
        unsigned b : 7;
        short s;
    };
    struct BytePackAfterCharOnly
    {
        char c;
        unsigned a : 2;
    };
    struct ByteRun24After { unsigned a : 24; char c; };
    struct ByteRun40After { long long a : 40; char c; };
    struct ByteRun12AfterChar { char c; unsigned a : 12; };
    struct ByteRun20AfterChar { char c; unsigned a : 20; };
    struct ByteRun40WithTail { long long a : 40; char c; int x; double d; };
    inline ByteRun24After byte_run24_make() noexcept
    { ByteRun24After v{}; v.a = 0x654321; v.c = 71; return v; }
    inline long long byte_run24_read(const ByteRun24After* v) noexcept
    { return (long long)v->a * 100 + v->c; }
    inline long long byte_run24_byval(ByteRun24After v) noexcept { return byte_run24_read(&v); }
    inline int byte_run24_size() noexcept { return (int)sizeof(ByteRun24After); }
    inline int byte_run24_align() noexcept { return (int)alignof(ByteRun24After); }
    inline int byte_run24_c_offset() noexcept { return (int)offsetof(ByteRun24After, c); }
    inline ByteRun40After byte_run40_make() noexcept
    { ByteRun40After v{}; v.a = 123456789; v.c = 71; return v; }
    inline long long byte_run40_read(const ByteRun40After* v) noexcept
    { return v->a * 100 + v->c; }
    inline long long byte_run40_byval(ByteRun40After v) noexcept { return byte_run40_read(&v); }
    inline int byte_run40_size() noexcept { return (int)sizeof(ByteRun40After); }
    inline int byte_run40_align() noexcept { return (int)alignof(ByteRun40After); }
    inline int byte_run40_c_offset() noexcept { return (int)offsetof(ByteRun40After, c); }
    inline ByteRun12AfterChar byte_run12_make() noexcept
    { ByteRun12AfterChar v{}; v.c = 71; v.a = 3000; return v; }
    inline long long byte_run12_read(const ByteRun12AfterChar* v) noexcept
    { return (long long)v->c * 10000 + v->a; }
    inline long long byte_run12_byval(ByteRun12AfterChar v) noexcept { return byte_run12_read(&v); }
    inline int byte_run12_size() noexcept { return (int)sizeof(ByteRun12AfterChar); }
    inline int byte_run12_align() noexcept { return (int)alignof(ByteRun12AfterChar); }
    inline ByteRun20AfterChar byte_run20_make() noexcept
    { ByteRun20AfterChar v{}; v.c = 71; v.a = 500000; return v; }
    inline long long byte_run20_read(const ByteRun20AfterChar* v) noexcept
    { return (long long)v->c * 10000000 + v->a; }
    inline long long byte_run20_byval(ByteRun20AfterChar v) noexcept { return byte_run20_read(&v); }
    inline int byte_run20_size() noexcept { return (int)sizeof(ByteRun20AfterChar); }
    inline int byte_run20_align() noexcept { return (int)alignof(ByteRun20AfterChar); }
    inline ByteRun40WithTail byte_run40_tail_make() noexcept
    { ByteRun40WithTail v{}; v.a = 123456789; v.c = 71; v.x = 55; v.d = 2.5; return v; }
    inline long long byte_run40_tail_read(const ByteRun40WithTail* v) noexcept
    { return v->a * 1000000 + (long long)v->c * 10000 + (long long)v->x * 100 + (long long)(v->d * 10); }
    inline long long byte_run40_tail_byval(ByteRun40WithTail v) noexcept { return byte_run40_tail_read(&v); }
    inline int byte_run40_tail_size() noexcept { return (int)sizeof(ByteRun40WithTail); }
    inline int byte_run40_tail_align() noexcept { return (int)alignof(ByteRun40WithTail); }
    inline int byte_run40_tail_c_offset() noexcept { return (int)offsetof(ByteRun40WithTail, c); }
    inline int byte_run40_tail_x_offset() noexcept { return (int)offsetof(ByteRun40WithTail, x); }
    inline int byte_run40_tail_d_offset() noexcept { return (int)offsetof(ByteRun40WithTail, d); }
    inline long nua_read_bitfield(const NuaBitfield* v) noexcept { return v->bits * 1000L + v->later; }
    inline long nua_bitfield_by_value(NuaBitfield v) noexcept { return v.bits * 1000L + v.later; }
    inline NuaBitfield nua_make_bitfield(unsigned bits, int later) noexcept
    {
        NuaBitfield v{};
        v.bits = bits;
        v.later = later;
        return v;
    }
    inline long nua_bit_units(NuaBitUnits v) noexcept
    {
        return (long)v.a * 10000 + v.b * 1000 + v.g * 10 + v.c;
    }
    inline NuaBitUnits nua_make_bit_units() noexcept
    {
        NuaBitUnits v{};
        v.a = 999999; v.b = 7; v.g = -2; v.c = 100;
        return v;
    }
    inline long nua_anon(NuaAnon v) noexcept { return v.x * 1000000L + v.y * 10000 + v.s * 100 + v.z; }
    inline long nua_anon_bits(NuaAnonBits v) noexcept { return v.x * 10000L + v.a * 100 + v.z; }
    inline long nua_size_bitfield() noexcept { return sizeof(NuaBitfield); }
    inline long nua_size_bit_units() noexcept { return sizeof(NuaBitUnits); }
    inline long nua_size_anon() noexcept { return sizeof(NuaAnon); }
    inline long nua_size_anon_bits() noexcept { return sizeof(NuaAnonBits); }
    inline NuaBitBetween nua_make_bit_between() noexcept
    {
        NuaBitBetween v{}; v.a = 5; v.b = 6; v.y = 321; return v;
    }
    inline int nua_bit_between_read(const NuaBitBetween* v) noexcept
    {
        return (int)v->a * 10000 + (int)v->b * 1000 + v->y;
    }
    inline int nua_bit_between_size() noexcept { return (int)sizeof(NuaBitBetween); }
    inline int nua_bit_between_y_offset() noexcept { return (int)offsetof(NuaBitBetween, y); }
    inline BytePackAfterChar byte_pack_after_char_make() noexcept
    {
        BytePackAfterChar v{}; v.a = 5; v.b = 6000; v.c = 71; v.d = 101; return v;
    }
    inline int byte_pack_after_char_read(const BytePackAfterChar* v) noexcept
    {
        return (int)v->a * 10000000 + (int)v->b * 1000 + v->c * 10 + v->d;
    }
    inline int byte_pack_after_char_size() noexcept { return (int)sizeof(BytePackAfterChar); }
    inline int byte_pack_after_char_align() noexcept { return (int)alignof(BytePackAfterChar); }
    inline int byte_pack_after_char_c_offset() noexcept { return (int)offsetof(BytePackAfterChar, c); }
    inline BytePackBeforeShort byte_pack_before_short_make() noexcept
    {
        BytePackBeforeShort v{}; v.x = 77; v.a = 5; v.b = 99; v.s = 1234; return v;
    }
    inline int byte_pack_before_short_read(const BytePackBeforeShort* v) noexcept
    {
        return v->x * 1000000 + (int)v->a * 100000 + (int)v->b * 1000 + v->s;
    }
    inline int byte_pack_before_short_size() noexcept { return (int)sizeof(BytePackBeforeShort); }
    inline int byte_pack_before_short_align() noexcept { return (int)alignof(BytePackBeforeShort); }
    inline int byte_pack_before_short_s_offset() noexcept { return (int)offsetof(BytePackBeforeShort, s); }
    inline BytePackAfterCharOnly byte_pack_after_char_only_make() noexcept
    {
        BytePackAfterCharOnly v{}; v.c = 71; v.a = 3; return v;
    }
    inline int byte_pack_after_char_only_read(const BytePackAfterCharOnly* v) noexcept
    {
        return v->c * 10 + (int)v->a;
    }
    inline int byte_pack_after_char_only_size() noexcept { return (int)sizeof(BytePackAfterCharOnly); }
    inline int byte_pack_after_char_only_align() noexcept { return (int)alignof(BytePackAfterCharOnly); }
    inline int byte_pack_after_char_only_c_offset() noexcept { return (int)offsetof(BytePackAfterCharOnly, c); }

    // Overload pair. The double leg adds 1000 so the SELECTED overload is observable
    // from the return value alone.
    int add(int a, int b) noexcept;
    int add(double a, double b) noexcept;

    struct ConstPointerRankValue {};
    struct ConstPointerRank
    {
        int pick(ConstPointerRankValue* value) const noexcept { return 1; }
        int pick(const ConstPointerRankValue* value) const noexcept { return 2; }
    };
    inline int const_pointer_rank(ConstPointerRankValue* value) noexcept { return 1; }
    inline int const_pointer_rank(const ConstPointerRankValue* value) noexcept { return 2; }
    // Dropping pointee const is not viable: const Derived* skips D* / P* for const P*.
    struct ConstPointerRankDerived : ConstPointerRankValue {};
    inline int const_derived_rank(ConstPointerRankDerived* value) noexcept { return 70; }
    inline int const_derived_rank(const ConstPointerRankValue* value) noexcept { return 71; }
    inline int const_base_rank(ConstPointerRankValue* value) noexcept { return 80; }
    inline int const_base_rank(const ConstPointerRankValue* value) noexcept { return 81; }
    // Pointer-to-pointer: the inner const must match exactly.
    inline int const_pp_rank(ConstPointerRankValue** value) noexcept { return 1; }
    inline int const_pp_rank(const ConstPointerRankValue** value) noexcept { return 2; }
    struct ConstPointerPointerRank
    {
        int pick(const ConstPointerRankValue** value) noexcept { return 40; }
        int pick(ConstPointerRankValue** value) noexcept { return 41; }
    };
    // Member and free operator siblings rank on pointee const like a call argument.
    struct ConstPointerRankShift
    {
        int k = 0;
        int operator<<(const ConstPointerRankValue* value) noexcept { return 2; }
        int operator<<(ConstPointerRankValue* value) noexcept { return 1; }
    };
    inline int operator+(const ConstPointerRankShift& s, ConstPointerRankValue* value) noexcept { return 5; }
    inline int operator+(const ConstPointerRankShift& s, const ConstPointerRankValue* value) noexcept { return 6; }
    // Lone mutable operator: a const pointer operand is refused.
    struct ConstPointerRankShiftMut
    {
        int k = 0;
        int operator<<(ConstPointerRankValue* value) noexcept { return 9; }
    };
    // Variadic sibling that drops const never displaces the const-correct overload.
    inline int const_variadic_rank(ConstPointerRankValue* value, ...) noexcept { return 1; }
    inline int const_variadic_rank(const ConstPointerRankValue* value, int a) noexcept { return 2; }
    // Function template beside a non-template: clang deduces T = const Rec for a const pointer.
    template<class T> int const_tpl_rank(T* value) noexcept { return 1; }
    inline int const_tpl_rank(ConstPointerRankValue* value) noexcept { return 2; }
    template<class T> int const_tpl_crank(const T* value) noexcept { return 3; }
    inline int const_tpl_crank(ConstPointerRankValue* value) noexcept { return 4; }
    // Constructor with only a mutable pointer parameter: a const pointer argument is refused.
    struct ConstPointerRankCtor { int k; ConstPointerRankCtor(ConstPointerRankValue* value) : k(3) {} };

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

    // Default constructors callable with no arguments only through a C++ default argument
    // cflat cannot pass: an unmappable parameter type, or a non-constant default expression.
    template <class A = std::allocator<char>>
    struct DefArgAlloc
    {
        long v;
        explicit DefArgAlloc(const A& a = A()) noexcept : v(31) {}
        long get() const noexcept { return v; }
    };
    using DefArgBuf = DefArgAlloc<>;
    inline int defarg_seven() noexcept { return 7; }
    struct DefArgNonConst
    {
        long v;
        DefArgNonConst(int x = defarg_seven()) noexcept : v(x + 10) {}
        long get() const noexcept { return v; }
    };
    struct DefArgOwned
    {
        std::unique_ptr<long> p;
        DefArgOwned(const std::allocator<char>& a = {}, const std::allocator<int>& b = {})
            : p(new long(33)) {}
        long get() const noexcept { return p ? *p : -1; }
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

    // A non-constant class default beside a `const char*` parameter: the wrapper ranks a string
    // literal argument exactly like its declaration does (it takes a PREFIX of its parameters).
    struct DefSize { float x, y; constexpr DefSize(float a, float b) : x(a), y(b) {} };
    inline int dflt_label(const char* label, const DefSize& size = DefSize(0, 0)) noexcept
    { return label[0] + (int)size.x; }
    struct DefLabeler
    {
        int k = 5;
        int label(const char* text, const DefSize& size = DefSize(0, 0)) const noexcept
        { return text[0] + (int)size.x + k; }
    };

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

    // simdjson's `parser.iterate(padded_string&)`: the argument reaches the parameter only through
    // `operator UdcView()`, and the callee returns a class that is NOT trivially copyable but has a
    // trivial destructor (no destructor symbol). The generated conversion wrapper must still bind.
    class UdcView;
    struct UdcStr { const char* p = "abc"; unsigned long n = 3; operator UdcView() const noexcept; };
    class UdcView : public std::string_view
    {
    public:
        UdcView() noexcept = default;
        UdcView(const char* s, unsigned long n) noexcept : std::string_view(s, n) {}
        UdcView(int k) noexcept : std::string_view("abcdefg", (unsigned long)k) {}
    };
    inline UdcStr::operator UdcView() const noexcept { return UdcView(p, n); }
    // User operator= only.
    struct UdcResA { int first = 0; UdcResA() = default; UdcResA(const UdcResA&) = default;
                     UdcResA& operator=(const UdcResA& o) { first = o.first; return *this; } };
    // Nontrivial std base: libc++ std::pair has a user-provided operator= (simdjson_result_base).
    struct UdcResB : protected std::pair<int, int>
    {
        UdcResB() : std::pair<int, int>(0, 0) {}
        int get() const { return first; }
        void set(int v) { first = v; }
    };
    // User copy constructor that marks a copy: an unelided copy reads first + 100.
    struct UdcResC { int first = 0; UdcResC() = default; UdcResC(const UdcResC& o) : first(o.first + 100) {} };
    // Polymorphic, implicit (trivial) destructor.
    struct UdcResV { int first = 0; virtual int kind() const { return 7; } };
    // Private destructor, befriended by the parser only: the conversion wrapper cannot return it.
    struct UdcParser;
    struct UdcResP { int first = 0; friend struct UdcParser; private: ~UdcResP() {} };
    struct UdcParser
    {
        UdcResA a(UdcView v) { UdcResA r; r.first = (int)v.size(); return r; }
        UdcResB b(UdcView v) { UdcResB r; r.set((int)v.size()); return r; }
        UdcResC c(UdcView v) { UdcResC r; r.first = (int)v.size(); return r; }
        UdcResV v(UdcView x) { UdcResV r; r.first = (int)x.size(); return r; }
        UdcResP p(UdcView x);
    };
    inline UdcResP UdcParser::p(UdcView x) { UdcResP r; r.first = (int)x.size(); return r; }
    inline UdcResA udc_fa(UdcView v) { UdcResA r; r.first = (int)v.size(); return r; }
    inline UdcResB udc_fb(UdcView v) { UdcResB r; r.set((int)v.size()); return r; }
    inline UdcResV udc_fv(UdcView v) { UdcResV r; r.first = (int)v.size(); return r; }

    // A failed default wrapper must still compete with a reference overload.
    struct DefaultRankOnly { int value = 7; };
    struct DefaultRankArg { int value = 9; DefaultRankArg() {} };
    inline int default_rank(DefaultRankOnly v, DefaultRankArg a = DefaultRankArg())
    { return v.value + a.value; }
    inline int default_rank(const DefaultRankOnly&) { return 20; }
    struct DefaultRankHost {
        int rank(DefaultRankOnly v, DefaultRankArg a = DefaultRankArg()) const
        { return v.value + a.value; }
        int rank(const DefaultRankOnly&) const { return 30; }
    };
    template<class T> struct DefaultRankTemplate {
        int rank(T v, DefaultRankArg a = DefaultRankArg()) const
        { return v.value + a.value; }
        int rank(const T&) const { return 20; }
    };
    using DefaultRankTemplateOnly = DefaultRankTemplate<DefaultRankOnly>;

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
    // Variadic beside a non-variadic sibling: with nothing in the ellipsis the variadic ranks
    // on its declared parameter (int exact beats int -> long), in either declaration order.
    int variadic_exact_rank(int x, ...) noexcept;
    int variadic_exact_rank(long x) noexcept;
    int variadic_exact_rank_rev(long x) noexcept;
    int variadic_exact_rank_rev(int x, ...) noexcept;
    // Ellipsis receiving arguments: clang ranks it pairwise against every sibling.
    int variadic_pair_rank(int x, ...) noexcept;
    int variadic_pair_rank(long x, ...) noexcept;
    int variadic_pair_rank_rev(long x, ...) noexcept;
    int variadic_pair_rank_rev(int x, ...) noexcept;
    int variadic_conv_rank(int x, ...) noexcept;
    int variadic_conv_rank(int x, double y) noexcept;
    int variadic_ptr_rank(ConstPointerRankValue* value, ...) noexcept;
    int variadic_ptr_rank(ConstPointerRankValue* value, int a) noexcept;
    // Ambiguous in clang: each is better at one argument (err_cpp_variadic_overload_ambiguous).
    int variadic_ambig(int x, ...) noexcept;
    int variadic_ambig(long x, int y) noexcept;
    int variadic_ambig_ptr(ConstPointerRankValue* value, ...) noexcept;
    int variadic_ambig_ptr(const ConstPointerRankValue* value, long a) noexcept;

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
    // A reference over a THREE-level pointer: the mapper folds ONE reference level into
    // IsCxxRefToPointer, never two, so this stays refused while `T **const &` binds.
    struct PtrDepthOwner { int value; };
    int ptr_depth3_ref_param(PtrDepthOwner ***const &value) noexcept;
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
        // `?:` arm shapes: a chained call and an operator returning the class by value.
        Tracked twice() const noexcept { return Tracked(payload * 2); }

        int payload;

    private:
    };

    inline int field_default_ctor_count = 0;
    inline int field_default_arg_count = 0;
    inline int field_default_dtor_count = 0;
    struct FieldDefaulted
    {
        int value;
        FieldDefaulted() : value(7) { ++field_default_ctor_count; }
        explicit FieldDefaulted(int v) : value(v) { ++field_default_arg_count; }
        ~FieldDefaulted() { ++field_default_dtor_count; }
    };
    inline void reset_field_default_counts()
    {
        field_default_ctor_count = field_default_arg_count = field_default_dtor_count = 0;
    }
    inline Tracked operator+(const Tracked& a, const Tracked& b) noexcept
    {
        return Tracked(a.payload + b.payload);
    }

    class TrackedBase
    {
    public:
        virtual ~TrackedBase() noexcept;
        virtual Tracked make(int payload) noexcept = 0;
    };

    class ConvertSource
    {
    public:
        explicit ConvertSource(int value) noexcept;
        ConvertSource(const ConvertSource& other) noexcept;
        ConvertSource(ConvertSource&& other) noexcept;
        ~ConvertSource() noexcept;
        int value() const noexcept;

    private:
        int value_;
    };

    class ConvertTarget
    {
    public:
        ConvertTarget(const ConvertSource& other) noexcept;
        ConvertTarget(const ConvertTarget& other) noexcept;
        ConvertTarget(ConvertTarget&& other) noexcept;
        ~ConvertTarget() noexcept;
        int value() const noexcept;

    private:
        int value_;
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
    // A DEDUCED callable parameter (the std::erase_if / std::jthread shape): F is whatever the
    // caller's callable is, so a CFlat callable must arrive as a type clang can deduce.
    template <class F> int apply_n(F&& f, int n) { int total = 0; for (int i = 0; i < n; ++i) total += f(i); return total; }
    template <class F> int apply_copy(F f, int v) { F second = f; return f(v) + second(v); }
    // F&& that std::move()s the callable into storage outliving the call (N50 escape shape).
    struct ClosureHolder { std::function<int(int)> f; int run(int x) { return f(x); } };
    template <class F> ClosureHolder move_hold(F&& f) { return {std::move(f)}; }
    // Calls F again after std::move()ing it into a std::function: a moved-from callable stays usable.
    template <class F> int reuse_moved(F&& f) { int a = f(1); { std::function<int(int)> g(std::move(f)); a += g(2); } return a + f(3); }
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
    // `auto x = <lvalue>` sources (7300-7329): a mutable reference return, and a holder whose
    // field is reached directly, through a pointer and through reference accessors.
    inline Tracked& tracked_mut_ref() noexcept { static Tracked value(813); return value; }
    // A vector of a nontrivial class returned by value (7363-7365).
    inline std::vector<Tracked> make_tracked_vec() noexcept
    {
        std::vector<Tracked> v;
        v.reserve(2);
        v.emplace_back(130);
        v.emplace_back(131);
        return v;
    }
    // Implicit copy constructors (7350-7353): a user destructor alone leaves the copy implicit
    // and trivial (no symbol to call); `= default` spells the same.
    inline int implicit_copy_dtors = 0;
    struct ImplicitCopy
    {
        int v;
        explicit ImplicitCopy(int x) noexcept : v(x) {}
        ~ImplicitCopy() { implicit_copy_dtors++; }
    };
    struct DefaultedCopy
    {
        int v;
        explicit DefaultedCopy(int x) noexcept : v(x) {}
        DefaultedCopy(const DefaultedCopy&) = default;
        ~DefaultedCopy() { implicit_copy_dtors++; }
    };
    struct TrackedHolder
    {
        Tracked t;
        explicit TrackedHolder(int v) noexcept : t(v) {}
        Tracked& get() noexcept { return t; }
        const Tracked& cget() const noexcept { return t; }
    };
    // A MUTABLE class reference return. A CFlat pointer local binds to the REFERENT, so a write
    // through that pointer is visible to the next call - a copy would hide it.
    struct RefCell { int v; };
    inline RefCell& ref_cell() noexcept { static RefCell cell{770}; return cell; }
    // RETURN-statement twins (3370-3389): a const reference to the same cell, a second cell for
    // '?:', and a member accessor whose referent lives inside its receiver.
    inline const RefCell& ref_cell_const() noexcept { return ref_cell(); }
    inline RefCell& ref_cell_other() noexcept { static RefCell cell{880}; return cell; }
    struct RefBox
    {
        RefCell c;
        RefCell& get() noexcept { return c; }
        const RefCell& cget() const noexcept { return c; }
    };
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

        struct MutFree { int value; };
        struct MutHost { MutFree field; };
        inline MutFree& operator|(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; return a; }
        inline MutFree operator^(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; return a; }
        inline void operator%(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; }
        inline MutFree& operator&(const FreeOps& a, MutFree& b) noexcept
        { b.value += a.value; return b; }
        inline MutFree make_mutfree() noexcept { return MutFree{}; }
        // A const-ref overload beside an unrelated mutable-ref one: an rvalue left operand binds.
        inline int operator+(const MutFree& a, int b) noexcept { return a.value + b; }
        inline MutFree& operator+(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; return a; }
        /*
         * One mutable-reference free operator per BINARY PAIR PARSER, each with its own
         * multiplier, so a chain leg can tell which operator ran and how often. `operator|`
         * above covers the bitwise pair; `<<`/`>>` cover the shift pair, `-`/`*`/`/` the
         * additive and multiplicative pairs, `<` the relational one and `&&` the logical one.
         */
        inline MutFree& operator<<(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; return a; }
        inline MutFree& operator>>(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 2; return a; }
        inline MutFree& operator-(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 3; return a; }
        inline MutFree& operator*(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 4; return a; }
        inline MutFree& operator/(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 5; return a; }
        inline MutFree& operator<<=(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 6; return a; }
        inline MutFree& operator&&(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value * 7; return a; }
        inline bool operator<(MutFree& a, const FreeOps& b) noexcept
        { a.value += b.value; return a.value < b.value; }
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
        static int linkedStatic;
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
        struct NoInit { static int k; };

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
        inline int t71_take_float_ref(const float& v) { return 710 + (int)(v * 10.0f); }
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

    class HoldsStdVector
    {
    public:
        std::vector<int> v;
        int tail;
        HoldsStdVector() : tail(5) { v.push_back(9); }
    };
    inline HoldsStdVector make_holds_std_vector() { return HoldsStdVector(); }
    inline HoldsStdVector holds_std_vector_global;

    class HoldsStdFunction
    {
    public:
        std::function<int()> f;
        std::function<int(int, int)> add;
        HoldsStdFunction()
            : f([] { return 23; }), add([](int a, int b) { return a + b; }) {}
    };
    inline int invoke(std::function<int()>& f) noexcept { return f(); }
    inline HoldsStdFunction make_holds_std_function() { return HoldsStdFunction(); }

    class LazyStdFunctionMember
    {
    public:
        int apply(std::function<int(int)> fn, int value) const noexcept
        {
            return fn(value);
        }
        int select(std::function<int(int)> fn) const noexcept { return fn(1) + 40; }
        int select(std::function<int(double)> fn) const noexcept { return fn(1.0) + 50; }
    };

    struct StatefulLess
    {
        bool operator()(int a, int b) const noexcept { return a < b; }
    };
    class HoldsCustomMap
    {
    public:
        std::map<int, int, StatefulLess> m;
        HoldsCustomMap() { m[4] = 97; }
    };
}

// A volatile member and a non-volatile twin with a default argument: CFlat has no volatile objects,
// so a one-argument call must take the non-volatile overload (MSVC <atomic> fetch_add shape).
namespace cppi
{
    struct VolatilePick
    {
        int pick(int) volatile { return 1; }
        int pick(int, int = 0) { return 2; }
    };
}

extern int cppi_nsobj_global;   // global-scope C++ object: no mangling at all

extern "C" int cppi_c_linkage(int v) noexcept;

namespace cppi
{
    inline long rrefLongValue = 101;
    inline int rrefIntValue = 23;
    inline size_t rrefSizeValue = 31;
    inline double rrefDoubleValue = 4.5;
    inline bool rrefBoolValue = false;
    inline char rrefCharValue = 'Q';
    inline int rrefPointerTarget = 29;
    inline int* rrefPointerValue = &rrefPointerTarget;
    enum class RrefEnum : int { Value = 17 };
    inline RrefEnum rrefEnumValue = RrefEnum::Value;

    inline long&& rref_long() { return std::move(rrefLongValue); }
    inline int&& rref_int() { return std::move(rrefIntValue); }
    inline size_t&& rref_size() { return std::move(rrefSizeValue); }
    inline double&& rref_double() { return std::move(rrefDoubleValue); }
    inline bool&& rref_bool() { return std::move(rrefBoolValue); }
    inline char&& rref_char() { return std::move(rrefCharValue); }
    inline int*&& rref_pointer() { return std::move(rrefPointerValue); }
    inline RrefEnum&& rref_enum() { return std::move(rrefEnumValue); }
    inline const long&& rref_const_long() { return std::move(rrefLongValue); }
    inline long& rref_lvalue_long() { return rrefLongValue; }
    inline long rref_by_value(long value) { return value + 3; }

    template <class U> inline U&& rref_move_impl(U& value) { return std::move(value); }
    template <class U> inline U&& rref_forward_impl(U&& value) { return std::forward<U>(value); }
    inline long&& rref_template_move() { return rref_move_impl(rrefLongValue); }
    inline long&& rref_template_forward()
    { return rref_forward_impl(std::move(rrefLongValue)); }
    inline std::optional<size_t> rref_optional()
    { return std::optional<size_t>(rrefSizeValue); }

    struct RrefScalar
    {
        long value = 41;
        long&& member_long() { return std::move(value); }
        long&& qualified_long() && { return std::move(value); }
        const long&& const_member_long() const { return std::move(value); }
        static long staticValue;
        static long&& static_long() { return std::move(staticValue); }
    };
    inline long RrefScalar::staticValue = 53;
    inline RrefScalar rref_make_scalar() { return RrefScalar(); }

    // std::move / std::forward shapes over an in-repo trait (no <utility>): a `T&&` CLASS result
    // is an xvalue at declaration init, assignment and return (keyed on the result type).
    template <class U> struct xv_remove_ref { using type = U; };
    template <class U> struct xv_remove_ref<U&> { using type = U; };
    template <class U> struct xv_remove_ref<U&&> { using type = U; };
    template <class U> inline typename xv_remove_ref<U>::type&& xv_move(U&& value) noexcept
    { return static_cast<typename xv_remove_ref<U>::type&&>(value); }
    template <class U> inline U&& xv_forward(typename xv_remove_ref<U>::type& value) noexcept
    { return static_cast<U&&>(value); }
    inline Tracked&& xv_pass(Tracked& t) noexcept { return static_cast<Tracked&&>(t); }
    struct XvHolder { Tracked item; XvHolder() noexcept : item(0) {} };
    // `const T&&` binds only the copy members; a declared-deleted move never falls back to copy.
    inline const Tracked&& xv_cmove(const Tracked& t) noexcept { return static_cast<const Tracked&&>(t); }
    inline int xv_copies = 0, xv_moves = 0;
    inline void xv_reset() noexcept { xv_copies = xv_moves = 0; }
    inline int xv_copy_count() noexcept { return xv_copies; }
    inline int xv_move_count() noexcept { return xv_moves; }
    struct XvNoMove
    {
        int v;
        explicit XvNoMove(int x) noexcept : v(x) {}
        XvNoMove(const XvNoMove& o) noexcept : v(o.v) { ++xv_copies; }
        XvNoMove(XvNoMove&&) = delete;
        XvNoMove& operator=(const XvNoMove& o) noexcept { v = o.v; ++xv_copies; return *this; }
        XvNoMove& operator=(XvNoMove&&) = delete;
        ~XvNoMove() noexcept {}
    };
    inline int xv_take_nomove(XvNoMove x) noexcept { return x.v; }
    struct XvMoveOnly
    {
        int v;
        explicit XvMoveOnly(int x) noexcept : v(x) {}
        XvMoveOnly(const XvMoveOnly&) = delete;
        XvMoveOnly(XvMoveOnly&& o) noexcept : v(o.v) { o.v = -1; }
        XvMoveOnly& operator=(const XvMoveOnly&) = delete;
        XvMoveOnly& operator=(XvMoveOnly&& o) noexcept { v = o.v; o.v = -1; return *this; }
        ~XvMoveOnly() noexcept {}
    };
    // Clang's overload resolution, not cflat's, picks the member: a viable `const T&&` beside a
    // deleted copy, a deleted `const T&&` beside a valid `T&&`, a defaulted move deleted by a
    // member (ignored, so the copy runs).
    struct XvConstMove
    {
        int v;
        explicit XvConstMove(int x) noexcept : v(x) {}
        XvConstMove(const XvConstMove&) = delete;
        XvConstMove(const XvConstMove&& o) noexcept : v(o.v) { ++xv_moves; }
        XvConstMove& operator=(const XvConstMove&) = delete;
        XvConstMove& operator=(const XvConstMove&& o) noexcept { v = o.v; ++xv_moves; return *this; }
        ~XvConstMove() noexcept {}
    };
    struct XvMultiMove
    {
        int v;
        explicit XvMultiMove(int x) noexcept : v(x) {}
        XvMultiMove(const XvMultiMove& o) noexcept : v(o.v) { ++xv_copies; }
        XvMultiMove(XvMultiMove&& o) noexcept : v(o.v) { o.v = -1; ++xv_moves; }
        XvMultiMove(const XvMultiMove&&) = delete;
        XvMultiMove& operator=(const XvMultiMove& o) noexcept { v = o.v; ++xv_copies; return *this; }
        XvMultiMove& operator=(XvMultiMove&& o) noexcept { v = o.v; o.v = -1; ++xv_moves; return *this; }
        XvMultiMove& operator=(const XvMultiMove&&) = delete;
        ~XvMultiMove() noexcept {}
    };
    // Trivially copyable, yet clang selects the constructor template for a `T&&` source.
    struct XvTrivialTemplate
    {
        int v;
        explicit XvTrivialTemplate(int x) noexcept : v(x) {}
        XvTrivialTemplate(const XvTrivialTemplate&) = default;
        template <class U> XvTrivialTemplate(U&& x) noexcept : v(x.v + 100) { x.v = -1; ++xv_moves; }
    };
    inline int xv_take_trivial(XvTrivialTemplate x) noexcept { return x.v; }
    // Trivially copyable with a deleted move: `std::move(x)` selects the deleted constructor.
    struct XvTrivialNoMove
    {
        int v;
        explicit XvTrivialNoMove(int x) noexcept : v(x) {}
        XvTrivialNoMove(const XvTrivialNoMove&) = default;
        XvTrivialNoMove(XvTrivialNoMove&&) = delete;
    };
    inline int xv_take_trivial_nomove(XvTrivialNoMove x) noexcept { return x.v; }
    // Trivially copyable, yet clang selects the assignment template for a `T&&` source.
    struct XvTrivialAssign
    {
        int v;
        explicit XvTrivialAssign(int x) noexcept : v(x) {}
        XvTrivialAssign(const XvTrivialAssign&) = default;
        XvTrivialAssign& operator=(const XvTrivialAssign&) = default;
        template <class U> XvTrivialAssign& operator=(U&& x) noexcept
        { v = x.v + 100; ++xv_moves; return *this; }
    };
    // Copy-initialization from an xvalue skips an `explicit` move constructor: the copy runs.
    struct XvExplicitMove
    {
        int v;
        explicit XvExplicitMove(int x) noexcept : v(x) {}
        XvExplicitMove(const XvExplicitMove& o) noexcept : v(o.v + 10) { ++xv_copies; }
        explicit XvExplicitMove(XvExplicitMove&& o) noexcept : v(o.v + 100) { ++xv_moves; }
        ~XvExplicitMove() noexcept {}
    };
    inline int xv_take_explicit(XvExplicitMove x) noexcept { return x.v; }
    // A non-const assignment template beats a const-qualified `operator=(T&&) const`.
    struct XvConstAssign
    {
        mutable int v;
        explicit XvConstAssign(int x) noexcept : v(x) {}
        XvConstAssign(const XvConstAssign& o) noexcept : v(o.v) {}
        XvConstAssign(XvConstAssign&& o) noexcept : v(o.v) {}
        const XvConstAssign& operator=(XvConstAssign&& o) const noexcept
        { v = o.v + 10; ++xv_copies; return *this; }
        template <class U> XvConstAssign& operator=(U&& o) noexcept
        { v = o.v + 100; ++xv_moves; return *this; }
        ~XvConstAssign() noexcept {}
    };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdefaulted-function-deleted"
    struct XvDefaultedMove
    {
        XvNoMove item;
        explicit XvDefaultedMove(int x) noexcept : item(x) {}
        XvDefaultedMove(const XvDefaultedMove&) = default;
        XvDefaultedMove(XvDefaultedMove&&) = default;
        XvDefaultedMove& operator=(const XvDefaultedMove&) = default;
        XvDefaultedMove& operator=(XvDefaultedMove&&) = default;
    };
#pragma clang diagnostic pop
}

namespace cppi_inh
{
    struct StaticGrand
    {
        static int K() { return 71; }
    };
    struct StaticBase : StaticGrand {};
    struct StaticDerived : StaticBase {};
    struct StaticHidden : StaticBase
    {
        static int K() { return 83; }
    };
    struct StaticPrivateBase
    {
    private:
        static int K() { return 97; }
    };
    struct StaticPrivateDerived : StaticPrivateBase {};
    struct StaticProtectedBase
    {
    protected:
        static int K() { return 101; }
    };
    struct StaticProtectedDerived : StaticProtectedBase {};

    template <class D> struct StaticCrtpBase
    {
        static int Identity() { return 109; }
    };
    template <class T, int N> struct StaticMat : StaticCrtpBase<StaticMat<T, N>> {};
    using StaticMat3 = StaticMat<double, 3>;

    // Eigen shape: the static lives two CRTP levels up and returns a specialization nothing
    // requested at import, so the base refuses it until the derived-name use retries it.
    template <class D> struct StaticExprResult
    {
        int value = 0;
        int get() const { return value; }
    };
    template <class D> struct StaticExprMatBase
    {
        static StaticExprResult<D> Identity() { StaticExprResult<D> r; r.value = 127; return r; }
    };
    template <class D> struct StaticExprPlain : StaticExprMatBase<D> {};
    struct StaticExprLeaf : StaticExprPlain<StaticExprLeaf> { int x = 0; };

    template <class D> struct OperatorBase
    {
        D operator+(const D& rhs) const
        {
            D result;
            result.value = static_cast<const D*>(this)->value + rhs.value;
            return result;
        }
        bool operator==(const D& rhs) const
        { return static_cast<const D*>(this)->value == rhs.value; }
        int operator[](int index) const
        { return static_cast<const D*>(this)->value + index; }
        D& operator+=(int amount)
        {
            static_cast<D*>(this)->value += amount;
            return *static_cast<D*>(this);
        }
    };
    struct OperatorValue : OperatorBase<OperatorValue>
    {
        int value = 0;
        OperatorValue() = default;
        OperatorValue(int v) : value(v) {}
    };
    struct OperatorPadding { long pad = 0; };
    struct OperatorNonFirst : OperatorPadding, OperatorBase<OperatorNonFirst>
    {
        int value = 0;
        OperatorNonFirst() = default;
        OperatorNonFirst(int v) : value(v) {}
    };
    struct OperatorHidden : OperatorBase<OperatorHidden>
    {
        int value = 0;
        OperatorHidden() = default;
        OperatorHidden(int v) : value(v) {}
        OperatorHidden operator+(const OperatorHidden& rhs) const
        { return OperatorHidden(value + rhs.value + 1000); }
    };
    struct OperatorHidingBase
    {
        int value = 0;
        OperatorHidingBase operator+(const OperatorHidingBase& rhs) const
        { return OperatorHidingBase{value + rhs.value}; }
    };
    struct OperatorHiddenDifferent : OperatorHidingBase
    {
        OperatorHiddenDifferent() = default;
        OperatorHiddenDifferent(int v) { value = v; }
        OperatorHiddenDifferent operator+(int rhs) const
        {
            OperatorHiddenDifferent result;
            result.value = value + rhs + 1000;
            return result;
        }
    };
    struct OperatorNameHidingBase
    {
        int value = 0;
        OperatorNameHidingBase operator+(const OperatorNameHidingBase& rhs) const
        { return OperatorNameHidingBase{value + rhs.value}; }
    };
    struct OperatorNameHiding : OperatorNameHidingBase
    {
        OperatorNameHiding operator+(int rhs) const
        {
            OperatorNameHiding result;
            result.value = value + rhs;
            return result;
        }
    };
    struct OperatorNameUsing : OperatorNameHidingBase
    {
        using OperatorNameHidingBase::operator+;
        OperatorNameUsing operator+(int rhs) const
        {
            OperatorNameUsing result;
            result.value = value + rhs + 1000;
            return result;
        }
    };
    template <class D> struct OtherOperatorBase
    {
        D operator+(const D& rhs) const
        {
            D result;
            result.value = static_cast<const D*>(this)->value + rhs.value + 2000;
            return result;
        }
    };
    struct OperatorAmbiguous : OperatorBase<OperatorAmbiguous>, OtherOperatorBase<OperatorAmbiguous>
    {
        int value = 0;
        OperatorAmbiguous() = default;
        OperatorAmbiguous(int v) : value(v) {}
    };

    // Inherited static DATA members with no library symbol: only the out-of-line DEFINITION is
    // `inline`, or the storage is an implicit template instantiation. Nothing in the companion
    // .cpp odr-uses them, so the request companion must emit them or the link fails.
    struct DataGrand { static int K; };
    inline int DataGrand::K = 17;
    struct DataBase : DataGrand {};
    struct DataDerived : DataBase {};
    struct DataUsing : DataGrand { using DataGrand::K; };
    struct DataConstGrand { static const int K; };
    inline const int DataConstGrand::K = 21;
    struct DataConstDerived : DataConstGrand {};
    template <class T> struct DataTplBase { static int K; };
    template <class T> int DataTplBase<T>::K = 9;
    struct DataTplDerived : DataTplBase<int> {};
    template <class T> struct DataTplInline { inline static int K = 10; };
    struct DataTplInlineDerived : DataTplInline<int> {};
    // Same shape at namespace scope: only the redeclaring definition is `inline`.
    extern int DataNsRedecl;
    inline int DataNsRedecl = 23;
    // A public member of a private/protected base exists but is inaccessible.
    struct DataViaPrivateBase : private DataGrand {};
    struct StaticViaPrivateBase : private StaticGrand {};
    struct StaticViaProtectedBase : protected StaticGrand {};

    // Operator hiding past one level, `using` in a middle class, unary vs binary hiding, and an
    // operator behind a private base.
    struct OpBase
    {
        int value = 0;
        OpBase() = default;
        OpBase(int v) : value(v) {}
        OpBase operator+(const OpBase& rhs) const { return OpBase(value + rhs.value); }
        OpBase operator-() const { return OpBase(-value - 1000); }
        OpBase operator-(const OpBase& rhs) const { return OpBase(value - rhs.value + 2000); }
    };
    struct OpMid : OpBase
    {
        OpMid() = default;
        OpMid(int v) : OpBase(v) {}
        OpMid operator+(int rhs) const { return OpMid(value + rhs + 100); }
    };
    struct OpLeaf : OpMid { OpLeaf() = default; OpLeaf(int v) : OpMid(v) {} };
    struct OpMidUsing : OpBase
    {
        OpMidUsing() = default;
        OpMidUsing(int v) : OpBase(v) {}
        using OpBase::operator+;
        OpMidUsing operator+(int rhs) const { return OpMidUsing(value + rhs + 300); }
    };
    struct OpLeafUsing : OpMidUsing { OpLeafUsing() = default; OpLeafUsing(int v) : OpMidUsing(v) {} };
    struct OpHideBinary : OpBase
    {
        OpHideBinary() = default;
        OpHideBinary(int v) : OpBase(v) {}
        OpHideBinary operator-(int rhs) const { return OpHideBinary(value - rhs + 500); }
    };
    struct OpHideUnary : OpBase
    {
        OpHideUnary() = default;
        OpHideUnary(int v) : OpBase(v) {}
        OpHideUnary operator-() const { return OpHideUnary(-value - 700); }
    };
    struct OpPlain : OpBase { OpPlain() = default; OpPlain(int v) : OpBase(v) {} };
    struct OpLeafPlain : OpPlain { OpLeafPlain() = default; OpLeafPlain(int v) : OpPlain(v) {} };
    struct OpPrivate : private OpBase { OpPrivate() = default; OpPrivate(int v) : OpBase(v) {} };
}

// Eigen shape: a CRTP base whose operator+ and static return expression templates (with a user
// copy constructor, so they never cross by value), and nontrivially-copyable targets that take
// them through a `const Base<D>&` constructor TEMPLATE - converting, or explicit-only.
namespace cppi_expr
{
    template <class L, class R> struct SumExpr;
    template <class D> struct FillExpr;
    template <class D> struct VecBase
    {
        const D& derived() const { return *static_cast<const D*>(this); }
        SumExpr<D, D> operator+(const D& rhs) const { return SumExpr<D, D>(derived(), rhs); }
        static FillExpr<D> Ones() { return FillExpr<D>(1.0); }
    };
    template <class L, class R> struct SumExpr : VecBase<SumExpr<L, R>>
    {
        const L& lhs;
        const R& rhs;
        SumExpr(const L& l, const R& r) : lhs(l), rhs(r) {}
        SumExpr(const SumExpr& other) : lhs(other.lhs), rhs(other.rhs) {}
        double coeff(int i) const { return lhs.coeff(i) + rhs.coeff(i); }
    };
    template <class D> struct FillExpr : VecBase<FillExpr<D>>
    {
        double fill;
        explicit FillExpr(double f) : fill(f) {}
        FillExpr(const FillExpr& other) : fill(other.fill) {}
        double coeff(int) const { return fill; }
    };
    struct Vec3 : VecBase<Vec3>
    {
        double v[3] = { 0.0, 0.0, 0.0 };
        Vec3() = default;
        Vec3(double a, double b, double c) : v{ a, b, c } {}
        Vec3(const Vec3& other) : v{ other.v[0], other.v[1], other.v[2] } {}
        Vec3& operator=(const Vec3& other)
        {
            for (int i = 0; i < 3; ++i) v[i] = other.v[i];
            return *this;
        }
        template <class D> Vec3(const VecBase<D>& other)
        {
            for (int i = 0; i < 3; ++i) v[i] = other.derived().coeff(i);
        }
        double coeff(int i) const { return v[i]; }
        double sum() const { return v[0] + v[1] + v[2]; }
    };
    struct ExplicitVec3
    {
        double total = 0.0;
        ExplicitVec3(const ExplicitVec3& other) : total(other.total) {}
        template <class D> explicit ExplicitVec3(const VecBase<D>& other)
        {
            for (int i = 0; i < 3; ++i) total += other.derived().coeff(i);
        }
        double sum() const { return total; }
    };
}

// A function template body odr-uses static constexpr members of an explicit specialization that
// nothing binds, so only that reference can get their storage emitted into the companion module
// (libc++ std::format's __bool_strings<char>::__true linked undefined the same way).
namespace cppi
{
    struct PtrArgClass
    {
        int value = 0;
        PtrArgClass(const int* p) : value(p == nullptr ? 44 : *p + 20) {}
    };
    inline int ptrarg_int(int value) noexcept { return value; }
    inline int ptrarg_int_ref(int& value) noexcept { return value; }
    inline int ptrarg_const_int_ref(const int& value) noexcept { return value; }
    inline int ptrarg_bool(bool value) noexcept { return value ? 9700 : 9701; }
    inline int ptrarg_bool_ref(bool& value) noexcept { value = true; return 9702; }
    inline int ptrarg_const_bool_ref(const bool& value) noexcept { return value ? 9703 : 9704; }
    inline int ptrarg_class_ref(PtrArgClass& value) noexcept { return value.value; }
    inline int ptrarg_const_class_ref(const PtrArgClass& value) noexcept { return value.value; }
    inline int ptrarg_class_value(PtrArgClass value) noexcept { return value.value; }
    inline int ptrarg_class_or_bool(PtrArgClass value) noexcept { return value.value; }
    inline int ptrarg_class_or_bool(bool& value) noexcept { value = true; return 9717; }
    struct PtrArgHost
    {
        int ptrarg_int(int value) const noexcept { return value; }
        int ptrarg_bool_ref(bool& value) const noexcept { value = true; return 9705; }
    };
    template <class T> inline int ptrarg_template_bool(bool value, T) noexcept
    { return value ? 9706 : 9707; }

    template <class T> struct IvStrings;
    template <> struct IvStrings<char>
    {
        static constexpr long yes = 5;
        static constexpr long no = 6;
    };
    template <class T> inline long iv_pick(T flag)
    {
        const long* picked = flag ? &IvStrings<char>::yes : &IvStrings<char>::no;
        return *picked;
    }
}

// Eigen `block` shape: member templates over the count types, with inline users recording the
// <int, long> and <long, int> specializations. An int-literal call still deduces <int, int>.
namespace cppi
{
    struct MtBlk { long r; long c; };
    template <class T> struct MtWidth { static const int v = sizeof(T); };
    template <class T> struct MtMat
    {
        T k = 0;
        template <class R, class C> MtBlk block(long i, long j, R rows, C cols)
        { return MtBlk{ (long)rows * 10 + MtWidth<R>::v, (long)cols * 10 + MtWidth<C>::v }; }
        template <class R, class C> MtBlk block(long i, long j, R rows, C cols) const
        { return MtBlk{ -1, -1 }; }
        template <int NR, int NC> MtBlk block(long i, long j) { return MtBlk{ NR, NC }; }
        MtBlk useA() { return block(0, 0, 1, 2L); }
        MtBlk useB() { return block(0, 0, 1L, 2); }
        long use() { return useA().r + useB().c; }
    };
    typedef MtMat<double> MtMatD;
}

namespace cppi
{
    struct MtRankBase
    {
        template<class A, class B> long pick(A, B) { return 5000 + sizeof(A) * 10 + sizeof(B); }
        long pick(int, int) { return 5007; }
    };
    struct MtRankDerived : MtRankBase {};
    struct MtRank
    {
        template<class A> long f(A) { return 100 + sizeof(A); }
        long f(int) { return 1; }
        long f(long) { return 2; }
        template<class A> long h(A) { return 200 + sizeof(A); }
        long h(double) { return 3; }
        template<class A, class B> long n(A, B) { return sizeof(A) * 10 + sizeof(B); }
        long n(int, int) { return 7; }
        template<class A, class B> static long staticPair(A, B)
        { return 1000 + sizeof(A) * 10 + sizeof(B); }
        static long staticPair(int, int) { return 1007; }
        template<class A> long explicitPick(A) { return 6000 + sizeof(A); }
        long explicitPick(int) { return 6001; }
        template<class A> long constrained(A) requires (sizeof(A) == 1)
        {
            static_assert(sizeof(A) == 0, "requires-excluded template must not instantiate");
            return 7000;
        }
        long constrained(int) { return 7001; }
        template<class A> long assertTie(A)
        {
            static_assert(sizeof(A) == 0, "non-template exact match should win");
            return 8000;
        }
        long assertTie(int) { return 8001; }
        template<class T> long body(T value) { return value.invalid(); }
        long body(int) { return 9001; }
        template<class T> long deleted(T) = delete;
        long deleted(int) { return 9002; }
        template<class T> long ambiguous(T, int) { return 9003; }
        template<class T> long ambiguous(int, T) { return 9004; }
        long ambiguous(int, int) { return 9005; }
        template<class T> long cv(T&) { return 9006; }
        long cv(const int&) { return 9007; }
        template<class T> long self(T) { return 9008; }
        long self(int) const { return 9009; }
        template<class T> long constReverse(T) const { return 9010; }
        long constReverse(int) { return 9011; }
        template<class T> long refPick(T) { return 9020; }
        long refPick(int& value) { value = 9; return 9021; }
        template<class T> long constArg(T&) { return 9030; }
        long constArg(int&) { return 9031; }
        template<class T> long ptrWrite(T* p) { *p = 5; return 9040; }
        long ptrWrite(long) { return 9041; }
        template<class T> long nullPick(T*) { return 9050; }
        long nullPick(int*) { return 9051; }
        template<class T> long arrPick(T) { return 9060 + sizeof(T); }
        long arrPick(int*) { return 9061; }
        template<class T> long charArrPick(T) { return 9070 + sizeof(T); }
        long charArrPick(const char*) { return 9071; }
        // A fixed array reaches a reference parameter as the array itself, not a pointer.
        template<class T> long arrRef(const T&) { return 9080 + sizeof(T); }
        long arrRef(long) { return 9081; }
        template<class T, std::size_t N> long arrExtent(T (&)[N]) { return 9090 + N; }
        long arrExtent(long) { return 9099; }
    };
    inline const MtRank& mt_rank_const_ref(MtRank& value) { return value; }
    inline const int& mt_rank_const_arg() { static const int value = 1; return value; }
    // A refused `put(int, const T&)` must not turn an arity miss of the template into a verdict.
    inline int b22MoveCtorCount = 0;
    inline int b22MoveMoveCount = 0;
    inline int b22MoveDtorCount = 0;
    struct MtNoCopy
    {
        int v = 0;
        MtNoCopy() { ++b22MoveCtorCount; }
        explicit MtNoCopy(int value) : v(value) { ++b22MoveCtorCount; }
        MtNoCopy(const MtNoCopy&) = delete;
        MtNoCopy(MtNoCopy&& other) noexcept : v(other.v)
        { other.v = -1; ++b22MoveMoveCount; }
        ~MtNoCopy() { ++b22MoveDtorCount; }
        int value() const { return v; }
    };
    inline void b22ResetMoveCounts()
    { b22MoveCtorCount = b22MoveMoveCount = b22MoveDtorCount = 0; }
    inline int b22MoveCounts()
    { return b22MoveCtorCount * 100 + b22MoveMoveCount * 10 + b22MoveDtorCount; }
    template<class T> struct MtSink
    {
        long put(int, const T& value) { T copy = value; return 1; }
        long put(int, T&&) { return 2; }
        template<class It> long put(int, It, It) { return 3; }
    };
    using MtSinkNoCopy = MtSink<MtNoCopy>;
    // Member `T *const &` returns: the const is the pointer slot, not the pointee.
    struct MtRankPtrs
    {
        int* const& intPtr() const { static int value = 1; static int* p = &value; return p; }
        MtRank* const& rank(MtRank& value) const
        { static MtRank* p = nullptr; p = &value; return p; }
    };
    // Pointee const of a C++ result reaches clang ranking, and const referents refuse writes.
    struct MtConstObj
    {
        int v = 7;
        template<class A> long g(A) { return 100 + sizeof(A); }
        long g(short) const { return 2; }
        // Templates only: the const and non-const wrappers share one CFlat signature.
        template<class A> long t2(A) const { return 1; }
        template<class A> long t2(A) { return 2; }
    };
    struct MtConstKind
    {
        template<class A> long kind(A) { return 50; }
        long kind(MtConstObj*) { return 1; }
        long kind(const MtConstObj*) { return 2; }
        template<class A> long tk(A*) { return 60; }
        template<class A> long tk(const A*) { return 61; }
    };
    struct MtConstHolder
    {
        MtConstObj s;
        const MtConstObj* cp = &s;
        MtConstObj* mp = &s;
        const MtConstObj* const* cpp = &cp;
        long lv = 5;
        const MtConstObj*& cpr() { return cp; }
        const MtConstObj* const& ccpr() { return cp; }
        MtConstObj const* const& eccpr() { return cp; }
        const MtConstObj* const* cpptr() { return cpp; }
        const MtConstObj* cpv() { return cp; }
        MtConstObj* const& mcr() { return mp; }
        MtConstObj*& mr() { return mp; }
        const long& clr() { return lv; }
        long& lr() { return lv; }
        const long&& crr() { return static_cast<const long&&>(lv); }
        long&& rr() { return static_cast<long&&>(lv); }
        const MtConstObj& csr() { return s; }
        MtConstObj& sr() { return s; }
    };
    inline MtConstHolder mtConstHolder;
    inline const MtConstObj* mt_const_cpv() { return mtConstHolder.cp; }
    inline MtConstObj* const& mt_const_mcr() { return mtConstHolder.mp; }
    inline const long& mt_const_clr() { return mtConstHolder.lv; }
    inline const long&& mt_const_crr() { return static_cast<const long&&>(mtConstHolder.lv); }
    inline const MtConstObj& mt_const_csr() { return mtConstHolder.s; }
    inline int mtConstInt = 3;
    inline int* const& mt_const_ipcr() { static int* p = &mtConstInt; return p; }
    // A const receiver (const namespace object, const& result, const T* result) takes the const
    // member of a const/non-const pair, never the non-const one on read-only storage.
    struct CrPair
    {
        int v = 3;
        int get() const { return v; }
        int get() { v += 10; return v; }
        int add(int k) const { return v + k; }
        int add(int k) { v += 10; return v + k; }
        int operator+(int k) const { return v + k + 100; }
        int operator+(int k) { v += 10; return v + k; }
        int only() { v += 1; return v; }
        int conly() const { return v + 1000; }
        int operator-() const { return v; }
        int operator-() { v += 10; return v; }
    };
    // The most-derived class declaring the name decides: CrHideConst hides the base pair with a
    // lone const get(), CrInherit inherits the pair unhidden.
    struct CrHideConst : CrPair { int get() const { return v + 900; } };
    struct CrInherit : CrPair {};
    inline const CrHideConst crHide{};
    inline const CrInherit crInherit{};
    // A const receiver ranks the whole const-callable set, twins plus unique-signature const members.
    struct CrMixed
    {
        int v = 3;
        int f(int) const { return 1; }
        int f(int) { v += 10; return 2; }
        int f(double) const { return 3; }
    };
    inline const CrMixed crMixed{};
    struct CrMixedGet
    {
        int v = 3;
        int get() const { return v; }
        int get() { v += 10; return v; }
        int get(int) { v += 20; return v; }
    };
    inline const CrMixedGet crMixedGet{};
    inline const CrPair crConst{};
    constexpr CrPair crConstexpr{};
    inline CrPair crMutable{};
    inline const CrPair& cr_const_ref() { return crConst; }
    inline const CrPair* cr_const_ptr() { return &crMutable; }
    inline const std::vector<int>& cr_cvec() { static std::vector<int> values; return values; }
    // Virtual const/non-const pairs: a const receiver dispatches the const overload virtually.
    struct CrVB { int v = 3; virtual int get() const { return v; } virtual int get() { v += 10; return v; }
        virtual ~CrVB() = default; };
    struct CrVD : CrVB { int get() const override { return v + 60; } int get() override { v += 20; return v; } };
    struct CrAbs { virtual int get() const = 0; virtual int get() = 0; virtual ~CrAbs() = default; };
    struct CrImpl : CrAbs { int v = 3; int get() const override { return v + 70; } int get() override { v += 10; return v; } };
    inline CrVD crVd{};
    inline CrImpl crImpl{};
    inline const CrVB& cr_vb_ref() { return crVd; }
    inline const CrVB* cr_vb_ptr() { return &crVd; }
    inline const CrAbs& cr_abs_ref() { return crImpl; }
    struct Cm2
    {
        int get() const { return 3; }
        int get() { return 13; }
        int operator-() const { return 3; }
        int operator-() { return 13; }
        int operator+(int k) const { return 100 + k; }
        int operator+(int k) { return 10 + k; }
        bool operator!() const { return false; }
        bool operator!() { return true; }
    };
    struct HC { const Cm2 c{}; };
    inline const HC hcGlobal{};
    inline HC hcMutable{};
    inline const HC& hc_ref() { return hcGlobal; }
    inline const HC* hc_ptr() { return &hcGlobal; }
    struct CrReceiverValue
    {
        int get() const { return 3; }
        int get() { return 13; }
        int only() { return 7; }
        int operator-() const { return 3; }
        int operator-() { return 13; }
    };
    inline int cr_take_mut(CrReceiverValue* p) { return p->only(); }
    inline int cr_take_mut_pp(CrReceiverValue** p) { return (*p)->only(); }
    inline int cr_take_const(const CrReceiverValue* p) { return p->get(); }
    struct CrTemplateReceiver
    {
        template<class T> int it(T) { return 44; }
        template<class T> int it(T) const { return 45; }
        template<class T> static int mix(T) { return 50; }
        template<class T> int mix(T, int) { return 51; }
        ~CrTemplateReceiver() {}
    };
    inline CrTemplateReceiver cr_template_make() { return CrTemplateReceiver{}; }
    template<class T> int cr_template_store(T* p) { *p = 5; return 5; }
    inline CrReceiverValue crReceiverGlobal{};
    struct CrFieldHolder
    {
        CrReceiverValue* p = &crReceiverGlobal;
        CrReceiverValue* const pc = &crReceiverGlobal;
        mutable CrReceiverValue mu{};
    };
    struct CrArrayHolder { CrReceiverValue arr[2]{}; };
    inline const CrArrayHolder crArrayConst{};
    inline CrFieldHolder crFieldMutable{};
    inline const CrFieldHolder crFieldConst{};
    inline const CrFieldHolder& cr_field_ref() { return crFieldConst; }
    struct Tm
    {
        template<class T> int f(T) { return 41; }
        template<class T> static int ts(T) { return 43; }
    };
    inline const Tm tmGlobal{};
    inline const Tm& tm_ref() { return tmGlobal; }
    inline const Tm* tm_ptr() { return &tmGlobal; }
    struct MaL
    {
        int only() const { return 7; }
        int only() { return 17; }
        int operator-() const { return 7; }
        int operator-() { return 17; }
    };
    struct MaR
    {
        int only() const { return 8; }
        int only() { return 18; }
        int operator-() const { return 8; }
        int operator-() { return 18; }
    };
    struct Ma : MaL, MaR {};
    inline const Ma maGlobal{};
    inline const Ma& ma_ref() { return maGlobal; }
    inline const Ma* ma_ptr() { return &maGlobal; }
}

// Member and free operators whose return type is a class-template specialization nothing has
// requested yet. Each result template is used by exactly one operator so the on-use retry is what
// requests it; a shared template would be requested by the first leg and hide the rest.
namespace cppi_opspec
{
    template <class L> struct OsMul { const L& l; double k; double eval() const { return l.v * k; }
                                      OsMul<L> operator*(double k2) const { return OsMul<L>{ l, k * k2 }; } };
    template <class L> struct OsNamed { double x; double get() const { return x; } };
    template <class L> struct OsEq { bool r; bool ok() const { return r; } };
    template <class L> struct OsRef { L* p; double get() const { return p->v; } };
    template <class L> struct OsNeg { const L& l; double eval() const { return -l.v; } };
    template <class L> struct OsIdx { double x; double get() const { return x; } };
    template <class L> struct OsCall { double x; double get() const { return x; } };
    template <class L> struct OsBad { double x; };
    struct OsOther { double v = 0.0; };
    struct OsW
    {
        double v = 0.0;
        OsOther other;
        OsW() = default;
        OsW(double x) : v(x) { other.v = x; }
        OsMul<OsW> operator*(double k) const { return OsMul<OsW>{ *this, k }; }
        OsMul<OsOther> operator/(double k) const { return OsMul<OsOther>{ other, k }; }
        OsNamed<OsW> operator-(double k) const { return OsNamed<OsW>{ v - k }; }
        OsEq<OsW> operator==(double k) const { return OsEq<OsW>{ v == k }; }
        OsRef<OsW>& operator*=(double k) { v *= k; static OsRef<OsW> r; r.p = this; return r; }
        OsNeg<OsW> operator-() const { return OsNeg<OsW>{ *this }; }
        OsIdx<OsW> operator[](int i) const { return OsIdx<OsW>{ v + i }; }
        OsCall<OsW> operator()(double d) const { return OsCall<OsW>{ v * d }; }
        // Refused for a reason other than the specialization: a pointer-to-member parameter.
        OsBad<OsW>& operator%=(double OsW::* p) { static OsBad<OsW> b; b.x = v; return b; }
    };
    // The operators sit on a base the legs never use directly, so a derived receiver is the only
    // way their result templates get requested.
    template <class L> struct OsDMul { const L& l; double k; double eval() const { return l.v * k; } };
    template <class L> struct OsDNeg { const L& l; double eval() const { return -l.v; } };
    template <class L> struct OsDIdx { double x; double get() const { return x; } };
    template <class L> struct OsDCall { double x; double get() const { return x; } };
    template <class L> struct OsDRef { L* p; double get() const { return p->v; } };
    struct OsBase
    {
        double v = 0.0;
        OsBase() = default;
        OsBase(double x) : v(x) {}
        OsDMul<OsBase> operator*(double k) const { return OsDMul<OsBase>{ *this, k }; }
        OsDNeg<OsBase> operator-() const { return OsDNeg<OsBase>{ *this }; }
        OsDIdx<OsBase> operator[](int i) const { return OsDIdx<OsBase>{ v + i }; }
        OsDCall<OsBase> operator()(double d) const { return OsDCall<OsBase>{ v * d }; }
        OsDRef<OsBase>& operator+=(double k) { v += k; static OsDRef<OsBase> r; r.p = this; return r; }
    };
    struct OsDerived : OsBase { OsDerived(double x) : OsBase(x) {} };
    // Free operators: a compound one returning a reference to a specialization, and a binary one.
    template <class L> struct OsFreeRef { L* p; double get() const { return p->v; } };
    template <class L> struct OsFreeMul { double x; double get() const { return x; } };
    struct OsFw { double v = 0.0; OsFw() = default; OsFw(double x) : v(x) {} };
    struct OsFv { double v = 0.0; OsFv() = default; OsFv(double x) : v(x) {} };
    inline OsFreeRef<OsFw>& operator*=(OsFw& a, double k) { a.v *= k; static OsFreeRef<OsFw> r; r.p = &a; return r; }
    inline OsFreeMul<OsFv> operator*(const OsFv& a, double k) { return OsFreeMul<OsFv>{ a.v * k }; }
    // An rvalue-reference operand: the operator steals from the moved source (v -> 0).
    template <class L> struct OsMvRef { L* p; double get() const { return p->v; } };
    struct OsMv
    {
        double v = 0.0;
        OsMv() = default;
        OsMv(double x) : v(x) {}
        OsMvRef<OsMv>& operator&=(OsMv&& o) { v += o.v; o.v = 0.0; static OsMvRef<OsMv> r; r.p = this; return r; }
    };
    struct OsMv2
    {
        double v = 0.0;
        OsMv2() = default;
        OsMv2(double x) : v(x) {}
        OsMv2& operator&=(OsMv2&& o) { v += o.v; o.v = 0.0; return *this; }
        OsMv2 operator&(OsMv2&& o) const { OsMv2 r(v + o.v); o.v = 0.0; return r; }
    };
    // Prefix ++/-- returning an unrequested specialization; postfix `z++` binds the prefix form.
    template <class L> struct OsIncRef { L* p; double get() const { return p->v; } };
    struct OsInc
    {
        double v = 0.0;
        OsInc() = default;
        OsInc(double x) : v(x) {}
        OsIncRef<OsInc>& operator++() { v += 1.0; static OsIncRef<OsInc> r; r.p = this; return r; }
        OsIncRef<OsInc>& operator--() { v -= 1.0; static OsIncRef<OsInc> r; r.p = this; return r; }
    };
}

// Pointer returns to unrequested class-template specializations are requested only when the
// member is used, so the opaque pointer cannot hide the pointee type from a chained access.
namespace cppi_retryptr
{
    template <class T> struct PtrResult { T* p; double get() const { return p->v; } };
    template <class T> struct ArrowResult { T* p; double get() const { return p->v; } };
    template <class T> struct CompoundResult { T* p; double get() const { return p->v; } };
    struct PtrOwner
    {
        double v = 0.0;
        PtrOwner() = default;
        PtrOwner(double x) : v(x) {}
        PtrResult<PtrOwner>* ptr() { static PtrResult<PtrOwner> r; r.p = this; return &r; }
    };
    struct ArrowOwner
    {
        double v = 0.0;
        ArrowOwner() = default;
        ArrowOwner(double x) : v(x) {}
        ArrowResult<ArrowOwner>* operator->() { static ArrowResult<ArrowOwner> r; r.p = this; return &r; }
    };
    struct CompoundOwner
    {
        double v = 0.0;
        CompoundOwner() = default;
        CompoundOwner(double x) : v(x) {}
        CompoundResult<CompoundOwner>* operator*=(double k)
        {
            v *= k;
            static CompoundResult<CompoundOwner> r;
            r.p = this;
            return &r;
        }
    };
}
// Explicit `move` of a NON-trivial class into a `T&&` / by-value operator parameter (legs 31700+).
namespace cppi_opmv
{
#define CPPI_OPMV_COUNTS int v; static inline int cp = 0, mv = 0, dt = 0; \
    static void reset() { cp = 0; mv = 0; dt = 0; } \
    static int CP() { return cp; } static int MV() { return mv; } static int DT() { return dt; }
#define CPPI_OPMV_MEMBER(N) \
    N& operator&=(N&& o) { v += o.v; o.v = 0; return *this; } \
    N operator&(N&& o) const { N r(v + o.v); o.v = 0; return r; }
#define CPPI_OPMV_FREE(N) \
    inline N& operator&=(N& a, N&& o) { a.v += o.v; o.v = 0; return a; } \
    inline N operator&(const N& a, N&& o) { N r(a.v + o.v); o.v = 0; return r; }
    struct MO { CPPI_OPMV_COUNTS MO(int x = 0) : v(x) {} MO(const MO&) = delete;
        MO(MO&& o) noexcept : v(o.v) { o.v = 0; ++mv; } ~MO() { ++dt; } CPPI_OPMV_MEMBER(MO) };
    struct UD { CPPI_OPMV_COUNTS UD(int x = 0) : v(x) {} ~UD() { ++dt; } CPPI_OPMV_MEMBER(UD) };
    struct UC { CPPI_OPMV_COUNTS UC(int x = 0) : v(x) {} UC(const UC& o) : v(o.v) { ++cp; }
        UC& operator=(const UC&) = default; CPPI_OPMV_MEMBER(UC) };
    struct UM { CPPI_OPMV_COUNTS UM(int x = 0) : v(x) {} UM(const UM&) = default;
        UM(UM&& o) noexcept : v(o.v) { o.v = 0; ++mv; } CPPI_OPMV_MEMBER(UM) };
    struct FMO { CPPI_OPMV_COUNTS FMO(int x = 0) : v(x) {} FMO(const FMO&) = delete;
        FMO(FMO&& o) noexcept : v(o.v) { o.v = 0; ++mv; } ~FMO() { ++dt; } };
    struct FUD { CPPI_OPMV_COUNTS FUD(int x = 0) : v(x) {} ~FUD() { ++dt; } };
    struct FUC { CPPI_OPMV_COUNTS FUC(int x = 0) : v(x) {} FUC(const FUC& o) : v(o.v) { ++cp; }
        FUC& operator=(const FUC&) = default; };
    struct FUM { CPPI_OPMV_COUNTS FUM(int x = 0) : v(x) {} FUM(const FUM&) = default;
        FUM(FUM&& o) noexcept : v(o.v) { o.v = 0; ++mv; } };
    CPPI_OPMV_FREE(FMO) CPPI_OPMV_FREE(FUD) CPPI_OPMV_FREE(FUC) CPPI_OPMV_FREE(FUM)
    // `T&&` beside `const T&`: an explicit move picks `T&&` (x100), an lvalue `const T&`.
    struct OV { CPPI_OPMV_COUNTS OV(int x = 0) : v(x) {} ~OV() { ++dt; }
        OV& operator&=(OV&& o) { v += 100 * o.v; o.v = 0; return *this; }
        OV& operator&=(const OV& o) { v += o.v; return *this; }
        OV operator&(OV&& o) const { return OV(v + 100 * o.v); }
        OV operator&(const OV& o) const { return OV(v + o.v); } };
    struct FOV { CPPI_OPMV_COUNTS FOV(int x = 0) : v(x) {} ~FOV() { ++dt; } };
    inline FOV& operator&=(FOV& a, FOV&& o) { a.v += 100 * o.v; o.v = 0; return a; }
    inline FOV& operator&=(FOV& a, const FOV& o) { a.v += o.v; return a; }
    // By-value operand: `move d` move-constructs the parameter (clang), never copies.
    struct BV { CPPI_OPMV_COUNTS BV(int x = 0) : v(x) {} BV(const BV& o) : v(o.v) { ++cp; }
        BV(BV&& o) noexcept : v(o.v) { o.v = 0; ++mv; } ~BV() { ++dt; }
        BV& operator&=(BV o) { v += o.v; return *this; } };
    struct FBV { CPPI_OPMV_COUNTS FBV(int x = 0) : v(x) {} FBV(const FBV& o) : v(o.v) { ++cp; }
        FBV(FBV&& o) noexcept : v(o.v) { o.v = 0; ++mv; } ~FBV() { ++dt; } };
    inline FBV& operator&=(FBV& a, FBV o) { a.v += o.v; return a; }
    // A constructor-call prvalue binds `T&&` (trivial class).
    struct TQ { int v; TQ(int x = 0) : v(x) {} TQ& operator&=(TQ&& o) { v += o.v; o.v = 0; return *this; } };
#undef CPPI_OPMV_COUNTS
#undef CPPI_OPMV_MEMBER
#undef CPPI_OPMV_FREE
}

// ST4 - naming C++ entities from CFlat: static data members through a chained path, a
// parenthesized path and an instance; a static member FUNCTION called through an instance;
// a field-less scoped enum (std::byte shape) named as a type; a variable template read as a
// value (std::numbers::pi_v shape); a trait specialized for decltype(nullptr) vs void*.
namespace cppi_entity
{
    struct Point { int x = 0; int y = 0; };
    struct StaticData { inline static Point origin{8, 9}; };
    struct OutOfLine { int pad = 0; static int K; };
    inline int OutOfLine::K = 17;
    template <class T, T... I>
    struct Seq { static constexpr unsigned long size() noexcept { return sizeof...(I); } };
    struct Counted { int pad = 0; static int twice(int v) noexcept { return v * 2; } };
    enum class byte_like : unsigned char {};
    template <class I> constexpr I to_int(byte_like b) noexcept { return static_cast<I>(b); }
    template <class T> inline constexpr T e_v = T(2.718281828459045);
    template <class T> inline constexpr int width_v = (int)sizeof(T) * 10 + 1;
    template <class T> struct is_null_ptr { static constexpr bool value = false; };
    template <> struct is_null_ptr<decltype(nullptr)> { static constexpr bool value = true; };
    // Side-effect initializers: never folded, read as the live object (or refused for templates).
    inline int dyn_counter = 0;
    inline const int dyn_const = (++dyn_counter, 27);
    template <class T> inline const int dyn_tmpl = (++dyn_counter, 27);
    inline int dynCount() noexcept { return dyn_counter; }
    // Same name as a static and a non-static overload.
    struct Mixed
    {
        static int f(int) noexcept { return 101; }
        int f(double) const noexcept { return 202; }
    };
}

// T73: implicit non-trivial default constructors must run for CFlat struct fields.
namespace cpp_t73
{
    inline int ctor_count = 0;
    inline int dtor_count = 0;
    struct Leaf
    {
        int value;
        Leaf() : value(7) { ++ctor_count; }
        ~Leaf() { ++dtor_count; }
    };
    struct Mix { Leaf first; Leaf second; };
    struct Trivial { int value; };
    struct Virtual
    {
        int value;
        Virtual() : value(19) { ++ctor_count; }
        virtual int get() const { return value; }
        virtual ~Virtual() { ++dtor_count; }
    };
    struct DefaultMember { int value = 5; };
}
