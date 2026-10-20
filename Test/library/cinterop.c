#include <stddef.h>
#include "cinterop_global_arrays.h"
/* Real C (clang-compilable) fixture for the C-interop test.
   Compiled by clang-cl when imported from a .cb; the object is linked by lld. */

/* Externally-linkable global - the .c auto-extern path binds this as a
   declaration-only, mutable CFlat global. (c_handle_payload below is static,
   so it must NOT be bound - validates the internal-linkage skip.) */
int c_global_counter = 1000;

int t168KCArray[3] = {16, 17, 18};
T168KCArrayPair t168KCPairs[2] = {{22, 23}, {24, 25}};
int t168KCArrayRead(int index) { return t168KCArray[index]; }
int t168KCArrayAt(int values[]) { return values[1]; }
int t168KCPairSum(T168KCArrayPair values[]) { return values[1].x + values[1].y; }

/* T183: const C globals live in read-only storage; CFlat writes through them are refused. */
const int t183KCScalar = 41;
const int t183KCArray[3] = {51, 52, 53};
int t183KCMutable = 62;
int t183KCSecond = 63;
const int* t183KCPtr = &t183KCMutable;
int* const t183KCConstPtr = &t183KCMutable;
int t183KCTakeMutable(int* p) { return *p; }
int t183KCTakeConst(const int* p) { return *p; }

/* c10: const C globals, results and fields read back through CFlat. */
static const int t10KSeven = 7;
static const char* t10Names[2] = { "p0", "p1" };
const char t10KStr[6] = "hello";
const int* t10KPArr[2] = { &t183KCScalar, &t10KSeven };
struct T10RS t10RS = { 3, 4 };
struct T10FS t10FS = { { 5, 6 }, "nm", "pc", { 8, 9 } };
T10_PCSTR t10Pc = "xyz";
const char* t10Name(void) { return t10KStr; }
T10_PCSTR t10Name2(void) { return t10KStr + 1; }
const int* t10Ip(void) { return &t10KSeven; }
const char** t10Pp(void) { return t10Names; }
int t10TakeMut(char* s) { return s[0]; }
int t10TakeConst(const char* s) { return s[0]; }

int c_add(int a, int b)
{
    return a + b;
}

int c_square(int x)
{
    return x * x;
}

/* Typedef chain regression: HANDLE -> void* must be chased by the auto-extern
   path so both the return type and parameter type register without being
   skipped as "unsupported". The body returns &c_handle_payload so the cflat
   side can round-trip identity through an opaque pointer. */
typedef void* C_HANDLE;
typedef long long C_LONG_PTR;

static int c_handle_payload = 0x5A5A5A5A;

C_HANDLE c_get_handle(void)
{
    return &c_handle_payload;
}

int c_handle_load(C_HANDLE h)
{
    return *(int*)h;
}

/* Nested typedef: SCK -> C_HANDLE -> void*. Mapper must follow >1 hop. */
typedef C_HANDLE SCK;

int c_sck_load(SCK s)
{
    return *(int*)s;
}

/* Typedef'd integer alias used in the signature - mapper must resolve through
   the chain to "long long" so the parameter lowers as i64, not gets dropped. */
C_LONG_PTR c_lp_double(C_LONG_PTR x)
{
    return x + x;
}

/* Returns (C_HANDLE)(C_LONG_PTR)-1 - the same bit pattern as the sentinel
   macro CB_INVALID_HANDLE declared in c_macro_helpers.h, used by the cflat
   side to verify the macro registers as a pointer (not an i64 constant). */
C_HANDLE c_get_invalid_handle(void)
{
    return (C_HANDLE)(C_LONG_PTR)-1;
}

/* A mini COM-style vtable: a struct of function pointers plus an object whose first
   field points at it. Exercises CFlat's C function-pointer struct fields (mapped to a
   thin function<>, so they are callable) and typed struct-pointer fields (so
   obj.vtbl->fn(&obj) dispatches with no hand-written cast - the same shape a real COM
   lpVtbl has). No allocation: the caller supplies the object by value. */
struct CbObj;
typedef struct CbVtbl {
    int (*add)(struct CbObj*, int, int);
    int (*get)(struct CbObj*);
} CbVtbl;
typedef struct CbObj {
    const CbVtbl* vtbl;
    int value;
} CbObj;

static int cb_vt_add(struct CbObj* self, int a, int b) { self->value += a + b; return self->value; }
static int cb_vt_get(struct CbObj* self) { return self->value; }
static const CbVtbl cb_global_vtbl = { cb_vt_add, cb_vt_get };

void cb_init_obj(CbObj* obj, int start)
{
    obj->vtbl = &cb_global_vtbl;
    obj->value = start;
}

/* Targets for the alias-macro legs in c_macro_helpers.h (Section U of
   test_c_interop.cb). Kept trivial: the legs assert the alias reaches THIS body. */
int c_triple(int x) { return x * 3; }
int CAA_GetObjectW(int value) { return 700 + value; }
int CAA_GetObject(int value) { return 400 + value; }
int CAA_GetReverse(int value) { return 500 + value; }
int CAA_GetReverseW(int value) { return 900 + value; }
int CAA_GetGroupW(int value) { return 1100 + value; }
int CAA_GetGroup(int value) { return 800 + value; }
int CAA_GetGroupRealFirst(int value) { return 1000 + value; }
int CAA_GetGroupRealFirstW(int value) { return 1200 + value; }
int CAA_LateTarget(int value) { return 1300 + value; }
int CAA_LateChainTarget(int value) { return 1400 + value; }
int CAA_GroupLateTarget(int value) { return 1500 + value; }
int CAA_DiffTarget(double value) { return 1600 + (int)value; }
int c_quad(int x)   { return x * 4; }
int c_alias_victim(int x) { return x * 10; }
int c_negate(int x)       { return -x; }

#include "c_macro_helpers.h"

int cbf_flex_int_size(void) { return (int)sizeof(struct CBF_FlexInt); }
int cbf_flex_int_offset(void) { return (int)offsetof(struct CBF_FlexInt, data); }
int cbf_flex_zero_size(void) { return (int)sizeof(struct CBF_ZeroInt); }
int cbf_flex_byte_size(void) { return (int)sizeof(struct CBF_FlexByte); }
int cbf_flex_packed_size(void) { return (int)sizeof(struct CBF_FlexPacked); }
int cbf_flex_packed_offset(void) { return (int)offsetof(struct CBF_FlexPacked, data); }
int cbf_flex_int_read(struct CBF_FlexInt* v, int i) { return v->data[i]; }
void cbf_flex_int_write(struct CBF_FlexInt* v, int i, int n) { v->data[i] = n; }
int cbf_flex_byte_read(struct CBF_FlexPacked* v, int i) { return v->data[i]; }
void cbf_flex_byte_write(struct CBF_FlexPacked* v, int i, unsigned char n) { v->data[i] = n; }
int cbf_flex_byte_tail_read(struct CBF_FlexByte* v, int i) { return v->data[i]; }
void cbf_flex_byte_tail_write(struct CBF_FlexByte* v, int i, unsigned char n) { v->data[i] = n; }
int cbf_flex_item_read(struct CBF_FlexItem* v, int i) { return v->data[i].value; }
void cbf_flex_item_write(struct CBF_FlexItem* v, int i, int n) { v->data[i].value = n; }
int cbf_flex_zero_item_read(struct CBF_ZeroItem* v, int i) { return v->data[i].value; }
void cbf_flex_zero_item_write(struct CBF_ZeroItem* v, int i, int n) { v->data[i].value = n; }
int cbf_flex_ptr_read(struct CBF_FlexPointer* v, int i) { return *v->data[i]; }
int cbf_flex_zero_ptr_read(struct CBF_ZeroPointer* v, int i) { return *v->data[i]; }

int cbf_zero_size(void) { return (int)sizeof(struct CBF_Zero); }
struct CBF_Zero cbf_zero_make(int c, int d)
{
    struct CBF_Zero v = { (char)c, (char)d };
    return v;
}
int cbf_zero_check(struct CBF_Zero v) { return v.c * 100 + v.d; }

int cbf_nested_size(void) { return (int)sizeof(struct CBF_Nested); }
struct CBF_Nested cbf_nested_make(int c, int d, int e, int f)
{
    struct CBF_Nested v = { 0 };
    v.c = (char)c;
    v.in.d = (char)d;
    v.in.e = e;
    v.f = (short)f;
    return v;
}
int cbf_nested_check(struct CBF_Nested v)
{
    return v.c * 1000 + v.in.d * 100 + v.in.e * 10 + v.f;
}

/* Verbose C spellings: signed short int / signed long long int must bind as short / i64. */
signed long long int cbf_verbose_spellings(signed short int a, signed long long int b)
{
    return b + a;
}

int cbf_mixed_size(void) { return (int)sizeof(struct CBF_Mixed); }
struct CBF_Mixed cbf_mixed_make(unsigned a, unsigned b, int c,
                                unsigned long long d)
{
    struct CBF_Mixed v = { 0 };
    v.a = a;
    v.b = b;
    v.c = (char)c;
    v.d = d;
    return v;
}
int cbf_mixed_check(struct CBF_Mixed v)
{
    return (int)(v.a * 1000000u + v.b * 10000u + (unsigned char)v.c * 100u
                 + (unsigned)(v.d & 0xFFFFu));
}

int cbf_char_short_size(void) { return (int)sizeof(struct CBF_CharShort); }
struct CBF_CharShort cbf_char_short_make(int c, unsigned a, int s, unsigned b)
{
    struct CBF_CharShort v = { 0 };
    v.c = (char)c;
    v.a = a;
    v.s = (short)s;
    v.b = b;
    return v;
}
int cbf_char_short_check(struct CBF_CharShort v)
{
    return (unsigned char)v.c * 10000 + v.a * 1000 + v.s * 10 + v.b;
}

int cbf_pack1_size(void) { return (int)sizeof(struct CBF_Pack1); }
int cbf_pack1_align(void) { return (int)_Alignof(struct CBF_Pack1); }
int cbf_pack1_n_offset(void) { return (int)offsetof(struct CBF_Pack1, n); }
int cbf_pack1_check(struct CBF_Pack1* v) { return v->c * 1000 + v->a * 100 + v->b * 10 + v->n; }
int cbf_pack2_size(void) { return (int)sizeof(struct CBF_Pack2); }
int cbf_pack2_align(void) { return (int)_Alignof(struct CBF_Pack2); }
int cbf_pack2_n_offset(void) { return (int)offsetof(struct CBF_Pack2, n); }
int cbf_pack2_check(struct CBF_Pack2* v) { return v->c * 1000 + v->n * 10 + v->s; }

/* Sub-32-bit integer arguments / return: the caller must extend (Apple arm64; -O2 exposes it). */
long long cbf_ext_s8(signed char a, long long b) { return b + a; }
long long cbf_ext_u8(unsigned char a, long long b) { return b + a; }
long long cbf_ext_u16(unsigned short a, long long b) { return b + a; }
short cbf_ext_ret_s16(int x) { return (short)x; }
/* Struct by value (an ABI recipe with byval) next to narrow ints: the narrow ints still extend. */
struct CBF_Big3 { long long a, b, c; };
long long cbf_ext_sv(struct CBF_Big3 s, short a) { return s.a + s.b + s.c + a; }
short cbf_ext_sv_ret(struct CBF_Big3 s) { return (short)s.a; }
struct CBF_Two { long long a, b; };
long long cbf_ext_two(struct CBF_Two s, signed char c) { return s.a + s.b + c; }
/* Indirect call of a CFlat extern definition: the call carries no attributes, so the definition
   must not assume extended arguments. */
typedef long long (*cbf_ext_fp)(short);
cbf_ext_fp cbf_ext_ident(cbf_ext_fp p) { return p; }

/* Indirect calls through C function pointers: the call site must extend narrow arguments (the
   callee is clang -O2 code that trusts them). Also a pointer stored in a struct field. */
typedef long long (*cbf_ext_fpb)(unsigned char, signed char);
__attribute__((noinline)) static long long cbf_ext_innerb(unsigned char a, signed char b) { return (long long)a * 1000 + b; }
cbf_ext_fpb cbf_ext_getfb(void) { return cbf_ext_innerb; }
typedef long long (*cbf_ext_fpu8)(unsigned char);
typedef long long (*cbf_ext_fps8)(signed char);
__attribute__((noinline)) static long long cbf_ext_inner_u8(unsigned char a) { return a; }
__attribute__((noinline)) static long long cbf_ext_inner_s8(signed char a) { return a; }
cbf_ext_fpu8 cbf_ext_get_u8(void) { return cbf_ext_inner_u8; }
cbf_ext_fps8 cbf_ext_get_s8(void) { return cbf_ext_inner_s8; }
struct CBF_ExtHolder { long long (*f)(short); };
__attribute__((noinline)) static long long cbf_ext_inner16(short a) { return a; }
void cbf_ext_fill(struct CBF_ExtHolder* h) { h->f = cbf_ext_inner16; }
/* CFlat extern DEFINITIONS returning narrow ints: the definition extends the return value and
   this -O2 C caller trusts it (block-scope prototypes: not auto-registered as C declarations). */
long long cbf_ext_call_def(int v) { extern short cbf_ext_def_ret(int); volatile int vv = v; return cbf_ext_def_ret(vv); }
long long cbf_ext_call_defu(int v) { extern unsigned char cbf_ext_def_retu(int); volatile int vv = v; return cbf_ext_def_retu(vv); }
typedef short (*cbf_ext_fpr)(int);
cbf_ext_fpr cbf_ext_identr(cbf_ext_fpr p) { return p; }

/* T174: C records over-aligned by aligned(N) on the record, a member, a typedef or a union member.
   CFlat must take clang's size, alignment and field offsets (values pinned in test_c_interop.cb). */
struct __attribute__((aligned(16))) T174Al16 { int x; };
struct T174Mid { char c; int x __attribute__((aligned(8))); };
struct __attribute__((packed, aligned(8))) T174PkAl { char c; int x; };
struct T174Outer { char c; struct T174Al16 a; };
union T174UAl { char c; int x __attribute__((aligned(16))); };
struct T174Plain { int x; };
typedef struct T174Plain T174Plain16 __attribute__((aligned(16)));
struct T174PlHold { char c; T174Plain16 p; };
struct T174Sh { char c; short s __attribute__((aligned(8))); long long ll; };
struct __attribute__((aligned(32))) T174Big { long long v[4]; };
struct T174ArrHold { int tag; struct T174Al16 items[2]; };
struct T174Al16 t174AlArr[3] = {{100}, {200}, {300}};
struct T174Mid t174Mid = {'m', 55};
struct T174PkAl t174Pk = {'p', 66};
struct T174Outer t174Outer = {'o', {77}};
struct T174PlHold t174PlH = {'h', {12}};
struct T174Sh t174Sh = {'s', 33, 44};
struct T174Big t174Big = {{1, 2, 3, 4}};
struct T174ArrHold t174AH = {7, {{21}, {22}}};
struct T174Al16* t174_base(void) { return t174AlArr; }
int t174_al_x(int i) { return t174AlArr[i].x; }
struct T174Al16 t174_al_id(struct T174Al16 v) { v.x += 1; return v; }
struct T174Big t174_big_bump(struct T174Big b) { b.v[0] += 1; return b; }
long long t174_sh_ll(void) { return t174Sh.ll; }

/* c03: a C parameter keeps the const levels it spells (`const int **` is const at the int only). */
int c03KCReadMulti(const int** p) { return **p; }
int c03KCReadAll(const int* const* p) { return **p; }
/* c03: `int *const *` is const at the pointer slot only; its int stays writable. */
int c03KCSlotRead(int* const* p) { return **p; }
/* c06: a C field spelled `const char*` (directly or through a typedef) takes a string literal. */
typedef const char* C06_PCSTR;
struct C06Names { const char* direct; C06_PCSTR alias; char* mutable_name; };
int c06NamesSecond(const struct C06Names* n) { return n->direct[1] * 1000 + n->alias[1]; }
