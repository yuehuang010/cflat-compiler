/* Real-C fixture for the struct-by-value ABI test. Compiled by clang-cl on import;
   the object is linked by lld-link. The CFlat side auto-registers each struct as a
   CFlat type and calls these functions across the C ABI - exercising the Win64/Win32
   coerce-to-int (sizes 1/2/4/8), byval pointer (other sizes), and sret return paths. */

struct Byte1 { char b; };                          /* 1 byte  -> CoerceToInt(i8)  */
struct Byte4 { int x; };                           /* 4 bytes -> CoerceToInt(i32) */
struct Pair  { int x; int y; };                    /* 8 bytes -> CoerceToInt(i64) */
struct Med   { int a; int b; int c; };             /* 12 bytes -> ByVal/SRet      */
struct Big   { double a; double b; double c; };    /* 24 bytes -> ByVal/SRet      */

/* Constructors (struct-by-value return) */
struct Byte1 make_byte1(char b) { struct Byte1 r; r.b = b; return r; }
struct Byte4 make_byte4(int x)  { struct Byte4 r; r.x = x; return r; }
struct Pair  make_pair (int x, int y) { struct Pair  r; r.x = x; r.y = y; return r; }
struct Med   make_med  (int a, int b, int c) { struct Med r; r.a=a; r.b=b; r.c=c; return r; }
struct Big   make_big  (double a, double b, double c) { struct Big r; r.a=a; r.b=b; r.c=c; return r; }

/* Consumers (struct-by-value parameter) */
int    sum_byte1(struct Byte1 r) { return (int)r.b; }
int    sum_byte4(struct Byte4 r) { return r.x; }
int    sum_pair (struct Pair  r) { return r.x + r.y; }
int    sum_med  (struct Med   r) { return r.a + r.b + r.c; }
double sum_big  (struct Big   r) { return r.a + r.b + r.c; }

/* Mixed: struct param plus regular scalar args (exercise non-first slot too). */
int scaled_pair(int factor, struct Pair r, int bias)
{
    return factor * (r.x + r.y) + bias;
}

/* Round-trip: struct in, struct out, with a mutation. */
struct Big bump_big(struct Big r, double delta)
{
    r.a += delta;
    r.b += delta;
    r.c += delta;
    return r;
}

/* --- Odd sizes that exercise the non-power-of-2 ByVal path on both platforms. --- */
struct Two   { short a; short b; };                              /* 4 bytes -> CoerceToInt(i32) but built from shorts (field alignment matters) */
struct Tri   { char a; char b; char c; };                        /* 3 bytes -> ByVal/SRet on both (not in {1,2,4,8}) */
struct Sev   { char a, b, c, d, e, f, g; };                      /* 7 bytes -> ByVal/SRet on both */

struct Two make_two(short a, short b) { struct Two r; r.a=a; r.b=b; return r; }
int sum_two(struct Two r) { return (int)r.a + (int)r.b; }

struct Tri make_tri(char a, char b, char c) { struct Tri r; r.a=a; r.b=b; r.c=c; return r; }
int sum_tri(struct Tri r) { return (int)r.a + (int)r.b + (int)r.c; }

struct Sev make_sev(char a, char b, char c, char d, char e, char f, char g)
{ struct Sev r; r.a=a; r.b=b; r.c=c; r.d=d; r.e=e; r.f=f; r.g=g; return r; }
int sum_sev(struct Sev r) { return (int)r.a + (int)r.b + (int)r.c + (int)r.d + (int)r.e + (int)r.f + (int)r.g; }

/* --- Single-float / single-double structs: MSVC x64 passes structs of size {1,2,4,8}
   in a GPR even when the only field is a float/double (no HFA on Windows). The CFlat
   recipe must use CoerceToInt(iN), not an XMM path. --- */
struct Flt { float  f; };  /* 4 bytes  -> CoerceToInt(i32) */
struct Dbl { double d; };  /* 8 bytes  -> CoerceToInt(i64) */

struct Flt make_flt(float  f) { struct Flt r; r.f = f; return r; }
struct Dbl make_dbl(double d) { struct Dbl r; r.d = d; return r; }
float  get_flt(struct Flt r) { return r.f; }
double get_dbl(struct Dbl r) { return r.d; }

/* --- Mixed-type struct: int + double with 4 bytes of padding in between.
   sizeof == 16 on both platforms (4 + 4 padding + 8). ByVal/SRet on both. --- */
struct Mix { int i; double d; };

struct Mix make_mix(int i, double d) { struct Mix r; r.i = i; r.d = d; return r; }
double mix_combine(struct Mix r) { return (double)r.i + r.d; }

/* --- Nested struct field. Pair must be registered before Nested (RegisterCRecords
   walks records in source order; the body of Nested references Pair as a value field). --- */
struct Nested { struct Pair p; int extra; };  /* 12 bytes -> ByVal/SRet on both */

struct Nested make_nested(int x, int y, int extra)
{ struct Nested r; r.p.x = x; r.p.y = y; r.extra = extra; return r; }
int nested_sum(struct Nested r) { return r.p.x + r.p.y + r.extra; }

/* --- Struct containing a pointer field. On Win64: sizeof==16 (ByVal). On Win32:
   sizeof==8 (CoerceToInt(i64)). --- */
struct PtrFld { int* p; int n; };

struct PtrFld make_ptrfld(int* p, int n) { struct PtrFld r; r.p = p; r.n = n; return r; }
int ptrfld_load(struct PtrFld r) { return r.p ? *(r.p) + r.n : -1; }

/* --- Register exhaustion: six struct-by-value params (each 8 bytes -> CoerceToInt
   on Win64). Win64 has 4 GPRs for args (RCX/RDX/R8/R9); the remaining 2 go on the
   stack. Validates the recipe + call-site lowering past the register window. --- */
int sum_six_pairs(struct Pair a, struct Pair b, struct Pair c,
                  struct Pair d, struct Pair e, struct Pair f)
{
    return (a.x + a.y) + (b.x + b.y) + (c.x + c.y) +
           (d.x + d.y) + (e.x + e.y) + (f.x + f.y);
}
