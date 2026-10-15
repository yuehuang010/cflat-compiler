# Alias return of an owning call-result field dangles

Summary: `alias int r1() { return mk()->v; }` returns an alias into the receiver temp, and the function's return
path destroys that temp first. Callers read freed memory. Same on master and after T256 (found while probing T256;
the alias DECLARATION path is fixed, this is the return path).

Repro:
```
int gD = 0;
int gS = 0;
struct R { int v = 3; ~R() { gD = gD + 1; } };
unique<R> mk() { return unique<R>(new R()); }
alias int r1() { return mk()->v; }
extern int main() { for (int i = 0; i < 1000; i = i + 1) gS = gS + r1(); return 0; }
```
Observed: `--asan` heap-use-after-free READ in the caller; freed by `~unique` inside `r1`. Without asan the sum is
garbage on master (1003022880 instead of 3000).

Fix direction: a diagnostic ruling first (refuse an alias return of a temp-owned field, or keep the owner alive
for the caller). Accept-set first: alias returns of fields of named locals/params must keep compiling.
