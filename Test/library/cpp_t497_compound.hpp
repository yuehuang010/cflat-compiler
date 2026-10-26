namespace cpp_t497
{
    struct MemberAssign
    {
        int value;
        MemberAssign(int n = 0) : value(n) {}
        MemberAssign operator+(int n) const { return MemberAssign(value + n + 100); }
        MemberAssign& operator+=(int n) { value += n + 10; return *this; }
    };

    struct FreeAssign { int value; };
    inline FreeAssign operator+(FreeAssign a, int n)
    { a.value += n + 100; return a; }
    inline FreeAssign& operator+=(FreeAssign& a, int n)
    { a.value += n + 10; return a; }
    inline FreeAssign makeFreeAssign() { FreeAssign a; a.value = 2; return a; }

    struct RefQualifiedAssign
    {
        int value;
        RefQualifiedAssign& operator+=(int n) & { value += n + 10; return *this; }
    };
    inline RefQualifiedAssign makeRefQualifiedAssign()
    { RefQualifiedAssign a; a.value = 2; return a; }

    struct RightAssign { int value; };
    inline int& operator+=(int& a, const RightAssign& b)
    { a += b.value + 20; return a; }

    struct BinaryOnly
    {
        int value;
        BinaryOnly(int n = 0) : value(n) {}
        BinaryOnly operator+(int n) const { return BinaryOnly(1000 + n); }
        BinaryOnly operator-(int n) const { return BinaryOnly(1100 + n); }
        BinaryOnly operator*(int n) const { return BinaryOnly(1200 + n); }
        BinaryOnly operator/(int n) const { return BinaryOnly(1300 + n); }
        BinaryOnly operator%(int n) const { return BinaryOnly(1400 + n); }
        BinaryOnly operator&(int n) const { return BinaryOnly(1500 + n); }
        BinaryOnly operator|(int n) const { return BinaryOnly(1600 + n); }
        BinaryOnly operator^(int n) const { return BinaryOnly(1700 + n); }
        BinaryOnly operator<<(int n) const { return BinaryOnly(1800 + n); }
        BinaryOnly operator>>(int n) const { return BinaryOnly(1900 + n); }
    };

    struct PrivateAssign
    {
        int value;
        PrivateAssign(int n = 0) : value(n) {}
        PrivateAssign operator+(int n) const { return PrivateAssign(value + n + 100); }
    private:
        PrivateAssign& operator+=(int n) { value += n + 10; return *this; }
    };

    struct AmbiguousAssign
    {
        int value;
        AmbiguousAssign(int n = 0) : value(n) {}
        AmbiguousAssign operator+(int n) const { return AmbiguousAssign(value + n + 100); }
        AmbiguousAssign& operator+=(short n) { value += n + 10; return *this; }
        AmbiguousAssign& operator+=(unsigned short n) { value += n + 20; return *this; }
    };

    struct FreeBinaryOnly { int value; };
    inline FreeBinaryOnly operator+(FreeBinaryOnly a, int n)
    { a.value += n + 100; return a; }
}

namespace cpp_t497
{
    namespace r2
    {
        template <class T> struct TemplateAssign { T value; };
        template <class T> TemplateAssign<T>& operator+=(TemplateAssign<T>& a, T n)
        { a.value += n + 10; return a; }

        inline int memberHits = 0;
        struct MA
        {
            int value;
            MA operator+(int n) const { MA m; m.value = value + n + 100; return m; }
            MA& operator+=(int n) { value += n + 10; ++memberHits; return *this; }
        };

        inline MA mkMA() { MA m; m.value = 2; return m; }
        inline int getMemberHits() { return memberHits; }
    }
    // Template compound operator whose right parameter is a non-const reference: a literal cannot bind it.
    namespace rref
    {
        struct RightRef { int value; };
        template <class T> RightRef& operator+=(RightRef& a, T& n) { a.value += n; return a; }
    }
}
