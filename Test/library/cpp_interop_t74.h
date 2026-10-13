#pragma once

namespace cflat_t74 {
inline int& ctor_slot() { static int n = 0; return n; }
inline int& dtor_slot() { static int n = 0; return n; }
inline int ctor_count() { return ctor_slot(); }
inline int dtor_count() { return dtor_slot(); }
struct Track {
    int value;
    Track() : value(37) { ++ctor_slot(); }
    ~Track() { ++dtor_slot(); }
};
// Same shape as Track with its own counter: the direct-global leg needs a count no other global bumps.
inline int& probe_ctor_slot() { static int n = 0; return n; }
inline int probe_ctor_count() { return probe_ctor_slot(); }
struct Probe {
    int value;
    Probe() : value(41) { ++probe_ctor_slot(); }
};
}
