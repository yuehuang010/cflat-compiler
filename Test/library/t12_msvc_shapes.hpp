#pragma once
#include <cstddef>
#include <type_traits>
// MSVC <memory>/<system_error> shapes on any host: a harvested operator==<int,int> specialization
// must not take a nullptr operand, and a mixed-type hidden friend is found only by ADL.
namespace t12 {
template<class T> struct Sp {
    T* p;
    Sp(): p(nullptr) {}
    Sp(std::nullptr_t): p(nullptr) {}
    explicit Sp(T* q): p(q) {}
    T& operator*() const { return *p; }
    T* operator->() const { return p; }
    T* get() const { return p; }
};
template<class T1, class T2> bool operator==(const Sp<T1>& a, const Sp<T2>& b) { return a.get() == b.get(); }
template<class T> bool operator==(const Sp<T>& a, std::nullptr_t) { return a.get() == nullptr; }
template<class T1, class T2> bool operator<(const Sp<T1>& a, const Sp<T2>& b) { return a.get() < b.get(); }
inline int spValue = 7;
inline Sp<int> makeSp() { return Sp<int>(&spValue); }
enum class Ec { a = 1, b = 2 };
template<class E> struct is_cond_enum : std::false_type {};
template<> struct is_cond_enum<Ec> : std::true_type {};
class Cond {
public:
    int v;
    Cond(): v(0) {}
    template<class E, std::enable_if_t<is_cond_enum<E>::value, int> = 0> Cond(E e): v((int)e + 100) {}
    int value() const { return v; }
    friend bool operator==(const Cond& l, const Cond& r) { return l.v == r.v; }
};
template<class T> class CodeT {
public:
    T v;
    CodeT(): v(0) {}
    explicit CodeT(T x): v(x) {}
    friend bool operator==(const CodeT& l, const CodeT& r) { return l.v == r.v; }
    friend bool operator==(const CodeT& l, const Cond& r) { return l.v + 100 == r.v; }
};
inline CodeT<int> makeCode(int x) { return CodeT<int>(x); }
// MSVC <bitset> shape: the proxy is a plain member class of the class template, and the const
// operator[] comes first. Returning it by value needs the lazily instantiated member completed.
template<std::size_t N> class Bits {
public:
    class reference {
        friend Bits;
    public:
        reference(const reference&) noexcept = default;
        ~reference() noexcept {}
        reference& operator=(const bool v) noexcept { b->set(pos, v); return *this; }
        reference& operator=(const reference& r) noexcept { b->set(pos, static_cast<bool>(r)); return *this; }
        bool operator~() const noexcept { return !b->test(pos); }
        operator bool() const noexcept { return b->test(pos); }
        reference& flip() noexcept { b->set(pos, !b->test(pos)); return *this; }
    private:
        reference(Bits& bits, std::size_t p) noexcept : b(&bits), pos(p) {}
        Bits* b;
        std::size_t pos;
    };
    constexpr bool operator[](std::size_t p) const noexcept { return test(p); }
    reference operator[](std::size_t p) noexcept { return reference(*this, p); }
    // Same proxy through operator() and unary operator*, const overload again declared first.
    bool operator()(std::size_t p) const noexcept { return test(p); }
    reference operator()(std::size_t p) noexcept { return reference(*this, p); }
    bool operator*() const noexcept { return test(0); }
    reference operator*() noexcept { return reference(*this, 0); }
    constexpr bool test(std::size_t p) const noexcept { return ((word >> p) & 1u) != 0; }
    void set(std::size_t p, bool v) noexcept { if (v) word |= (1ul << p); else word &= ~(1ul << p); }
    unsigned long word = 0;
};
// The same proxy pair where clang spells the owner with an apostrophe (CharBox<'a'>).
template<char C> class CharBox {
public:
    struct reference { int* p; ~reference() {} reference& flip() { *p ^= 1; return *this; } int tag() const { return C; } };
    int operator()(int) const { return 99; }
    reference operator()(int) { return reference{&word}; }
    int word = 0;
};
// The proxy pair behind an earlier, unrelated refused sibling of the same name: an incomplete
// by-value return (operator()), a deleted overload (operator[]), a private one (at).
struct MaskOther;
template<int N> class Mask {
    int at(double);
public:
    struct reference { int* p; ~reference() {} reference& flip() { *p ^= 1; return *this; } int tag() const { return N; } };
    MaskOther operator()(double);
    int operator()(int) const { return 99; }
    reference operator()(int) { return reference{&word}; }
    int operator[](double) = delete;
    int operator[](int) const { return 98; }
    reference operator[](int) { return reference{&word}; }
    int at(int) const { return 97; }
    reference at(int) { return reference{&word}; }
    int word = 0;
};
// Top-level parameter cv is not part of the function type: `const int` ties `int`, `int *const`
// ties `int *`, so the non-const proxy overload still wins on the object.
template<int N> class CvBox {
public:
    struct reference { int* p; ~reference() {} operator int() const { return *p; } };
    int operator()(const int) const { return 99; }
    reference operator()(int) { return reference{&word}; }
    int operator[](int *const) const { return 98; }
    reference operator[](int *) { return reference{&word}; }
    int word = 7;
};
}
