// OSTREAM: std::stringstream reaches basic_ostream through basic_iostream, past basic_istream's
// virtual basic_ios. The helpers measure where clang puts each base subobject.
#pragma once
#include <ios>
#include <istream>
#include <ostream>
#include <sstream>

inline long long ostream_at(std::stringstream* d, std::ostream* b) { return (char*)b - (char*)d; }
inline long long ostream_istream_at(std::stringstream* d, std::istream* b) { return (char*)b - (char*)d; }
inline long long ostream_iostream_at(std::stringstream* d, std::iostream* b) { return (char*)b - (char*)d; }
inline long long ostream_ios_at(std::stringstream* d, std::ios_base* b) { return (char*)b - (char*)d; }
inline long long ostream_want_o(std::stringstream* d) { return (char*)static_cast<std::ostream*>(d) - (char*)d; }
inline long long ostream_want_i(std::stringstream* d) { return (char*)static_cast<std::istream*>(d) - (char*)d; }
inline long long ostream_want_io(std::stringstream* d) { return (char*)static_cast<std::iostream*>(d) - (char*)d; }
inline long long ostream_want_ios(std::stringstream* d) { return (char*)static_cast<std::ios_base*>(d) - (char*)d; }
inline int ostream_put_ref(std::ostream& o, int v) { o << v << ';'; return o.good() ? 1 : 0; }
inline int ostream_put_ptr(std::ostream* o, int v) { *o << v << ';'; return o->good() ? 1 : 0; }
inline int ostream_read_int(std::istream& i) { int v = 0; char c = 0; i >> v >> c; return v; }
inline int ostream_text_is(std::stringstream& s, const char* t) { return s.str() == t ? 1 : 0; }
