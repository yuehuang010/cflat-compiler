#pragma once

namespace cppownmethod
{
    class ProtectedBase
    {
    protected:
        int prot() { return 3; }
    };
}

// T164 DTORORDER: construction/destruction ordinal log for C++ class members held by CFlat values.
// E's id is its construction ordinal (1-based), so a destruction sequence reads back as digits.
// C++ destroys members and array elements in reverse construction order; the legs assert that.
namespace dtororder {
struct Boom { int at; };
inline int& nseq() { static int v = 0; return v; }
inline unsigned& ctor_seq() { static unsigned v = 0; return v; }
inline unsigned& dtor_seq() { static unsigned v = 0; return v; }
inline int& throw_at() { static int v = -1; return v; }
inline int& first_dtor() { static int v = 0; return v; }
inline int& last_dtor() { static int v = 0; return v; }
inline int& dtor_seen() { static int v = 0; return v; }
inline int& dtor_out_of_order() { static int v = 0; return v; }
inline void reset()
{
    nseq() = 0; ctor_seq() = 0; dtor_seq() = 0; throw_at() = -1;
    first_dtor() = 0; last_dtor() = 0; dtor_seen() = 0; dtor_out_of_order() = 0;
}
inline void throw_at_ordinal(int n) { throw_at() = n; }
inline int ctor_log() { return static_cast<int>(ctor_seq() % 1000000000u); }
inline int dtor_log() { return static_cast<int>(dtor_seq() % 1000000000u); }
// Summary of the destruction order of a run of E's: first destroyed id, last destroyed id, and
// the count of destructions that did not step down by one. Reverse order gives out_of_order 0.
inline int dtor_summary() { return first_dtor() * 100 + last_dtor() + dtor_out_of_order() * 10000; }
inline void note(int k) { dtor_seq() = dtor_seq() * 10u + static_cast<unsigned>(k); }
struct E {
    int id;
    E() : id(0)
    {
        ++nseq();
        if (nseq() == throw_at()) throw Boom{nseq()};
        id = nseq();
        ctor_seq() = ctor_seq() * 10u + static_cast<unsigned>(id);
    }
    ~E()
    {
        dtor_seq() = dtor_seq() * 10u + static_cast<unsigned>(id);
        if (dtor_seen() == 0) first_dtor() = id;
        else if (id != last_dtor() - 1) dtor_out_of_order()++;
        last_dtor() = id;
        dtor_seen()++;
    }
};
inline int guard(int (*f)())
{
    try { return f(); } catch (Boom&) { return -1; }
}
}
