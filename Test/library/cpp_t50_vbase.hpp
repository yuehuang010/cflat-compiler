#pragma once
// Derived-to-VIRTUAL-base conversions (T50), user-header shape. The base sits where the most
// derived object put it, so only clang's conversion (or a complete object) finds it.
namespace t50v {
struct V { int value = 731; int vget() const { return value; } };
struct L : virtual V { int l = 11; };
struct R : virtual V { int r = 22; };
struct D : L, R { int d = 33; };
// Non-virtual edge over a virtual one: M -> L is plain, L -> V is virtual.
struct M : L { int m = 44; };
struct P : virtual V { int p = 55; virtual ~P() {} virtual int pv() const { return p; } };
inline D object;
inline M mobject;
inline P pobject;
inline L* left_ptr() { return &object; }
inline R* right_ptr() { return &object; }
inline D* d_ptr() { return &object; }
inline M* m_ptr() { return &mobject; }
inline P* p_ptr() { return &pobject; }
inline L* null_left() { return nullptr; }
inline L& left_ref() { return object; }
inline D& d_ref() { return object; }
struct Sink {
    int pointer(const V* v) const { return v ? v->value : -1; }
    int ref(const V& v) const { return v.value; }
    int byval(V v) const { return v.value + 1; }
    int pick(const V*) const { return 1; }
    int pick(const void*) const { return 2; }
    int left(const L* l) const { return l->l; }
};
inline int free_pointer(const V* v) { return v ? v->value : -1; }
struct Holder { int saved = 0; Holder(const V* v) : saved(v ? v->value : -1) {} };
struct RefHolder { int saved = 0; RefHolder(const V& v) : saved(v.value) {} };
// Ambiguous bases: clang refuses the conversion (Test/errors).
struct A { int a = 5; };
struct B1 : A { int b1 = 6; };
struct B2 : A { int b2 = 7; };
struct DD : B1, B2 { int dd = 8; };
inline DD ddobj;
inline DD* dd_ptr() { return &ddobj; }
inline int read_a(const A* p) { return p ? p->a : -1; }
// A plain base still outranks void*: [over.ics.rank] 4.4.
struct NB { int b = 1; };
struct ND : NB { int d = 2; };
inline ND ndobj;
inline ND* nd_ptr() { return &ndobj; }
struct NSink { int pick(const NB*) const { return 1; } int pick(const void*) const { return 2; } };
}
