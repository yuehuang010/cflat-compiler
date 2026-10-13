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
