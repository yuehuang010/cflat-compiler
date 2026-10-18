/* C function-pointer typedefs for the T170 legs (Test/test_c_interop.cb, Test/errors/err_c_fnptr_variadic.cb).
 * The definitions live in c_variadic_aggregate.c, which those tests already import. */
struct T170Pair { int a; int b; };

typedef int (*T170Vp)(int n, ...);
typedef int (*T170Bin)(int, int);

int t170_sum(int n, ...);
int t170_sumd(int n, ...);
int t170_sumc(int n, ...);
int t170_rdp(int n, ...);
int t170_pairsum(int n, ...);
int t170_add(int a, int b);
int t170_inc(int a);
T170Vp t170_getvp(void);

/* Same spelling as a native CFlat struct below: the native name keeps precedence. */
typedef int (*T170Shadow)(int);
typedef int (*T170Cb)(int);

/* T311: the C null-pointer macro into function slots (Test/test_function_ptr.cb). */
#include <stddef.h>
#define T311_NULL NULL

typedef int (*T311Bin)(int, int);
struct T311Ops { T311Bin f; int k; };
