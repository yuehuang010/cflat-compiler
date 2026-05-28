/* Comprehensive float/double #define fixture for the C-interop macro extractor.
 *
 * Coverage groups (each macro is tested from CFlat via test_c_float_macros.cb):
 *
 *   A. Plain literals - default-double, f-suffixed float, l-suffixed long double.
 *   B. Sign / parens   - leading minus, parenthesized, double-negation.
 *   C. Scientific      - mantissa+exponent, signed exponent, leading-dot, trailing-dot.
 *   D. Hex floats      - 0x1.8p3 style (mantissa . hex-frac p binary-exp).
 *   E. Arithmetic      - +, -, *, /, nested parens, precedence.
 *   F. Mixed int/float - usual-arithmetic-conversion paths inside the macro body.
 *   G. Casts           - (double)i, (float)i, (double)(int_macro).
 *   H. Magnitude       - very small (1e-300), very large (1e300).
 *   I. Macro chaining  - one float macro built from another float macro.
 *   J. Zero / negzero  - 0.0 and -0.0 (sign bit must survive the round-trip).
 *
 *   K. Negative tests  - macros that must NOT land on the float path:
 *      - FM_INT_CAST_OF_FLOAT: (int)(3.14) recovers `int` from __typeof__, so
 *        the macro registers as the integer 3, not as 3.14.
 *      - FM_STRING (existing pattern): string macro silently dropped.
 *      - FM_NONCONST: identifier reference clang cannot fold - dropped.
 */

#ifndef FLOAT_MACROS_H
#define FLOAT_MACROS_H

/* ---- A. Plain literals ------------------------------------------------ */
#define FM_DOUBLE_PLAIN   1.5            /* default double */
#define FM_FLOAT_SUFFIX   2.5f           /* float -> still routes to double */
#define FM_LONG_DOUBLE    3.5L           /* long double -> routes to double */

/* ---- B. Sign / parens ------------------------------------------------- */
#define FM_NEG            -1.25
#define FM_NEG_PAREN      (-2.75)
#define FM_DOUBLE_NEG     (-(-4.125))     /* unary - twice should fold to +4.125 */
#define FM_UNARY_PLUS     (+0.75)         /* leading + must be a no-op */

/* ---- C. Scientific notation ------------------------------------------ */
#define FM_SCI_POS        1.5e3           /* 1500.0 */
#define FM_SCI_NEG_EXP    2.5e-4          /* 0.00025 */
#define FM_SCI_LEAD_DOT   .25e2           /* 25.0 - no leading integer digit */
#define FM_SCI_TRAIL_DOT  4.e1            /* 40.0 - no fractional digits */
#define FM_SCI_CAP_E      6.25E2          /* 625.0 - capital E exponent */

/* ---- D. Hex floats ---------------------------------------------------- */
#define FM_HEX_FLOAT      0x1.8p3         /* 1.5 * 2^3 = 12.0 */
#define FM_HEX_FLOAT_NEG  -0x1.4p1        /* -2.5 */

/* ---- E. Arithmetic ---------------------------------------------------- */
#define FM_ADD            (1.25 + 0.75)               /* 2.0 */
#define FM_SUB            (10.5 - 0.5)                /* 10.0 */
#define FM_MUL            (2.5 * 4.0)                 /* 10.0 */
#define FM_DIV            (1.0 / 4.0)                 /* 0.25 */
#define FM_NESTED         ((1.0 + 2.0) * (3.0 + 1.0)) /* 12.0 */
#define FM_PRECEDENCE     (2.0 + 3.0 * 4.0)           /* 14.0, not 20.0 */

/* ---- F. Mixed int + float (C usual arithmetic conversions) ----------- */
#define FM_MIX_INT_FLOAT  (1 + 0.5)        /* 1.5 - int promoted to double */
#define FM_MIX_DIV_INT    (1.0 / 2)        /* 0.5 - integer divisor promoted */

/* ---- G. Casts -------------------------------------------------------- */
#define FM_CAST_DOUBLE    ((double)42)              /* 42.0 */
#define FM_CAST_FLOAT     ((float)7)                /* 7.0 */
#define FM_CAST_FROM_INT_MACRO ((double)(100 + 1))  /* 101.0 - cast of expression */

/* ---- H. Magnitude ---------------------------------------------------- */
#define FM_TINY           1e-300          /* still representable as double */
#define FM_HUGE           1e300

/* ---- I. Macro chaining (one float macro builds on another) ----------- */
#define FM_BASE           2.0
#define FM_DERIVED        (FM_BASE * 5.0) /* 10.0 - other macro reference */

/* ---- J. Zero / negative zero ----------------------------------------- */
#define FM_ZERO           0.0
#define FM_NEG_ZERO       -0.0

/* ---- K. Negative tests (these MUST NOT register as doubles) ---------- */

/* (int)(3.14): __typeof__ recovers `int`, so this stays on the integer
 * path. Value: truncation toward zero -> 3. If the extractor accidentally
 * routed by "has-FloatingLiteral-in-init", this would land as 3.14 (and
 * the CFlat == 3 check below would fail). */
#define FM_INT_CAST_OF_FLOAT  ((int)(3.14))

/* Pure string - existing drop pattern, repeated here so the harness exercises
 * it in the float context (a mid-stream string must not poison neighbors). */
#define FM_STRING             "not-a-number"

/* Function-like macro adjacent to float macros - must not crash extraction or
 * leak into the object-like set. */
#define FM_FN(x)              ((x) * 1.5)

#endif /* FLOAT_MACROS_H */
