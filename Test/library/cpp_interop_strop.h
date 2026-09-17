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

}
