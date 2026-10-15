#pragma once
#include "cpp_interop_basic.h"

namespace cppi {
struct T70Plain { int value = 0; };
inline int t70_pointer_or_const_ref(Tracked*) { return 1; }
inline int t70_pointer_or_const_ref(const Tracked&) { return 2; }
// Class with a user conversion to its own pointer: clang binds a class xvalue to C* through it.
struct T70Conv {
    int value = 31;
    operator T70Conv*() { return this; }
};
inline int t70_conv_pick(T70Conv* p) { return p->value; }
inline int t70_conv_pick(void*) { return 91; }
inline int t70_conv_only(T70Conv* p) { return p->value; }
// Review-3 shapes clang also binds to C*: an inherited operator, an operator template, and
// operator Derived*() plus Derived* -> C*. Each redirects to a static; the C* overloads return a
// tag only, since cflat binds the original object instead (conversion not invoked, pre-existing).
struct T70Inh;
struct T70InhBase { operator T70Inh*(); };
struct T70Inh : T70InhBase { int value = 31; };
inline T70InhBase::operator T70Inh*() { static T70Inh other; other.value = 73; return &other; }
inline int t70_inh_pick(T70Inh*) { return 1; }
inline int t70_inh_pick(void*) { return 2; }
inline int t70_inh_only(T70Inh*) { return 1; }
struct T70Tpl {
    int value = 31;
    template <class T> operator T*() { static T70Tpl other; other.value = 73; return &other; }
};
inline int t70_tpl_only(T70Tpl*) { return 1; }
struct T70DerivedTarget;
struct T70Der {
    int value = 31;
    operator T70DerivedTarget*();
};
struct T70DerivedTarget : T70Der {};
inline T70Der::operator T70DerivedTarget*() { static T70DerivedTarget other; other.value = 73; return &other; }
inline int t70_der_pick(T70Der*) { return 1; }
inline int t70_der_pick(void*) { return 2; }
inline int t70_der_only(T70Der*) { return 1; }
}

namespace cppi_t159 {
inline int arg_ctors = 0, arg_copies = 0, arg_moves = 0, arg_dtors = 0;
inline int mutable_r_hits = 0, const_r_hits = 0, const_l_hits = 0;
struct Arg {
    int value;
    Arg(int n) : value(n) { ++arg_ctors; }
    Arg(const Arg& a) : value(a.value) { ++arg_copies; }
    Arg(Arg&& a) : value(a.value) { ++arg_moves; }
    ~Arg() { ++arg_dtors; }
};
struct Host {
    int member(Arg a) && { ++mutable_r_hits; return 300 + a.value; }
    int member(Arg a) const && { ++const_r_hits; return 400 + a.value; }
    int member(Arg a) const & { ++const_l_hits; return 200 + a.value; }
};
inline const Host& free_host() { static const Host h{}; return h; }
struct Provider {
    const Host& host() const { static const Host h{}; return h; }
};
inline Provider provider{};
inline const Host global_host{};
struct HostArr { const Host a[2]; };
inline HostArr host_arr{};
inline const Host* host_arr_ptr() { return host_arr.a; }
struct BigHost {
    int v = 6;
    ~BigHost() {}
    int member(Arg a) && { ++mutable_r_hits; return 300 + a.value; }
    int member(Arg a) const && { ++const_r_hits; return 400 + a.value; }
    int member(Arg a) const & { ++const_l_hits; return 200 + a.value; }
};
struct BigHostArr { const BigHost a[2]; };
inline BigHostArr big_host_arr{};
inline void reset() {
    arg_ctors = arg_copies = arg_moves = arg_dtors = 0;
    mutable_r_hits = const_r_hits = const_l_hits = 0;
}
inline int ctors() { return arg_ctors; }
inline int copies() { return arg_copies; }
inline int moves() { return arg_moves; }
inline int dtors() { return arg_dtors; }
inline int mutable_r() { return mutable_r_hits; }
inline int const_r() { return const_r_hits; }
inline int const_l() { return const_l_hits; }
}
