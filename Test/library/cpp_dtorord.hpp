#pragma once

namespace cppdtorord {
    inline int dtor_order = 0;
    inline int route_order = 0;
    inline int ctor_count = 0;
    inline int min_ctor_count_before_dtor = 0;
    inline int premature_dtor = 0;

    inline void reset()
    {
        dtor_order = 0;
        route_order = 0;
        ctor_count = 0;
        min_ctor_count_before_dtor = 0;
        premature_dtor = 0;
    }
    inline void require_ctor_count_before_dtor(int count) { min_ctor_count_before_dtor = count; }
    inline void record(int id) { route_order = route_order * 10 + id; }
    inline void record_ctor(int id) { ++ctor_count; record(id); }
    inline void mark(int id)
    {
        if (min_ctor_count_before_dtor > ctor_count) premature_dtor = 1;
        dtor_order = dtor_order * 10 + id;
    }
    inline int order() { return dtor_order; }
    inline int routes() { return route_order; }
    inline int destroyed_too_early() { return premature_dtor; }

    struct C {
        int v;
        C(int x) : v(x) { record_ctor(1); }
        C(const C& other) : v(other.v) { record_ctor(2); }
        ~C() { mark(v); }
        int member(const C& other) const { record(9); return v * 10 + other.v; }
        explicit operator bool() const { record(8); return v != 0; }
    };

    inline int pick(const C& a, const C& b) { record(3); return a.v * 10 + b.v; }
    inline int pick(const C& a, const C& b, const C& c)
    {
        record(4);
        return a.v * 100 + b.v * 10 + c.v;
    }
    inline C g(int x) { record(5); return C(x); }
    inline int f(const C& a, const C& b) { record(6); return a.v * 10 + b.v; }
    inline int take_ref(const C& a) { record(7); return a.v; }
    inline int take_value(C a) { record(10); return a.v; }
    template <typename T> inline int tone(T a) { record(20); return a.v; }
    inline int operator+(const C& a, const C& b) { record(11); return a.v + b.v; }
}
