#pragma once

struct T217KM
{
    mutable int m;
    int n;
    int bump() { m += 100; return m + 1000; }
    int bump() const { m += 10; return m; }
};

constexpr T217KM t217_constexpr{1, 2};
inline const T217KM t217_const{3, 4};
inline const T217KM* t217_const_ptr() { return &t217_const; }

struct T217Inner { mutable int m; int n; };
struct T217Outer { T217Inner inner; };
inline const T217Outer t217_outer{{5, 6}};
