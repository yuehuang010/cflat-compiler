# Owned call temps in a multi-declarator declaration live until the end of the whole declaration

Found by the T284 Sol review (2026-10-15). Pre-existing on master for EVERY declaration (plain and, after T284, for-init).

## Repro
```cflat
int dtors = 0;
struct R { int v = 3; ~R() { dtors = dtors + 1; } };
unique<R> mk() { return unique<R>(new R()); }
extern int main() { int x = mk()->v, y = dtors; return y; }
```
CFlat returns 0; clang++ -std=c++20 (equivalent C++) returns 1: each init-declarator is its own full-expression, so the
`mk()` temp is destroyed before `y`'s initializer runs. Same for `for (int x = mk()->v, y = dtors; ...)`.
Repros: scratch/repro_keep/wt_t284/ (finding_full_expression.cb/.cpp, plain_decl.cb).

## Fix direction
Mark/flush owned temps per initializer in the ParseDeclaration declarator loop (MainListener_Declarations.cpp ~7021),
after ownership adoption / alias settling - keep bound owners and aliases alive. Observable only through side effects of
a destructor read by a later declarator, so low severity.
