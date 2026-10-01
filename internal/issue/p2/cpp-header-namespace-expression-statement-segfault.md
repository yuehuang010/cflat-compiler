# C++ header with an expression statement inside a namespace crashes clang Sema

Found 2026-09-30 by W3 round 3 (pre-existing on master; W3 fixed the hang class, not this crash).

## Repro
scratch/repro_keep/w3/w3r/p_h_stmt.cb + h_stmt.h:
```cpp
// h_stmt.h
#pragma once
namespace n { 1 + ; }
```
`import cpp "h_stmt.h"; int main() { return 0; }` -> SIGSEGV in `Sema::HandleDeclarator`.
clang++ reports a parse error.

## Root cause (partial)
With clang IncrementalExtensions on, a statement inside a namespace goes through
`ParseTopLevelStmtDecl`; the crash happens before any parse diagnostic is emitted, so W3's
diagnostic-triggered switch-off (CxxIncrementalGroup.cpp header parse) never fires.

## Fix direction
Catch the statement-in-namespace path before Sema (e.g. disable IncrementalExtensions for the
header parse without losing std::function binding - W3 round 3 found a whole-parse toggle loses
it downstream in CFlat import binding, cause untraced), or diagnose it up front.
