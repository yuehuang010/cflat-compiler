#pragma once

void c_p2i_take_int(int* value);
void c_p2i_take_long(long* value);
void c_p2i_take_ulong(unsigned long* value);
static inline int add3(int x) { return x + 3; }
inline int add4(int x) { return x + 4; }
#define ADD5(x) (add3(x) + 2)
/* A plain inline the program never names: only a static inline body takes its address, so the
   body is reached only transitively and must still bind the real symbol (C11 6.5.9). */
typedef int (*cb_int_fn)(int);
inline int add6(int x) { return x + 6; }
static inline cb_int_fn c_inline_sget_add6(void) { return add6; }

/* Plain C bitfield layouts checked by test_c_interop.cb. */
struct CBF_Zero
{
    char c;
    unsigned int : 0;
    char d;
};

struct CBF_Nested
{
    char c;
    struct { char d; int e : 5; } in;
    short f;
};

struct CBF_Mixed
{
    unsigned int a : 3;
    unsigned int : 0;
    unsigned int b : 4;
    char c;
    unsigned long long d : 40;
};

struct CBF_CharShort
{
    char c;
    unsigned int a : 3;
    short s;
    unsigned int b : 4;
};

/* #pragma pack records: clang lays these out with MaxFieldAlignmentAttr, not PackedAttr. */
#pragma pack(push, 1)
struct CBF_Pack1
{
    char c;
    unsigned int a : 3;
    unsigned int b : 5;
    int n;
};
#pragma pack(pop)

#pragma pack(push, 2)
struct CBF_Pack2
{
    char c;
    int n;
    short s;
};
#pragma pack(pop)

/* Flexible and zero-length tails are typed views at a zero-storage field offset. */
struct CBF_FlexInt { int count; int data[]; };
struct CBF_ZeroInt { int count; int data[0]; };
struct CBF_FlexByte { unsigned char count; unsigned char data[]; };
struct CBF_FlexItemElement { int value; };
struct CBF_FlexItem { int count; struct CBF_FlexItemElement data[]; };
struct CBF_ZeroItem { int count; struct CBF_FlexItemElement data[0]; };
struct CBF_FlexPointer { int count; int* data[]; };
struct CBF_ZeroPointer { int count; int* data[0]; };
struct CBF_AnonFlex { int n; struct { int k; int data[]; }; };
struct CBF_AnonUnionFlex { int n; union { struct { int k; int data[]; }; int other; }; };
struct CBF_FlexNested { int tag; struct { unsigned char count; int data[]; } nested; };
#pragma pack(push, 1)
struct CBF_FlexPacked { unsigned char count; unsigned char data[0]; };
#pragma pack(pop)

int cbf_pack1_size(void);
int cbf_pack1_align(void);
int cbf_pack1_n_offset(void);
int cbf_pack1_check(struct CBF_Pack1* v);
int cbf_pack2_size(void);
int cbf_pack2_align(void);
int cbf_pack2_n_offset(void);
int cbf_pack2_check(struct CBF_Pack2* v);
int cbf_zero_size(void);
struct CBF_Zero cbf_zero_make(int c, int d);
int cbf_zero_check(struct CBF_Zero v);
int cbf_nested_size(void);
struct CBF_Nested cbf_nested_make(int c, int d, int e, int f);
int cbf_nested_check(struct CBF_Nested v);
int cbf_mixed_size(void);
struct CBF_Mixed cbf_mixed_make(unsigned a, unsigned b, int c,
                                unsigned long long d);
int cbf_mixed_check(struct CBF_Mixed v);
int cbf_char_short_size(void);
struct CBF_CharShort cbf_char_short_make(int c, unsigned a, int s, unsigned b);
int cbf_char_short_check(struct CBF_CharShort v);
int cbf_flex_int_size(void);
int cbf_flex_int_offset(void);
int cbf_flex_zero_size(void);
int cbf_flex_byte_size(void);
int cbf_flex_packed_size(void);
int cbf_flex_packed_offset(void);
int cbf_flex_int_read(struct CBF_FlexInt* v, int i);
void cbf_flex_int_write(struct CBF_FlexInt* v, int i, int n);
int cbf_flex_byte_read(struct CBF_FlexPacked* v, int i);
void cbf_flex_byte_write(struct CBF_FlexPacked* v, int i, unsigned char n);
int cbf_flex_byte_tail_read(struct CBF_FlexByte* v, int i);
void cbf_flex_byte_tail_write(struct CBF_FlexByte* v, int i, unsigned char n);
int cbf_flex_item_read(struct CBF_FlexItem* v, int i);
void cbf_flex_item_write(struct CBF_FlexItem* v, int i, int n);
int cbf_flex_zero_item_read(struct CBF_ZeroItem* v, int i);
void cbf_flex_zero_item_write(struct CBF_ZeroItem* v, int i, int n);
int cbf_flex_ptr_read(struct CBF_FlexPointer* v, int i);
int cbf_flex_zero_ptr_read(struct CBF_ZeroPointer* v, int i);

// Function-like macros exercised by test_c_function_macros.cb. The compiler
// translates each into an auto generic function and rejects bodies that use
// calls, strings, member access, or other unsupported tokens (the last three
// here intentionally exercise the reject path).

#define CB_MIN(a,b)   ((a) < (b) ? (a) : (b))
#define CB_MAX(a,b)   ((a) > (b) ? (a) : (b))
#define CB_KB(n)      ((n) * 1024)
#define CB_LOWORD(x)  ((x) & 0xFFFF)
#define CB_HIWORD(x)  (((x) >> 16) & 0xFFFF)
#define CB_ABS(v)     ((v) < 0 ? -(v) : (v))
#define CB_IS_ODD(n)  ((n) & 1)
#define CB_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

// Richer bodies now supported by the translator:
//   - decimal float literals (int * 1.5 widens to double)
//   - char literals (with the same C escape forms cflat's lexer accepts)
//   - calls into an already-known C function (c_add, auto-externed from cinterop.c)
#define CB_SCALE(x)      ((x) * 1.5)
#define CB_IS_DIGIT(c)   ((c) >= '0' && (c) <= '9')
#define CB_CALL_ADD(a,b) (c_add(a, b))

/* Cast-heavy Windows macro shapes. Typedef spellings must resolve through the importing header. */
typedef unsigned short CB_WORD;
typedef unsigned long CB_DWORD_PTR;
typedef int CB_HRESULT;
typedef char* CB_LPSTR;
#define CB_CAST_LOWORD(x) ((CB_WORD)(((CB_DWORD_PTR)(x)) & 0xffff))
#define CB_CAST_FAILED(hr) (((CB_HRESULT)(hr)) < 0)
#define CB_CAST_SUCCEEDED(hr) (((CB_HRESULT)(hr)) >= 0)
#define CB_CAST_HRESULT_FROM_WIN32(x) ((CB_HRESULT)(x) <= 0 ? ((CB_HRESULT)(x)) : ((CB_HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000)))
#define CB_CAST_MAKEINTRESOURCEA(i) ((CB_LPSTR)((CB_DWORD_PTR)((CB_WORD)(i))))

/* Pointer-sentinel macro. Pass B's __typeof__ probe recovers the type as
   CB_HANDLE -> void*, so the macro registers as a void* global rather than
   an i64 integer constant. The cflat side compares it directly against a
   C-returned pointer of the same bit pattern (see c_get_invalid_handle in
   cinterop.c). */
typedef long long CB_LONG_PTR;
typedef void* CB_HANDLE;
#define CB_INVALID_HANDLE ((CB_HANDLE)(CB_LONG_PTR)-1)

/* Function-pointer-sentinel macro, mirroring sqlite3's
   SQLITE_TRANSIENT == ((sqlite3_destructor_type)-1). The macro's natural type is a
   C function pointer, so it registers as the THIN `function<...>` (a bare C fn ptr).
   Regression: the global's declared type and its inttoptr initializer must BOTH be the
   thin fn-ptr. A path that left the type unmarked picked the fat closure {ptr,ptr} for
   the global while the initializer stayed a thin ptr, failing LLVM module verification
   ("Global variable initializer type does not match global variable type"). */
typedef void (*CB_DTOR_FN)(void*);
#define CB_TRANSIENT_DTOR ((CB_DTOR_FN)-1)

/* Each of the following must still be silently rejected (dropped) by the
   translator - if any were accepted, its generated source would fail to compile,
   so their presence here exercises the drop path: */
#define CB_REJECT_CALL(x)   (some_func(x))      /* call into an UNKNOWN function */
#define CB_REJECT_STRING(x) (x ? "y" : "n")     /* string literal */
#define CB_REJECT_MEMBER(p) ((p)->field)        /* member access */

/* Alias macros: an object-like macro whose ENTIRE body is a single identifier
   (`#define A B`). A has no parameter list, so the function-like translator never
   sees it, and its body folds to no constant, so the object-like probe dropped it -
   the name simply did not exist in CFlat. Each must now bind onto whatever B names:
   a function, extern data, another alias, or a function-like macro. An unknown
   target must stay silently unimported (no diagnostic). See Section U. */
int c_triple(int x);                  /* defined in cinterop.c */
#define CB_ALIAS_ADD     c_add        /* -> C function */
#define CB_ALIAS_CHAIN   CB_ALIAS_ADD /* -> alias -> C function */
#define CB_ALIAS_EARLY   c_quad       /* alias precedes its target's declaration */
int c_quad(int x);                    /* defined in cinterop.c */
#define CB_ALIAS_COUNTER c_global_counter  /* -> extern data (shares storage) */
#define CB_ALIAS_MIN     CB_MIN       /* -> function-like macro template */
#define CB_ALIAS_FUNC_TARGET c_triple  /* object alias used inside a function macro */
#define CB_ALIAS_FUNC_CALL(x) CB_ALIAS_FUNC_TARGET(x)
#define CB_ALIAS_FUNC_MID CB_MIN
#define CB_ALIAS_FUNC_OUT CB_ALIAS_FUNC_MID
#define CB_ALIAS_FUNC_CHAIN(x,y) CB_ALIAS_FUNC_OUT(x,y)
#define CB_PARAM_TARGET c_global_counter
#define CB_PARAM_CONST c_global_counter
#define CB_PARAM_COLLISION(CB_PARAM_TARGET) CB_PARAM_TARGET + CB_PARAM_CONST
#define CB_ALIAS_TRAIL   c_triple     /* trailing comment must not defeat the match */
#define CB_ALIAS_UNKNOWN cb_no_such_name_anywhere  /* unknown: dropped, no error */
#define CB_ALIAS_CFLAT_TARGET CB_CFlatAliasTarget  /* must not bind to imported CFlat */
#define CB_BASE_VAL      100
#define CB_ALIAS_CONST   CB_BASE_VAL  /* folds to a constant: unchanged accept-set */
int c_alias_victim(int x);            /* defined in cinterop.c; called ONLY by leg 249 */
int c_negate(int x);                  /* defined in cinterop.c */
/* Colliding name whose target has the SAME arity, so an alias that did not lose to the
   real declaration would be a live overload candidate for the call in leg 249. */
#define c_alias_victim   c_negate
