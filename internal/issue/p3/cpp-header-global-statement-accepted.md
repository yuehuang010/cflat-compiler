# C++ header: expression statements at global scope are silently accepted

Summary: a header containing `int gx; gx + 1;` or `int gy; gy = 3;` at global (not namespace) scope imports and compiles with rc 0. `clang++ -fsyntax-only` rejects both.

Cause: clang's incremental top-level-statement extension parses them as statements. V15 (2026-10-01) switches the extension off only inside namespaces, because switching it off at global scope collides with the end-of-main-file chunking (PPLexerChange.cpp:534).

Probes: scratch/repro_keep/v15/rv15/m/m09.h, m17.h.

Fix direction: report a top-level statement that comes from a header file (not CFlat's own chunk text) as a clang-style error, for example by checking the TopLevelStmtDecl source location against the header file IDs.
