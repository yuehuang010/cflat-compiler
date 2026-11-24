#pragma once

// RETELIDE: CFlat functions returning a C++ class by value. TV has no move constructor and a
// forwarding template constructor (v + 3), which clang's implicit move picks for an rvalue source.
// TE throws 5 from its int constructor on a negative value.
namespace retelide {
struct TV {
    inline static int born = 0;
    inline static int dead = 0;
    inline static int mutCopy = 0;
    inline static int constCopy = 0;
    inline static int tmpl = 0;
    int v = 0;
    TV(int x) : v(x) { ++born; }
    TV(TV& o) : v(o.v + 1) { ++born; ++mutCopy; }
    TV(const TV& o) : v(o.v + 2) { ++born; ++constCopy; }
    template <class U> TV(U&& o) : v(o.v + 3) { ++born; ++tmpl; }
    ~TV() { ++dead; }
    static void reset() { born = 0; dead = 0; mutCopy = 0; constCopy = 0; tmpl = 0; }
};
struct TE {
    inline static int born = 0;
    inline static int dead = 0;
    inline static int copies = 0;
    int v = 0;
    TE(int x) : v(x) { if (x < 0) throw 5; ++born; }
    TE(const TE& o) : v(o.v + 2) { ++born; ++copies; }
    ~TE() { ++dead; }
    static void reset() { born = 0; dead = 0; copies = 0; }
};
}
