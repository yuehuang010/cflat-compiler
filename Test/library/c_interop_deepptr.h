/* Pointer depth 3 in C headers: int*** params, returns and fields keep their depth. */
#ifndef C_INTEROP_DEEPPTR_H
#define C_INTEROP_DEEPPTR_H

typedef struct DpHolder { int ***slot; } DpHolder;
/* Too deep to carry: the record stays complete, opaque bytes hold the field. */
typedef struct DpDeep { int x; int ********************************w; int y; } DpDeep;
typedef struct DpDeepArr { int x; int ********************************w[2]; int y; } DpDeepArr;
typedef union DpDeepUnion { int x; int ********************************w; } DpDeepUnion;
typedef struct DpDeepFlex { int y; int ********************************w[]; } DpDeepFlex;
static int dp_cb_impl(int ********************************p) { return p == 0 ? 8 : 9; }
typedef struct DpCbField { int (*cb)(int ********************************); int y; } DpCbField;
static inline DpCbField dp_mkcb(void) { DpCbField h; h.cb = dp_cb_impl; h.y = 3; return h; }
static inline int dp_runcb(int (*f)(int ********************************)) { return f(0); }
typedef struct DpDeepAnon { int x; struct { int ********************************w; int q; }; int y; } DpDeepAnon;
typedef struct DpDeepAnonU { int x; union { int ********************************w; int q; }; int y; } DpDeepAnonU;
static inline int dp_takevp(void *p) { return p == 0 ? 1 : 2; }
static inline int dp_load(int ***p) { return ***p; }
static inline int dp_load2(int **p) { return **p; }
static inline int dp_cload(const int * const * const *p) { return ***p; }
static inline int dp_cload2(const int * const *p) { return **p; }
static inline int dp_load_holder(DpHolder h) { return ***h.slot; }
static inline int ***dp_top(void)
{
    static int v = 5;
    static int *p;
    static int **pp;
    p = &v;
    pp = &p;
    return &pp;
}

/* Depth 3 and 4 through typedefs, function pointers, out-parameters, arrays and a global. */
typedef int **DpIPP;
typedef DpIPP *DpIPPP;
typedef int (*DpFp3)(int ***);
typedef struct DpH4 { int ****f; DpIPPP t; int ***arr[2]; DpFp3 cb; int *one; void *vp; } DpH4;
extern int dp_g3_value;
extern int ***dp_g3;
/* One '*' is a recorded depth too: results, globals and fields refuse a deeper destination. */
extern int *dp_g1;
static inline int *dp_r1(int *p) { return p; }
static inline void *dp_vr1(void *p) { return p; }
static inline int dp_take2(int **p) { return **p + 2; }
static inline int dp_load4(int ****p) { return ****p; }
static inline int dp_td(DpIPPP p) { return ***p; }
static inline int dp_null3(int ***p) { return p == 0 ? 1 : 2; }
static inline int dp_cpc(int *const *const *p) { return ***p; }
static inline int dp_fpimpl(int ***p) { return ***p + 1; }
static inline DpFp3 dp_getfp(void) { return dp_fpimpl; }
static inline int dp_runfp(DpFp3 f, int ***p) { return f(p) + 10; }
static inline int dp_out3(int ****out, int ***v) { *out = v; return 0; }
static inline int dp_h4(DpH4 *h) { return ****h->f + ***h->t + ***h->arr[1] + h->cb(h->arr[0]); }
static inline int dp_args(int *argc, char ***argv)
{
    static char *v[2] = {"ab", "cde"};
    *argc = 2;
    *argv = v;
    return 0;
}

#endif
