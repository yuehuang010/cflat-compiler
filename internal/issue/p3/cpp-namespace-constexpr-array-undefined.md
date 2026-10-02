# C++ namespace-scope constexpr array: CFlat use reports only "Undefined variable"

Summary: a namespace-scope `constexpr int ka[3] = {1, 2, 3};` in an `import cpp` header is not
bound. A CFlat use gives a plain "Undefined variable ka." The only reason is the -v line
"skipping global 'ka': array type 'int[3]' is not bindable" from MapRawGlobal
(LLVMBackend_CInterop.cpp), which drops every global whose C type contains '['. This is an old
limitation, and the use-site error does not explain it. Found during Q6 (V14 follow-up, p3 per-group statics).

Repro:
```
// k.hpp
constexpr int ka[3] = {1, 2, 3};
// probe.cb
import cpp "k.hpp";
extern int main() { return ka[1] - 2; }
```
Expected: ka binds (read-only array, like a C++ use), or the use is refused with a reason that
names the array type. Today: "Undefined variable ka."; the reason is only visible with -v.

Fix direction: bind namespace-scope constant arrays as per-group internal storage (the
__cflat_sv_ path already emits them for inline bodies). Otherwise, record a binding refusal so the
use site explains it.
