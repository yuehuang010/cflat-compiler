#ifndef MATHLIB_H
#define MATHLIB_H

/* A tiny prebuilt-library fixture for the `import package` binding path.
 * cflat does NOT compile this header; it AST-dumps it to extract the
 * function declarations and enum constants, then links mathlib.lib. */

int ml_add(int a, int b);
int ml_mul(int a, int b);

/* Variadic - exercises the C call-site ABI (default arg promotions). */
int ml_sum(int count, ...);

/* Enum constants are registered as bare globals (C's flat enum scope):
 * RED, GREEN, BLUE, WHITE - not Color.RED. */
enum Color
{
    RED = 1,
    GREEN,   /* 2 - auto-increment */
    BLUE,    /* 3 */
    WHITE = 100
};

/* Anonymous enum - constants are still registered as bare globals; there is
 * no type-name to bind. Exercises the EnumDecl walker's name-agnostic path. */
enum
{
    ML_ANON_LOW  = 50,
    ML_ANON_HIGH = 60
};

/* Typedef of anonymous enum - clang names the anonymous enum after the typedef
 * and lists desugaredQualType as the typedef itself (self-reference). The
 * CollectCTypedefs fallback maps ML_Mode -> "enum ML_Mode" so signatures using
 * ML_Mode resolve to int via the enum-strip path. */
typedef enum { ML_FAST = 1, ML_SLOW = 2, ML_AUTO = 3 } ML_Mode;

/* Typedef of named enum where the tag and alias differ. Exercises the
 * MapCTypeToTypeAndValue lookup of `ML_PriorityAlias` -> `enum ML_PriorityTag`
 * -> int. */
typedef enum ML_PriorityTag { ML_LOW_PRI = 10, ML_HIGH_PRI = 20 } ML_PriorityAlias;

/* Function consuming a typedef'd enum as a parameter and returning one - used
 * to verify the signature resolves end-to-end (was previously skipped with
 * "unsupported parameter type 'ML_Mode'"). */
int     ml_mode_value(ML_Mode m);
ML_Mode ml_pick_mode(int pick);

/* Gated behind a preprocessor define, to exercise the inline `define` clause:
 *   import package "mathlib.h" define "ML_EXTRA";
 * Without ML_EXTRA, EXTRA_FLAG is invisible and referencing it fails to compile.
 * It is an enum constant, so it needs no symbol from the library to link. */
#ifdef ML_EXTRA
enum Extra
{
    EXTRA_FLAG = 7
};
#endif

/* Object-like #define macros - extracted as bare global int/i64 constants
 * via the probe-stub resolution pass in ResolveCHeaderMacroValues. Mirrors how
 * libcurl ships CURL_* flags as macros rather than enum constants. */
#define ML_PI_X100        314          /* decimal int */
#define ML_MAX_NODES      0x1000       /* hex literal -> 4096 */
#define ML_NEG_OFFSET     (-5)         /* parenthesized negative */
#define ML_COMBINED_MASK  ((1 << 0) | (1 << 2) | (1 << 4))   /* 21 */
#define ML_BIG_CONST      0x100000000LL                       /* > INT32_MAX -> i64 */
#define ML_UINT_ALL       0xFFFFFFFFU                          /* unsigned int max; must stay positive (not sign-extended to -1) */
#define ML_UMAX64         (~(unsigned long long)0)             /* unsigned 64-bit max; bit-reinterp to i64(-1) */

/* Float/double macros - routed via Pass B's __typeof__ probe (type is `double`
 * for unsuffixed literals, `float` for f-suffixed) and resolved by the
 * double-fold probe. Integer fold would truncate, so type-based routing is
 * required.  Registered as CFlat `double` globals. */
#define ML_PI             3.14159265358979323846   /* double, unsuffixed */
#define ML_HALF           0.5                      /* simple round number */
#define ML_NEG_FLOAT      (-1.25)                  /* parenthesized negative */
#define ML_TWO_PI         (2.0 * 3.14159265358979323846)  /* folded expression */
#define ML_FLOAT_F        2.5f                     /* float-suffixed -> still routes to double */

/* Function-like macro: must be SKIPPED by the extractor (cflat has no
 * preprocessor). Presence here ensures it does not crash extraction or
 * leak in as a constant. */
#define ML_SQUARE(x) ((x) * (x))

/* String-valued macros - routed via Pass B's __typeof__ probe (type is
 * `char[N]`) and resolved by the string-literal probe. Registered as
 * CFlat `char*` globals interned in the string pool. */
#define ML_NAME      "mathlib"
#define ML_VERSION   "1.2.3"
#define ML_GREETING  "hello\nworld"  /* escape sequences must round-trip */

/* Gated by inline `define "ML_EXTRA"` - mirrors EXTRA_FLAG but as a #define.
 * Verifies per-import defines feed the macro extraction pass, not just the
 * enum/decl pass. */
#ifdef ML_EXTRA
#define ML_EXTRA_MACRO 77
#endif

/* Function-pointer sentinel macro - the SIG_IGN/SIG_DFL shape from <signal.h>.
 * Pass B's __typeof__ probe sees `int (*)(int, int)`; the value-fold yields 0.
 * Registered as a CFlat function<int(int,int)> global with env = intToPtr(0).
 * The sentinel value isn't meant to be invoked - only compared. */
#define ML_NULL_OP ((int (*)(int, int))0)

/* Named union - the classic int/float type-pun shape. Exercises the C-interop
 * extractor routing unions through CreateUnionType (single max-sized slot, all
 * fields share offset 0). Field access is the same `u.field` syntax as structs;
 * the IsUnion flag in the struct descriptor drives offset 0 instead of
 * FieldIndex GEPs. Nested anonymous structs/unions are NOT in scope here -
 * anonymous records have no name to bind and are skipped by RegisterCRecords. */
union ML_IntFloat
{
    int   as_int;
    float as_float;
};

/* Pass-by-pointer is the most common shape for C union APIs - exercises that
 * `union ML_IntFloat *` resolves and that GEP-to-field still hits offset 0. */
void  ml_pun_write_int   (union ML_IntFloat* u, int v);
float ml_pun_read_float  (union ML_IntFloat* u);
int   ml_pun_int_bits_of (float f);  /* helper: returns the reference bit pattern */

/* Anonymous union inside a struct (C11) - the OVERLAPPED / LARGE_INTEGER shape from
 * the Win32 SDK. The C-interop extractor synthesizes a tag (`ML_Overlap__anon0`)
 * for the inner union and rewrites the materializing unnamed field to a synthetic
 * name `__anon0`, so CFlat code reaches the inner members via `o.__anon0.as_int`.
 * Field overlap at offset 0 is preserved by the regular CreateUnionType codegen. */
struct ML_Overlap
{
    int header;
    union
    {
        int   as_int;
        float as_float;
    };
    int trailer;
};

/* `_Bool` (C99) and `long double` (MSVC: 8-byte alias of double) - the C-interop
 * mapper aliases `_Bool` -> CFlat `bool` and `long double` -> CFlat `double`.
 * These helpers lock the mappings in via pass-by-value and pointer-out shapes.
 * stdbool.h's `bool` macro expands to `_Bool`; clang canonicalizes the qualType
 * to `_Bool` in the AST dump, which is what the extractor sees. */
/* Three-level indirection - the mapper collapses `int***` to opaque `void**`
 * (the deepest two CFlat pointer levels CFlat can express). C owns the chain
 * (allocates and frees); CFlat threads the opaque handle through. The function
 * pointer is one machine word regardless of declared depth, so the call still
 * links correctly. */
int*** ml_make_ppp   (int v);            /* returns malloc'd int*** chain */
int    ml_ppp_load   (int*** p);         /* ***p */
void   ml_ppp_set    (int*** p, int v);  /* ***p = v */
void   ml_ppp_free   (int*** p);

_Bool       ml_bool_not    (_Bool x);
_Bool       ml_bool_and    (_Bool a, _Bool b);
void        ml_bool_store  (_Bool* out, _Bool v);
long double ml_ld_identity (long double x);
long double ml_ld_add      (long double a, long double b);
void        ml_ld_store    (long double* out, long double v);

int   ml_overlap_read_int    (struct ML_Overlap* o);
float ml_overlap_read_float  (struct ML_Overlap* o);
int   ml_overlap_header_of   (struct ML_Overlap* o);
int   ml_overlap_trailer_of  (struct ML_Overlap* o);

/* C bitfields - the Win32 PE/COFF flag-byte shape. CFlat replicates MSVC's
 * LSB-first packing so a struct with adjacent same-type bitfields lays out
 * the same bits in the same storage word on both sides. The reader/writer
 * helpers below cross the C/CFlat ABI boundary by-pointer so we can confirm
 * the bits agree without having to dump raw memory. */
struct ML_Flags
{
    unsigned int ready    : 1;
    unsigned int mode     : 3;
    unsigned int reserved : 4;
    unsigned int count    : 24;
};

void ml_flags_init     (struct ML_Flags* f);
unsigned ml_flags_word (struct ML_Flags* f);              /* raw 32-bit view */
void ml_flags_set_count(struct ML_Flags* f, unsigned c);
unsigned ml_flags_get_count(struct ML_Flags* f);

/* Externally-linkable global variables. The C-interop extractor binds these as
 * declaration-only, MUTABLE CFlat globals (external references resolved by the
 * linker against mathlib.lib). Scalar, double, and pointer shapes are covered.
 * `ml_global_static` is internal-linkage and must NOT be bound (it has no
 * external symbol to link against). */
extern int         ml_global_int;       /* defined = 4242 */
extern double      ml_global_double;    /* defined = 2.5  */
extern const char* ml_global_name;      /* defined = "mathlib-global" */

/* Function-pointer typedef - the comparator-style callback shape used by qsort,
 * libcurl's CURLOPT_WRITEFUNCTION, etc. clang spells the qualType as
 *   "int (*)(int, int)"
 * which the MapCTypeToTypeAndValue path detects via the "(*)" token and parses
 * into a CFlat function<int(int,int)> TypeAndValue. */
typedef int (*ML_BinaryOp)(int a, int b);

/* Function-pointer-typed parameter and return - exercises the typedef name being
 * used in C function signatures (not just as a top-level typedef). */
int         ml_apply(ML_BinaryOp op, int a, int b);
ML_BinaryOp ml_pick_op(int which);

/* Opaque forward-declared handle - declared but never defined anywhere in the
 * translation unit. This is the SDL_Window / sqlite3_stmt / CURL* idiom. The
 * C-interop extractor must register it as an opaque (pointer-only) type so CFlat
 * can NAME `ML_Opaque*`; a by-value use is rejected as an incomplete layout.
 * No library symbol is involved - it is purely a type. */
typedef struct ML_Opaque ML_Opaque;

/* The `Ref` form of the same idiom - a typedef of a POINTER to the opaque tag
 * (CoreGraphics' CGColorSpaceRef, Win32's HWND). The alias must bind as a usable
 * pointer type (ML_OpaqueRef = ML_Opaque*); it previously dropped, so the name did
 * not resolve at all and callers had to spell the handle void*. Purely a type. */
typedef struct ML_Opaque *ML_OpaqueRef;

#endif /* MATHLIB_H */
