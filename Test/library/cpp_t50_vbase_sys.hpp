#pragma once
#line 2 "t50s_system/vbase.hpp"
// T50 system-header twin of cpp_t50_vbase.hpp: lazily bound members spell a pointer to a class
// not yet requested as void*, so the spelled class still decides viability and the conversion.
namespace t50s {
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
// The void*-spelled class pointer also decides pointee const and constructor viability.
struct B2 { int b = 41; };
struct D2 : B2 { int d = 42; };
struct U2 { int u = 43; };
inline D2 d2object;
inline U2 u2object;
inline D2* d2_ptr() { return &d2object; }
inline const D2* cd2_ptr() { return &d2object; }
inline U2* u2_ptr() { return &u2object; }
struct Pick2 {
    int b(B2*) const { return 3; }
    int b(const B2*) const { return 4; }
    int b(void*) const { return 5; }
};
inline int fb2(B2*) { return 23; }
inline int fb2(const B2*) { return 24; }
inline int fb2(void*) { return 25; }
inline int fa2(const B2*) { return 21; }
inline int fa2(const void*) { return 22; }
struct Holder2 { int saved; Holder2(const B2*) : saved(31) {} Holder2(const void*) : saved(32) {} };
struct HolderB2 { int saved; HolderB2(const B2*) : saved(33) {} };
}
