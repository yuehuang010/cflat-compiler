// C++ fixture for return elision (RETELIDE): a counted value class returned by value, a branch
// between two named locals, a by-value parameter moved into a local, and a throw between the
// local's construction and its return. Header-only; counts are read back by test_cpp_interop_bridge.cb.
// Oracle: the same shapes in plain C++ under clang++ -std=c++20 (counts pinned by the legs).
namespace retelide {
struct RS_Stats {
    inline static int ctor = 0;
    inline static int copy = 0;
    inline static int move = 0;
    inline static int dtor = 0;
    inline static int bad = 0;   // touches of a destroyed or never-built RS (magic poisoned by ~RS)
    static void reset() { ctor = copy = move = dtor = bad = 0; }
};

struct RS {
    int value;
    unsigned magic = 0xBEEF;
    RS() : value(0) { ++RS_Stats::ctor; }
    RS(int x) : value(x) { ++RS_Stats::ctor; }
    RS(const RS& other) : value(other.value) { if (other.magic != 0xBEEF) ++RS_Stats::bad; ++RS_Stats::copy; }
    RS(RS&& other) : value(other.value) { if (other.magic != 0xBEEF) ++RS_Stats::bad; ++RS_Stats::move; }
    ~RS() { if (magic != 0xBEEF) ++RS_Stats::bad; magic = 0xDEAD; ++RS_Stats::dtor; }
};

struct PV { int value; PV(int x) : value(x) {} ~PV() {} private: PV(const PV&); PV(PV&&); int get() const { return value; } };
struct PM { int value; PM(int x) : value(x) {} PM(const PM&) = delete; ~PM() {} private: PM(PM&&); };

inline void boom(int t) { if (t) throw 1; }

typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
}
