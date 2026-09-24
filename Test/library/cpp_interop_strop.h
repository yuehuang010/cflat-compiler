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
