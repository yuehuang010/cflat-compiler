#pragma once

namespace cpi_t35 {
template<class T> class weak_ptr;

template<class T>
class shared_ptr {
public:
    int owner = 0;
    template<class U> bool owner_before(const shared_ptr<U>& other) const {
        return owner < other.owner;
    }
    template<class U> bool owner_before(const weak_ptr<U>& other) const {
        return owner < other.owner;
    }
};

template<class T>
class weak_ptr {
public:
    int owner = 0;
    weak_ptr() = default;
    template<class U> weak_ptr(const shared_ptr<U>& source) : owner(source.owner) {}
    template<class U> bool owner_before(const shared_ptr<U>& other) const {
        return owner < other.owner;
    }
    template<class U> bool owner_before(const weak_ptr<U>& other) const {
        return owner < other.owner;
    }
};

}

namespace cpi_t35x {
template<class T> struct X { T v = 0; };
template<class T> struct Y {
    T v = 0;
    Y() = default;
    template<class U> Y(const X<U>& x) : v(x.v + 100) {}
};
inline int fp(Y<int>* p) { return p ? p->v : -1; }
inline int fp(double) { return -2; }
inline int fr(Y<int>& r) { return r.v; }
inline int fr(double) { return -3; }
inline int fc(const Y<int>& r) { return r.v; }
inline int fc(double) { return -4; }
inline int fk(Y<int>*) { return 1; }
inline int fk(const Y<int>&) { return 2; }

template<class T> struct Vec {
    using value_type = T;
    T v = 0;
    Vec() = default;
    Vec(T a) : v(a) {}
    Vec(const Vec& o) : v(o.v) {}
    ~Vec() {}
};
template<class E> struct Expr {
    using result_type = typename E::value_type;
    E e;
    int k = 0;
    operator Vec<result_type>() const { return Vec<result_type>(e.v * k); }
};
template<class E> struct ExprX {
    using result_type = typename E::value_type;
    E e;
    int k = 0;
    explicit operator Vec<result_type>() const { return Vec<result_type>(e.v * k); }
};
template<class T> Expr<Vec<T>> operator*(const Vec<T>& a, int k) {
    Expr<Vec<T>> x; x.e = a; x.k = k; return x;
}
template<class T> ExprX<Vec<T>> operator%(const Vec<T>& a, int k) {
    ExprX<Vec<T>> x; x.e = a; x.k = k; return x;
}
}
