# string.data_mut() on a by-value string parameter leaks the copy

Summary: `data_mut(string* self)` (core/string.cb) copies a BORROWED string into an owned buffer and stores it
back through `self`. On a local or a field the new owned buffer is freed by the usual scope-exit / field teardown.
On a by-value `string` parameter it is not: the parameter is a borrow, the callee's scope exit frees nothing, and the
write through `&param` is invisible to the ownership analysis (a plain `s = x.copy();` on the parameter IS tracked
and freed).

Repro (leaks --atExit: 100 leaks, 1600 bytes; 0 with the call removed):

    int use(string s) { char* p = s.data_mut(); return (int)p[0]; }
    extern int main() { int i = 0; int acc = 0; while (i < 100) { acc = acc + use("abc"); i++; } return 0; }

Root cause: the parameter's ownership state changes inside a callee through a pointer; the caller-side analysis
that marks a reassigned parameter owning only sees direct assignment.

Fix direction: either treat `data_mut()` on a by-value string parameter as a reassignment of that parameter (free at
scope exit when the owned bit was set by the callee), or refuse it with a hint to copy into a local first. Documented
for now in doc/LANGUAGE.md and the data_mut() comment; core/network/tls.cb copies into a local before calling it.
