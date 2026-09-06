/* Accept-side fixture for the conflicting-declaration check (Test/test_c_interop.cb Section B).

   Every prototype here AGREES exactly with a declaration that is already bound when this header
   is imported, so the repeat-declaration check must treat each one as a silent no-op. This is
   the common case, not a corner: `import "windows.h"` re-declares core names such as Sleep and
   WriteFile at identical types, and it has to keep compiling.

   The names are deliberately NOT libc builtins - clang rewrites a redeclaration of a builtin
   (abs, toupper, ...) back to the builtin signature before the extractor ever sees it, so such
   a prototype could not prove it reached the check. These do reach it: the same names at a
   DIFFERING type are rejected (Test/errors/err_extern_collides_with_core.cb uses putn). */

void putn(const char* buf, int len);   /* agrees with core/cruntime.cb's putn */
int  printf(const char* fmt, ...);     /* agrees with core's VARIADIC printf */
int  c_add(int a, int b);              /* agrees with the .c auto-extern from cinterop.c */
