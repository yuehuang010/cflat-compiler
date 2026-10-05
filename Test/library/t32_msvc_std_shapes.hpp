#pragma once
#include <typeinfo>
#include <cfenv>
// MSVC <sstream>/<streambuf> shape on any host: the base basic_streambuf<char> is explicitly
// instantiated, and the derived buffer is first reached as a by-value field of a stream class.
namespace t32 {
template<class T> class Sbuf {
public:
    virtual ~Sbuf() {}
    int sputc(T c) { last = c; return c + 1; }
    T last = 0;
protected:
    Sbuf() {}
};
template class Sbuf<char>;
template<class T> class Strbuf : public Sbuf<T> {
public:
    Strbuf() {}
    explicit Strbuf(int v) : x(v) {}
    int x = 0;
};
template<class T> class Sstream {
public:
    Sstream() {}
    Strbuf<T>* rdbuf() { return &buf; }
    int y = 0;
private:
    Strbuf<T> buf;
};
using sstream = Sstream<char>;
using strbuf = Strbuf<char>;
// std::any / std::function shape: a member returns `const std::type_info &`, which MSVC spells
// unqualified (type_info is a global class re-exported by `using ::type_info;`).
struct Typed {
    int k = 0;
    const std::type_info& type() const { return k == 0 ? typeid(int) : typeid(double); }
};
// UCRT <fenv.h> shape: feraiseexcept is an inline body clang marks optnone (MSVC spells it
// `#pragma optimize("", off)`) whose only effect is the FP flag a dead divide raises.
__attribute__((optnone)) __attribute__((noinline)) inline int raiseDivByZero(int which)
{
    static const struct { int flag; double num; double den; } table[] = {
        { FE_INVALID, 0.0, 0.0 }, { FE_DIVBYZERO, 1.0, 0.0 } };
    double ans = 0.0;
    (void)ans;
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
        if ((which & table[i].flag) != 0) ans = table[i].num / table[i].den;
    return 0;
}
inline void clearFpFlags() { std::feclearexcept(FE_ALL_EXCEPT); }
inline int divByZeroRaised() { return std::fetestexcept(FE_DIVBYZERO) != 0 ? 1 : 0; }
inline int invalidRaised() { return std::fetestexcept(FE_INVALID) != 0 ? 1 : 0; }
inline int feDivByZero() { return FE_DIVBYZERO; }
// MSVC <valarray> shape: member unary operator+/- beside namespace-scope binary operator
// templates whose scalar operand is the non-deduced `typename Va<T>::value_type`.
template<class T> class Va {
public:
    using value_type = T;
    Va() {}
    explicit Va(unsigned n) : size(n) {}
    Va operator+() const { return *this; }
    Va operator-() const { Va r(size); for (unsigned i = 0; i < size; ++i) r.data[i] = -data[i]; return r; }
    T& operator[](unsigned i) { return data[i]; }
    T data[4] = {};
    unsigned size = 0;
};
template<class T> Va<T> operator+(const Va<T>& l, const typename Va<T>::value_type& r)
{ Va<T> o(l.size); for (unsigned i = 0; i < l.size; ++i) o.data[i] = l.data[i] + r; return o; }
template<class T> Va<T> operator+(const Va<T>& l, const Va<T>& r)
{ Va<T> o(l.size); for (unsigned i = 0; i < l.size; ++i) o.data[i] = l.data[i] + r.data[i]; return o; }
template<class T> Va<T> operator-(const Va<T>& l, const typename Va<T>::value_type& r)
{ Va<T> o(l.size); for (unsigned i = 0; i < l.size; ++i) o.data[i] = l.data[i] - r; return o; }
template<class T> Va<T> operator-(const Va<T>& l, const Va<T>& r)
{ Va<T> o(l.size); for (unsigned i = 0; i < l.size; ++i) o.data[i] = l.data[i] - r.data[i]; return o; }
}
