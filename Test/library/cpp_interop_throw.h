#pragma once
namespace cppthrow {
struct Boom { int code; };
inline int throw_if(int v) { if (v > 0) throw Boom{v}; return v; }
inline int plain(int v) noexcept { return v + 1; }
}
