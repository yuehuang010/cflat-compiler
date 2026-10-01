#include "c_inline_demand_a.h"
int c_inline_source(int x) { return c_inline_a(x); }
/* add4's external definition, valid C99 under clang and clang-cl: an inline definition plus an
   extern declaration in exactly one TU (c_macro_helpers.h holds the plain inline definition). */
inline int add4(int x) { return x + 4; }
extern inline int add4(int x);
int c_inline_is_add4(int (*p)(int)) { return p == add4; }
inline int add6(int x) { return x + 6; }
extern inline int add6(int x);
int c_inline_is_add6(int (*p)(int)) { return p == add6; }
int c_inline_hook(void) { return 42; }
int c_inline_gnu(int x) { return x + 3; }
