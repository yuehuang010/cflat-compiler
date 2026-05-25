#include "mathlib.h"
#include <stdarg.h>

int ml_add(int a, int b) { return a + b; }
int ml_mul(int a, int b) { return a * b; }

int ml_sum(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}
