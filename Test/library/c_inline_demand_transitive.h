inline int c_inline_inner(int x) { return x + 5; }
static inline int c_inline_outer(int x) { return c_inline_inner(x) + 2; }
static inline long labs(long x) { return x < 0 ? -x : x; }
inline int c_inline_plain_only(int x) { return x * 7; }
static inline int c_inline_opt_view(void) {
#ifdef __OPTIMIZE__
    return 1;
#else
    return 0;
#endif
}
#ifdef __OPTIMIZE__
#define C_INLINE_OPT_MACRO 1
#else
#define C_INLINE_OPT_MACRO 0
#endif
extern inline __attribute__((gnu_inline)) int c_inline_gnu(int x) { return x + 3; }
static inline int c_inline_gnu_user(int x) { return c_inline_gnu(x) * 2; }
/* A weak definition reached only from a demanded body keeps its name: the strong one wins. */
__attribute__((weak)) int c_inline_hook(void) { return 0; }
static inline int c_inline_use_hook(void) { return c_inline_hook(); }
/* Header-only plain inline, no external definition in any TU. clang-cl emits a linkonce_odr
   comdat copy per TU (MSVC inline semantics), so taking its address links on Windows; clang on
   macOS/Linux leaves an undefined symbol at -O0 and -O2 (C99 6.7.4), so only the call is
   portable. */
inline int c_inline_hdr_only(int x) { return x + 1; }
static inline int c_inline_hdr_only_call(int x) { return c_inline_hdr_only(x) * 10; }
