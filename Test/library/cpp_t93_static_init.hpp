#pragma once
// T93 fixture: a C++ class whose FIRST construction throws (the counter is shared by every spelling
// so each leg arms it with reset()). A static local whose initializer throws is never complete, so
// the next entry must construct it again, as clang does ([stmt.dcl]/4).
namespace t93s {
inline int made = 0, copies = 0, moves = 0, dead = 0, throws = 0;
struct Once {
    int v;
    Once(int x) : v(x) {
        if (throws == 0) { throws = 1; throw x; }
        made++;
    }
    Once(const Once& o) : v(o.v) { copies++; }
    Once(Once&& o) : v(o.v) { moves++; o.v = -1; }
    ~Once() { dead++; }
    int get() const { return v; }
};
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
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
inline void reset() { made = copies = moves = dead = throws = 0; }
}
