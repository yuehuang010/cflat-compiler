# C function returning a struct pointer: `auto` local deduces void*

Filed 2026-10-19 from the c10 (const plan item 10) implementer report. PRE-EXISTING on master 7292feeeb.

Repro (scratch/c10_sp/, gitignored):
```c
// h.h
struct PS { int x; int y; };
struct PS* m_ps(void);
```
```cflat
import "h.h";
import "c.c";   // defines m_ps returning &g
extern int main()
{
    auto p = m_ps();
    return p.y == 7 ? 0 : 1;   // error: Undefined variable y.
}
```
`PS* p = m_ps();` compiles. clang: fine. Per the implementer, diagnostics also spell the result type
`(const void)*` when the C result is `const struct PS*`, so the C result mapping loses the record pointee.

Fix direction: the C auto-extern result mapping (MapRawSig result type, LLVMBackend_CInterop.cpp near the
result ConstMask set by c10) maps `struct PS*` to void*; map it to the bound record pointer like params do.
Acceptance: the repro returns 0; `auto p` on a `const struct PS*` result is `(const PS)*` (write `p.y = 1` refused).
