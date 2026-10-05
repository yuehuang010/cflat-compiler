#pragma once
// Shapes of the std stream / regex surface (T26): an unscoped enumerator reached through its
// parent namespace or class (regex_constants::format_first_only, ios_base::beg), a base-class
// reference parameter reached through a VIRTUAL base (basic_ios::copyfmt from an ostringstream),
// and member calls on a non-const extern namespace-scope object (std::cout.flush()).
// The presumed file name below sits outside this import's in-scope directory, so nothing here is
// harvested at import: each name is bound on demand, exactly as a system header's is.
#line 9 "t26_system/ios_shapes.hpp"
namespace t26 {
namespace regex_constants {
// match_invalid makes the backing `int` on every ABI, as MSVC's is: an int-backed unscoped enum
// argument must still reach clang as the enum, never as `int`.
enum match_flag_type { match_invalid = -1, match_default = 0, format_no_copy = 1 << 9, format_first_only = 1 << 10 };
}
inline int flag_bits(regex_constants::match_flag_type f) { return static_cast<int>(f); }
struct ios_base {
    enum seekdir { beg, cur, end };
    int width_ = 0;
    int width() const { return width_; }
    void width(int w) { width_ = w; }
};
template <class C> struct basic_ios : ios_base {
    int prec_ = 6;
    C fill_ = ' ';
    basic_ios& copyfmt(const basic_ios& rhs) { prec_ = rhs.prec_; fill_ = rhs.fill_; this->width_ = rhs.width_; return *this; }
    int precision() const { return prec_; }
    void precision(int p) { prec_ = p; }
    C fill() const { return fill_; }
    void fill(C c) { fill_ = c; }
};
template <class C> struct basic_ostream : virtual basic_ios<C> {
    int written = 0;
    virtual ~basic_ostream() {}
};
template <class C> struct basic_ostringstream : basic_ostream<C> {
    int buf[4] = {0, 0, 0, 0};
};
using ios = basic_ios<char>;
using ostringstream = basic_ostringstream<char>;
inline long seek_to(long off, ios_base::seekdir d) { return d == ios_base::beg ? off : (d == ios_base::cur ? off + 100 : -1); }
struct Out {
    int flushes = 0;
    int buf = 0;
    Out& flush() { ++flushes; return *this; }
    int rdbuf() const { return buf; }
    int rdbuf(int b) { int old = buf; buf = b; return old; }
};
extern Out out;
namespace inner { struct Probe { int value = 73; int get() const { return value; } }; extern Probe probe; }
}
