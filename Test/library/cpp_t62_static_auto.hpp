#pragma once
#include <memory>
#include <string>
#include <vector>

namespace r62 {
inline int made = 0, copies = 0, moves = 0, dead = 0;
struct Trk {
    int v; Trk* self;
    Trk(int x) : v(x), self(this) { made++; }
    Trk(const Trk& o) : v(o.v), self(this) { copies++; }
    Trk(Trk&& o) : v(o.v), self(this) { moves++; o.v = -1; }
    ~Trk() { dead++; }
    int ok() const { return self == this ? 1 : 0; }
    int get() const { return v; }
};
inline Trk make(int x) { return Trk(x); }
inline void reset() { made = copies = moves = dead = 0; }
}
namespace cpp_t62 {
inline std::string global_string = "global";
inline const std::string& global_string_ref() { return global_string; }
inline void set_global_string(const char* value) { global_string = value; }
inline std::string make_string(int n) { return n == 9 ? "first" : "later"; }
inline std::vector<int> make_vector(int n) { return {n}; }
inline std::unique_ptr<int> make_unique(int n) { return std::make_unique<int>(n); }
}
