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

#endif /* MATHLIB_H */
