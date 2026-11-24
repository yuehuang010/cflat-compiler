#pragma once
// GRPLEAK: a dynamically initialized global a host template instantiation also reaches.
int grpleak_dyn_count = 0;
inline int grpleak_dyn_next() { return ++grpleak_dyn_count; }
int grpleak_dyn_value = grpleak_dyn_next();
namespace grpleak_dyn {
struct X { int v; static int sf(); static int bump(); static int g(); };
int X::sf() { return 7; }
int X::bump() { static int n = 0; return ++n; }
int X::g() { return grpleak_dyn_value * 10 + grpleak_dyn_count; }
}
