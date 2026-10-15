#include <stdarg.h>
#include "c_variadic_fnptr.h"

// C variadic callees for the aggregate-in-`...` refusal (Test/errors/err_c_variadic_aggregate.cb).
struct T145Pair { int a; int b; };

int t145_cva_pair(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    struct T145Pair p = va_arg(ap, struct T145Pair);
    va_end(ap);
    return n + p.a * 10 + p.b;
}

int t145_cva_scalar(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int v = va_arg(ap, int);
    va_end(ap);
    return n + v;
}

int t145_cva_hand(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    struct T145Pair p = va_arg(ap, struct T145Pair);
    va_end(ap);
    return n + p.a * 10 + p.b;
}

// T170: the variadic function-pointer legs (c_variadic_fnptr.h).
int t170_sum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int t170_sumd(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    double d = va_arg(ap, double);
    va_end(ap);
    return n + (int)d;
}

int t170_sumc(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int v = va_arg(ap, int);
    va_end(ap);
    return n + v;
}

int t170_rdp(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int *p = va_arg(ap, int *);
    va_end(ap);
    return n + *p;
}

int t170_pairsum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    struct T170Pair p = va_arg(ap, struct T170Pair);
    va_end(ap);
    return n + p.a * 10 + p.b;
}

int t170_add(int a, int b)
{
    return a + b;
}

int t170_inc(int a)
{
    return a + 1;
}

T170Vp t170_getvp(void)
{
    return t170_sum;
}
