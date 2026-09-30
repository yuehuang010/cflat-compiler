#pragma once
// Global-scope C names reached through `import cpp`: typedefs of builtins and of a function
// pointer, and object-like macros beside one whose body is no expression (only its probe fails).
#include <stdint.h>
typedef unsigned int cppcg_u32;
typedef long long cppcg_i64;
typedef int (*cppcg_binop)(int, int);
#define CPPCG_TYPE_SPELLING unsigned long
#define CPPCG_FLAG 0x40
#define CPPCG_LIMIT (CPPCG_FLAG * 4 + 1)
#define CPPCG_LOWORD(x) ((x) & 0xFFFF)
inline int cppcg_twice(int x) { return 2 * x; }
#define CPPCG_TWICE cppcg_twice
inline int cppcg_apply(cppcg_binop op, int a, int b) { return op(a, b); }
