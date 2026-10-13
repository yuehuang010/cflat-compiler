#include <stdarg.h>

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
