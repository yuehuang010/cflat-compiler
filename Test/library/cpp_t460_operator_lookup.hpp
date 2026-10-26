#pragma once

namespace t460
{
    struct BuiltinAmbA
    {
        int v;
        operator int() const { return v; }
    };

    struct BuiltinAmbB
    {
        int v;
        BuiltinAmbB(const BuiltinAmbA& source) : v(source.v + 100) {}
    };

    inline int operator+(const BuiltinAmbB&, int) { return 7; }

    struct HiddenFriendTarget
    {
        int v;
        friend int operator+(const HiddenFriendTarget& a, const HiddenFriendTarget& b)
        {
            return a.v + b.v + 51;
        }
    };

    struct HiddenConvSource
    {
        int v;
        operator HiddenFriendTarget() const { return HiddenFriendTarget{v}; }
    };

    struct UnrelatedOperand { int v; };
    struct UnrelatedFriendOwner
    {
        friend int operator+(const UnrelatedOperand&, const UnrelatedOperand&) { return 52; }
    };

    struct ExactFriend
    {
        int v;
        friend int operator+(const ExactFriend& a, const ExactFriend& b)
        {
            return a.v + b.v + 40;
        }
    };

    struct NamedFriend
    {
        int v;
        friend int f(const NamedFriend& value) { return value.v + 70; }
    };

    struct MemberBuiltin
    {
        int v;
        operator int() const { return v; }
        int operator+(int value) const { return v + value + 100; }
    };

    struct ExplicitSource
    {
        int v;
        explicit operator int() const { return v; }
    };

    struct ExplicitTarget
    {
        int v;
        ExplicitTarget(const ExplicitSource& source) : v(source.v + 100) {}
    };

    inline int operator+(const ExplicitTarget&, int) { return 78; }

}

namespace t460_enum
{
    enum E { one = 2 };
    inline int operator+(E value, int rhs)
    {
        return 20 + static_cast<int>(value) + rhs;
    }
}

namespace t460_adj
{
    struct T { int v; };
}

namespace t460
{
    struct GlobalSource
    {
        int v;
        operator t460_adj::T() const { return t460_adj::T{v}; }
    };
}

inline int operator+(const t460_adj::T&, const t460_adj::T&) { return 64; }

namespace r {}

namespace t460r2
{
    struct Associated
    {
        int v;
        friend int f(const Associated& value) { return value.v + 70; }
    };

    struct NamedVisible
    {
        int v;
        friend int f(const NamedVisible& value) { return value.v + 70; }
    };

    inline int f(int value) { return value + 10; }

    struct BaseInt { operator int() const { return 2; } };
    struct InheritedInt : BaseInt {};
    struct InheritedBox { InheritedBox(const InheritedInt&) {} };
    inline int operator+(const InheritedBox&, int) { return 7; }

    struct PrivateBaseInt { operator int() const { return 2; } };
    struct PrivateBaseDerived : private PrivateBaseInt {};
    struct PrivateBaseBox { PrivateBaseBox(const PrivateBaseDerived&) {} };
    inline int operator+(const PrivateBaseBox&, int) { return 71; }

    struct ExplicitOverride : BaseInt { explicit operator int() const { return 3; } };
    struct ExplicitOverrideBox { ExplicitOverrideBox(const ExplicitOverride&) {} };
    inline int operator+(const ExplicitOverrideBox&, int) { return 72; }

    struct DoubleSource { operator double() const { return 2.5; } };
    struct DoubleBox { DoubleBox(const DoubleSource&) {} };
    inline int operator%(const DoubleBox&, int) { return 17; }
    inline int operator&(const DoubleBox&, int) { return 18; }
    inline int operator|(const DoubleBox&, int) { return 19; }
    inline int operator^(const DoubleBox&, int) { return 20; }
    inline int operator<<(const DoubleBox&, int) { return 21; }
    inline int operator>>(const DoubleBox&, int) { return 22; }

    struct FloatSource { operator float() const { return 2.5f; } };
    struct FloatBox { FloatBox(const FloatSource&) {} };
    inline int operator%(const FloatBox&, int) { return 27; }
    inline int operator&(const FloatBox&, int) { return 28; }
    inline int operator|(const FloatBox&, int) { return 29; }
    inline int operator^(const FloatBox&, int) { return 30; }
    inline int operator<<(const FloatBox&, int) { return 31; }
    inline int operator>>(const FloatBox&, int) { return 32; }

    struct BoolSource { operator bool() const { return true; } };
    struct BoolBox { BoolBox(const BoolSource&) {} };
    inline int operator%(const BoolBox&, int) { return 37; }
    inline int operator&(const BoolBox&, int) { return 38; }
    inline int operator|(const BoolBox&, int) { return 39; }
    inline int operator^(const BoolBox&, int) { return 40; }
    inline int operator<<(const BoolBox&, int) { return 41; }
    inline int operator>>(const BoolBox&, int) { return 42; }

    enum class Scoped { two = 2 };
    struct ScopedSource { operator Scoped() const { return Scoped::two; } };
    struct ScopedBox { ScopedBox(const ScopedSource&) {} };
    inline int operator%(const ScopedBox&, int) { return 47; }
    inline int operator&(const ScopedBox&, int) { return 48; }
    inline int operator|(const ScopedBox&, int) { return 49; }
    inline int operator^(const ScopedBox&, int) { return 50; }
    inline int operator<<(const ScopedBox&, int) { return 51; }
    inline int operator>>(const ScopedBox&, int) { return 52; }
}

namespace t460r2
{
    struct MutablePointerPlus
    {
        friend int operator+(MutablePointerPlus&, char*) { return 90; }
    };
    struct Text { const char* c_str() const { return "text"; } };
}

// T460 r4: a hidden friend of a global class (unqualified ADL), beside a visible overload, and a
// visible overload that ranks against a hidden friend of the same name.
struct T460GlobalHidden { friend int t460_global_f(const T460GlobalHidden&) { return 84; } };
struct T460GlobalBeside { friend int t460_beside_f(const T460GlobalBeside&) { return 85; } };
inline int t460_beside_f(int) { return 33; }
struct T460EnumHolder { enum E { one = 1 }; friend int t460_enum_f(E) { return 89; } };
namespace t460gate
{
    struct X { friend int f(X&, int) { return 82; } };
    inline int f(const X&, long) { return 31; }
}
// A deleted conversion still ranks: clang reports the built-in candidate ambiguous.
namespace t460del
{
    struct Deleted { operator int() const = delete; };
    struct DeletedBox { DeletedBox(const Deleted&) {} };
    inline int operator+(const DeletedBox&, int) { return 61; }
}
