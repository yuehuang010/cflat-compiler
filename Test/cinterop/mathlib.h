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
 * via the enum-stub resolution pass in ExtractCHeaderMacros. Mirrors how
 * libcurl ships CURL_* flags as macros rather than enum constants. */
#define ML_PI_X100        314          /* decimal int */
#define ML_MAX_NODES      0x1000       /* hex literal -> 4096 */
#define ML_NEG_OFFSET     (-5)         /* parenthesized negative */
#define ML_COMBINED_MASK  ((1 << 0) | (1 << 2) | (1 << 4))   /* 21 */
#define ML_BIG_CONST      0x100000000LL                       /* > INT32_MAX -> i64 */

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

#endif /* MATHLIB_H */
