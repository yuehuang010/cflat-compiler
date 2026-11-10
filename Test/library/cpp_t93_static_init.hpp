#pragma once
// T93 fixture: a C++ class whose FIRST construction throws (the counter is shared by every spelling
// so each leg arms it with reset()). A static local whose initializer throws is never complete, so
// the next entry must construct it again, as clang does ([stmt.dcl]/4).
namespace t93s {
inline int made = 0, copies = 0, moves = 0, dead = 0, throws = 0;
inline int tempMade = 0, tempDead = 0, tempSelected = 0;
inline int argMade = 0, argDead = 0, argThrows = 0;
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
struct TempArg {
    int v;
    TempArg(int x) : v(x) { argMade++; }
    ~TempArg() noexcept(false) { argDead++; if (argThrows++ == 0) throw 9; }
};
struct TempDtor {
    int v;
    TempDtor(const TempArg& a) : v(a.v) { tempMade++; tempSelected = 1; }
    TempDtor(TempArg&& a) : v(a.v) { tempMade++; tempSelected = 2; }
    ~TempDtor() { tempDead++; }
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
inline int guardedTempDtor(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
inline void resetTempDtor() {
    tempMade = tempDead = tempSelected = argMade = argDead = argThrows = 0;
}
// Round-2 fixture: ArgCount never throws; Two, ArgOnce and take() take temps by const reference.
inline int cntMade = 0, cntDead = 0, twoMade = 0, twoDead = 0, onceMade = 0, onceDead = 0;
struct ArgCount {
    int v;
    ArgCount(int x) : v(x) { cntMade++; }
    ~ArgCount() { cntDead++; }
};
struct Two {
    int v;
    Two(const TempArg& a, const ArgCount& b) : v(a.v + b.v) { twoMade++; }
    ~Two() { twoDead++; }
    int get() const { return v; }
};
struct ArgOnce {
    int v;
    ArgOnce(const ArgCount& a) : v(a.v) { onceMade++; }
    ~ArgOnce() { onceDead++; }
    int get() const { return v; }
};
inline int take(const ArgCount& a) { return a.v; }
inline void resetStaticTemps() {
    cntMade = cntDead = twoMade = twoDead = onceMade = onceDead = 0;
    argMade = argDead = argThrows = 0;
}
}
