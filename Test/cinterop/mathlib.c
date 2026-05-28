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

void ml_pun_write_int(union ML_IntFloat* u, int v)
{
    u->as_int = v;
}

float ml_pun_read_float(union ML_IntFloat* u)
{
    return u->as_float;
}

int ml_pun_int_bits_of(float f)
{
    union ML_IntFloat u;
    u.as_float = f;
    return u.as_int;
}

#include <stdlib.h>

int*** ml_make_ppp(int v)
{
    int*   leaf  = (int*)malloc(sizeof(int));
    *leaf = v;
    int**  pp    = (int**)malloc(sizeof(int*));
    *pp = leaf;
    int*** ppp   = (int***)malloc(sizeof(int**));
    *ppp = pp;
    return ppp;
}
int  ml_ppp_load(int*** p)            { return ***p; }
void ml_ppp_set (int*** p, int v)     { ***p = v; }
void ml_ppp_free(int*** p)
{
    free(**p);
    free(*p);
    free(p);
}

_Bool       ml_bool_not   (_Bool x)              { return !x; }
_Bool       ml_bool_and   (_Bool a, _Bool b)     { return a && b; }
void        ml_bool_store (_Bool* out, _Bool v)  { *out = v; }
long double ml_ld_identity(long double x)        { return x; }
long double ml_ld_add     (long double a, long double b) { return a + b; }
void        ml_ld_store   (long double* out, long double v) { *out = v; }

int   ml_overlap_read_int   (struct ML_Overlap* o) { return o->as_int; }
float ml_overlap_read_float (struct ML_Overlap* o) { return o->as_float; }
int   ml_overlap_header_of  (struct ML_Overlap* o) { return o->header; }
int   ml_overlap_trailer_of (struct ML_Overlap* o) { return o->trailer; }

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
