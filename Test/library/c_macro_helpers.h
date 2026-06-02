#pragma once

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

/* Pointer-sentinel macro. Pass B's __typeof__ probe recovers the type as
   CB_HANDLE -> void*, so the macro registers as a void* global rather than
   an i64 integer constant. The cflat side compares it directly against a
   C-returned pointer of the same bit pattern (see c_get_invalid_handle in
   cinterop.c). */
typedef long long CB_LONG_PTR;
typedef void* CB_HANDLE;
#define CB_INVALID_HANDLE ((CB_HANDLE)(CB_LONG_PTR)-1)

/* Each of the following must still be silently rejected (dropped) by the
   translator - if any were accepted, its generated source would fail to compile,
   so their presence here exercises the drop path: */
#define CB_REJECT_CALL(x)   (some_func(x))      /* call into an UNKNOWN function */
#define CB_REJECT_STRING(x) (x ? "y" : "n")     /* string literal */
#define CB_REJECT_MEMBER(p) ((p)->field)        /* member access */
