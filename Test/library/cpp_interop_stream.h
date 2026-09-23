#pragma once
// Standard C++ streams used from CFlat (Test/test_cpp_interop.cb, the standard-streams section).
// Bomb's inserter throws on demand, for the leg that unwinds through a CFlat frame holding a
// stream. fmt_len / parse are inline bodies that USE the streams: companion CodeGen must
// define the hidden libc++ inline members they reach or the final link fails.
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
namespace cppstrm {
struct Bomb { int v; };
inline std::ostream& operator<<(std::ostream& o, const Bomb& b)
{
    if (b.v > 0) throw b.v;
    return o << "B" << b.v;
}
inline int fmt_len(int v) { std::ostringstream o; o << "v=" << v; return (int)o.str().size(); }
inline int parse(const char* s) { std::istringstream in(s); int v = 0; in >> v; return v; }
// std::cout redirected into a string, so what a CFlat `std.cout << ...` wrote can be asserted.
inline std::ostringstream& capture_buffer() { static std::ostringstream b; return b; }
inline std::streambuf*& saved_cout() { static std::streambuf* s = nullptr; return s; }
inline void capture_cout() { capture_buffer().str(""); saved_cout() = std::cout.rdbuf(capture_buffer().rdbuf()); }
inline std::string end_capture() { std::cout.rdbuf(saved_cout()); return capture_buffer().str(); }
inline int remove_file(const char* path) noexcept { return std::remove(path); }
}
