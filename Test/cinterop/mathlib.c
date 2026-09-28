#include "mathlib.h"
#include <stdarg.h>

/* Definitions for the extern globals declared in mathlib.h. */
int         ml_global_int    = 4242;
double      ml_global_double = 2.5;
const char* ml_global_name   = "mathlib-global";
/* Internal linkage: must be skipped by the global harvester. */
static int  ml_global_static = 999;

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

void ml_flags_init(struct ML_Flags* f)
{
    f->ready    = 1;
    f->mode     = 5;
    f->reserved = 12;
    f->count    = 1234567;
}
unsigned ml_flags_word(struct ML_Flags* f)
{
    unsigned* w = (unsigned*)f;
    return *w;
}
void ml_flags_set_count(struct ML_Flags* f, unsigned c) { f->count = c; }
unsigned ml_flags_get_count(struct ML_Flags* f) { return f->count; }

int ml_apply(ML_BinaryOp op, int a, int b) { return op(a, b); }
int ml_apply_const(int (* const op)(int), int value) { return op(value); }

static int ml_op_add(int a, int b) { return a + b; }
static int ml_op_mul(int a, int b) { return a * b; }
static int ml_op_sub(int a, int b) { return a - b; }

ML_BinaryOp ml_pick_op(int which)
{
    if (which == 0) return ml_op_add;
    if (which == 1) return ml_op_mul;
    return ml_op_sub;
}

int cbf_anon_union_size(void) { return (int)sizeof(struct CBF_AnonUnion); }
int cbf_anon_union_align(void) { return (int)_Alignof(struct CBF_AnonUnion); }
int cbf_anon_union_tail_offset(void)
{
    return (int)offsetof(struct CBF_AnonUnion, tail);
}
int cbf_anon_union_check(const struct CBF_AnonUnion* v)
{
    return (int)v->a * 100 + (int)v->tail;
}

int cbf_named_union_size(void) { return (int)sizeof(union CBF_NamedUnion); }
int cbf_named_union_align(void) { return (int)_Alignof(union CBF_NamedUnion); }
int cbf_named_union_check_a(const union CBF_NamedUnion* v) { return (int)v->a; }
int cbf_named_union_check_b(const union CBF_NamedUnion* v) { return (int)v->b; }
int cbf_named_union_check_c(const union CBF_NamedUnion* v)
{
    return (int)(v->c & 0xffffu);
}
int cbf_named_union_check_s(const union CBF_NamedUnion* v) { return (int)v->s; }

int cbf_enum_bits_size(void) { return (int)sizeof(struct CBF_EnumBits); }
int cbf_enum_bits_align(void) { return (int)_Alignof(struct CBF_EnumBits); }
int cbf_enum_bits_tail_offset(void)
{
    return (int)offsetof(struct CBF_EnumBits, tail);
}
int cbf_enum_bits_check(const struct CBF_EnumBits* v)
{
    return (int)v->u * 1000 + v->s * 100 + (int)v->neighbor * 10 + v->tail;
}
