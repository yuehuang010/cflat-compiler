# alias declared in a for-loop init frees its owner too early

Summary: `for (alias int x = mk()->v; ...) { ... x ... }` reads freed memory. The for-init is a
ForDeclarationContext, which the alias-declaration check (InAliasDeclaration) does not recognise, so the
receiver temp is freed at the init statement's end. Same on master and after T256 (garbage read; ASAN
heap-use-after-free).

Repro:
```
int gD = 0;
int gS = 0;
struct R { int v = 3; ~R() { gD = gD + 1; } };
unique<R> mk() { return unique<R>(new R()); }
void f() { for (alias int x = mk()->v; gD < 1; gD = gD + 0) { gS = gS + x; break; } }
extern int main() { f(); return 0; }
```

Fix direction: recognise ForDeclarationContext in InAliasDeclaration. Do it together with
p2/for-init-owning-local-break-double-destroy, or a `break` out of the loop double-destroys the extended temp.
