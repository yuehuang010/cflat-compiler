# break out of a for-loop with an owning init local destroys it twice

Summary: `for (unique<R> u = mk(); n < 3; n = n + 1) { break; }` destroys `u` twice: once on the break path, and
again in the loop's exit block (both drop the for-init scope). Crash (exit 133) on master. Found while probing
T256; the alias variant of the same shape is p3/alias-for-init-temp-freed-early.

Repro:
```
int gD = 0;
int gS = 0;
struct R { int v = 3; ~R() { gD = gD + 1; } };
unique<R> mk() { return unique<R>(new R()); }
void f() { int n = 0; for (unique<R> u = mk(); n < 3; n = n + 1) { gS = gS + 1; break; } }
extern int main() { f(); return 0; }
```
IR (master, `--out-lli`): two consecutive `call @~unique(ptr %u)` in `f`, one before the break branch and one in
the exit block. Without a break the loop destroys `u` once (dtors 1).

Fix direction: the break cleanup must stop at the loop's own init scope when the exit block drops it (or the
exit block must skip it on the break edge).
