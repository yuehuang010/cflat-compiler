#pragma once

// Copy-constructor pick: CV(CV&) bumps mutCopy, CV(const CV&) bumps constCopy, v encodes the pick.
// No move constructor, so clang's implicit move copies through the const copy constructor first.
namespace cpcpick {
struct CV {
    inline static int born = 0;
    inline static int dead = 0;
    inline static int mutCopy = 0;
    inline static int constCopy = 0;
    int v = 0;
    CV(int x) : v(x) { ++born; }
    CV(CV& o) : v(o.v + 1) { ++born; ++mutCopy; }
    CV(const CV& o) : v(o.v + 2) { ++born; ++constCopy; }
    ~CV() { ++dead; }
    static void reset() { born = 0; dead = 0; mutCopy = 0; constCopy = 0; }
};
}
