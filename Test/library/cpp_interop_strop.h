#pragma once
// Fixture for section M94: C++ operators declared over `const char*`, which a CFlat string
// literal has to reach through the same conversion an ordinary C++ call argument uses.
#include <string>
#include <cstring>

namespace cppsop {

// Both a const char* and a std::string overload: a literal must pick the char* leg (tag 1),
// exactly as C++ does, while a real std::string argument picks the std::string leg (tag 2).
class Acc {
public:
    Acc() : n_(0), tag_(0) {}
    Acc& operator+=(const char* s) { n_ += (int)std::strlen(s); tag_ = 1; return *this; }
    Acc& operator+=(const std::string& s) { n_ += (int)s.size(); tag_ = 2; return *this; }
    Acc& operator+=(char c) { n_ += 1; tag_ = 3; return *this; }
    int n() const { return n_; }
    int tag() const { return tag_; }
private:
    int n_;
    int tag_;
};

// Member operators over const char* only.
class CharOnly {
public:
    CharOnly() : n_(0) {}
    CharOnly& operator+=(const char* s) { n_ += (int)std::strlen(s); return *this; }
    bool operator==(const char* s) const { return (int)std::strlen(s) == n_; }
    int n() const { return n_; }
private:
    int n_;
};

// A member operator taking `char`: a string literal must NOT bind to it.
// Test/errors/err_cpp_string_literal_operator_char.cb pins that refusal.
class CharParam {
public:
    CharParam() : n_(0) {}
    CharParam& operator+=(char c) { n_ += 1; return *this; }
    int n() const { return n_; }
private:
    int n_;
};

// FREE operators with a const char* parameter, on each side of the class operand.
class Free {
public:
    Free() : n_(0) {}
    int n() const { return n_; }
    int n_;
};

inline Free operator+(const Free& f, const char* s)
{ Free r; r.n_ = f.n_ + (int)std::strlen(s); return r; }
inline Free operator+(const char* s, const Free& f)
{ Free r; r.n_ = f.n_ + 100 + (int)std::strlen(s); return r; }
inline bool operator==(const Free& f, const char* s)
{ return (int)std::strlen(s) == f.n_; }

// PRIMITIVE IDENTITY at a C++ template boundary. A template argument is deduced from the
// SPELLING the wrapper writes, so each CFlat primitive must land on its own C++ type: `char`
// is not `signed char`, `long` is not `long long`, `wchar` is not `int`. One code per C++
// type makes a wrong spelling a wrong VALUE instead of a silent rebind.
template<class C> struct TypeCode { static const int value = 0; };
template<> struct TypeCode<char> { static const int value = 1; };
template<> struct TypeCode<signed char> { static const int value = 2; };
template<> struct TypeCode<unsigned char> { static const int value = 3; };
template<> struct TypeCode<bool> { static const int value = 4; };
template<> struct TypeCode<short> { static const int value = 5; };
template<> struct TypeCode<unsigned short> { static const int value = 6; };
template<> struct TypeCode<int> { static const int value = 7; };
template<> struct TypeCode<unsigned int> { static const int value = 8; };
template<> struct TypeCode<long> { static const int value = 9; };
template<> struct TypeCode<unsigned long> { static const int value = 10; };
template<> struct TypeCode<long long> { static const int value = 11; };
template<> struct TypeCode<unsigned long long> { static const int value = 12; };
template<> struct TypeCode<char16_t> { static const int value = 13; };
template<> struct TypeCode<wchar_t> { static const int value = 14; };
template<> struct TypeCode<float> { static const int value = 15; };
template<> struct TypeCode<double> { static const int value = 16; };
template<> struct TypeCode<char32_t> { static const int value = 17; };
template<> struct TypeCode<char8_t> { static const int value = 18; };
template<> struct TypeCode<long double> { static const int value = 19; };

template<class C> int typeCodeOf(C v) { (void)v; return TypeCode<C>::value; }

template<class T> struct LiteralBox {
    int code;
    LiteralBox(T) : code(TypeCode<T>::value) {}
};

// A user function template over basic_string<C> and C: deduction fails unless both operands
// carry the SAME character identity, which is the shape libc++'s own string operators have.
template<class C> int countChar(const std::basic_string<C>& s, C c)
{
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) if (s[i] == c) ++n;
    return n;
}

struct LiteralCtor {
    int code;
    LiteralCtor(char) : code(1) {}
    LiteralCtor(int) : code(7) {}
};

struct LiteralMember {
    int value;
    LiteralMember() : value(0) {}
    int pick(char) { return 1; }
    int pick(int) { return 7; }
    int find(char) { return 1; }
    int find(int) { return 7; }
    void push_back(char c) { value = c; }
    void push_back(int i) { value = i + 1000; }
};

struct LiteralOps {
    int value;
    LiteralOps() : value(0) {}
    LiteralOps operator+(char) const { LiteralOps r; r.value = 1; return r; }
    LiteralOps operator+(int) const { LiteralOps r; r.value = 7; return r; }
    LiteralOps& operator+=(char) { value = 1; return *this; }
    LiteralOps& operator+=(int) { value = 7; return *this; }
    bool operator==(char) const { return true; }
    bool operator==(int) const { return false; }
};

// Last byte of a string, so a concatenation leg can assert WHICH byte was appended.
inline int stropTail(const std::string& s)
{ return s.empty() ? -1 : (int)(unsigned char)s[s.size() - 1]; }

}

// Free operators whose right operand is a pointer of a SPECIFIC depth (`char**` vs `const char*`)
// or an enum class beside `int`: the operand's own depth / enum type picks the overload.
namespace cppsopd {
struct Tally { int v = 0; };
enum class Pick { A = 1, B = 2 };
inline int operator-(const Tally& t, char** pp) { return t.v - (int)(*pp)[0]; }
inline int operator-(const Tally& t, const char* p) { return t.v - (int)p[0] + 100000; }
inline int operator+(const Tally& t, int x) { return t.v + x + 1000; }
inline int operator+(const Tally& t, Pick p) { return t.v + (int)p * 10; }
// Only a `char**` overload: a `char*` row must be refused, as clang does.
struct Deep { int v = 0; };
inline int operator-(const Deep& d, char** pp) { return d.v - (int)(*pp)[0]; }
// T122: the JSON-shaped subscript - string and integer operator[] overloads, each with a
// const twin. A literal key must reach the std::string leg through the subscript path.
struct TinyIndex {
    int operator[](std::string key) { return (int)key.size() + 10; }
    int operator[](const std::string& key) const { return (int)key.size() + 20; }
    int operator[](unsigned long index) { return (int)index + 30; }
    int operator[](unsigned long index) const { return (int)index + 40; }
};
struct TinyStaticTwin {
    static int s(int value) { return value; }
    int s() { return 1; }
    int s() const { return 2; }
};
struct TinyConversionTwin {
    operator int() { return 1; }
    operator int() const { return 2; }
};
struct TinyConstVolatileLRef {
    int f() const volatile & { return 601; }
};
// T122 r4: the valarray subscript shape - integer twins plus by-value and const-ref class-index
// twins whose index class (a std template instantiation) is not registered before the first call.
struct TinyProxy { int first; int n; };
struct TinySliced {
    int d[4] = {0, 1, 2, 3};
    const int& operator[](unsigned long i) const { return d[i]; }
    int& operator[](unsigned long i) { return d[i]; }
    int operator[](std::u16string_view s) const { return (int)s.size() + 100; }
    TinyProxy operator[](std::u16string_view s) { TinyProxy p; p.first = d[s.size()]; p.n = 200; return p; }
    int operator[](const std::u32string_view& m) const { return (int)m.size() + 300; }
    TinyProxy operator[](const std::u32string_view& m) { TinyProxy p; p.first = d[m.size() + 1]; p.n = 400; return p; }
};

// T127: the MSVC _Vector_iterator shape - the derived iterator re-exposes the base's const
// operator- overload set with a using-declaration and declares its own operator-(int) const.
struct TinyConstIter {
    int* p;
    int operator*() const { return *p; }
    TinyConstIter operator-(int n) const { TinyConstIter r; r.p = p - n; return r; }
    int operator-(const TinyConstIter& o) const { return (int)(p - o.p); }
};
struct TinyIter : TinyConstIter {
    using TinyConstIter::operator-;
    TinyIter operator-(int n) const { TinyIter r; r.p = p - n; return r; }
};
// T127 r2: ref-qualified base operator- twins re-exposed by a using-declaration; a derived
// argument needs the derived-to-base conversion, so the call binds through a wrapper.
struct TinyRefBase {
    int* p;
    TinyRefBase operator-(int n) const { return *this; }
    int operator-(const TinyRefBase& o) & { return 77; }
    int operator-(const TinyRefBase& o) && { return 78; }
};
struct TinyRefIter : TinyRefBase {
    using TinyRefBase::operator-;
    TinyRefIter operator-(int n) const { return *this; }
};
inline TinyRefIter tinyRefIter() { return TinyRefIter(); }
struct TinyCRefBase {
    int* p;
    TinyCRefBase operator-(int n) const { return *this; }
    int operator-(const TinyCRefBase& o) const & { return 73; }
    int operator-(const TinyCRefBase& o) && { return 74; }
};
struct TinyCRefIter : TinyCRefBase {
    using TinyCRefBase::operator-;
    TinyCRefIter operator-(int n) const { return *this; }
};
inline TinyCRefIter tinyCRefIter() { return TinyCRefIter(); }
// T127 r3: ref-qualified compound assignment twins; the chosen member records itself.
inline int tinyRefCompoundLast = 0;
struct TinyRefCompound {
    int v;
    void operator-=(int) & { tinyRefCompoundLast = 3; }
    void operator-=(int) && { tinyRefCompoundLast = 4; }
    void operator+=(int) & { tinyRefCompoundLast = 5; }
    void operator+=(int) && { tinyRefCompoundLast = 6; }
};
inline TinyRefCompound tinyRefCompound() { return TinyRefCompound(); }
struct TinyIterVec {
    int d[5] = {1, 2, 3, 4, 5};
    TinyIter begin() { TinyIter r; r.p = d; return r; }
    TinyIter end() { TinyIter r; r.p = d + 5; return r; }
};

// T221: using-declarations expose base overloads while preserving C++ name lookup.
struct T221OpBase {
    long operator-(long rhs) const { return 1000 + (int)rhs; }
};
struct T221OpUsingOnly : T221OpBase {
    using T221OpBase::operator-;
};
struct T221OpUsingPlus : T221OpBase {
    using T221OpBase::operator-;
    long operator-(int rhs) const { return 2000 + rhs; }
};
struct T221NamedBase {
    long get(long rhs) const { return 3000 + (int)rhs; }
};
struct T221NamedUsingOnly : T221NamedBase {
    using T221NamedBase::get;
};
template<class T> struct T221TemplBase {
    long operator-(long rhs) const { return 4000 + (int)rhs; }
};
struct T221TemplUsing : T221TemplBase<int> {
    using T221TemplBase<int>::operator-;
};
struct T221EqBase {
    int value = 0;
    bool operator==(const T221EqBase& rhs) const { return value == rhs.value; }
};
struct T221EqUsing : T221EqBase {
    using T221EqBase::operator==;
};
struct T221PrivateUsing : private T221OpBase {
public:
    using T221OpBase::operator-;
};
// Only the overload a using-declaration names is reachable; the other base's overload stays hidden.
struct T221NamedBaseInt {
    long get(int rhs) const { return 3500 + (int)rhs; }
};
struct T221NamedMulti : T221NamedBase, T221NamedBaseInt {
    using T221NamedBase::get;
};
struct T221OpBaseInt {
    long operator-(int rhs) const { return 1100 + rhs; }
};
struct T221OpMulti : T221OpBase, T221OpBaseInt {
    using T221OpBase::operator-;
};
struct T221PrivateNamedUsing : T221NamedBase {
private:
    using T221NamedBase::get;
};
struct T221StaticBaseLong {
    static long s(long rhs) { return 5000 + (int)rhs; }
};
struct T221StaticBaseInt {
    static long s(int rhs) { return 5100 + rhs; }
};
struct T221StaticMulti : T221StaticBaseLong, T221StaticBaseInt {
    using T221StaticBaseLong::s;
};
struct T221StaticPrivate : T221StaticBaseLong {
private:
    using T221StaticBaseLong::s;
};
}
