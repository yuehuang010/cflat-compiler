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

int ml_mode_value(ML_Mode m) { return (int)m; }

ML_Mode ml_pick_mode(int pick)
{
    if (pick <= 1) return ML_FAST;
    if (pick == 2) return ML_SLOW;
    return ML_AUTO;
}

int ml_apply(ML_BinaryOp op, int a, int b) { return op(a, b); }

static int ml_op_add(int a, int b) { return a + b; }
static int ml_op_mul(int a, int b) { return a * b; }
static int ml_op_sub(int a, int b) { return a - b; }

ML_BinaryOp ml_pick_op(int which)
{
    if (which == 0) return ml_op_add;
    if (which == 1) return ml_op_mul;
    return ml_op_sub;
}
