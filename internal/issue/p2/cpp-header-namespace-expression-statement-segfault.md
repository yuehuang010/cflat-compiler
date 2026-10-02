# C++ header: some expression statements at namespace scope still crash clang Sema

V15 (52a77168, 2026-10-01) fixed the common forms: `namespace n { 1 + ; }`, assignments, calls,
literals (15 of 17 probes in scratch/repro_keep/v15/rv15/m give a clean "clang:" error; master crashed).
Mechanism: a token watcher in cflat/CxxIncrementalGroup.cpp (`expressionStart` list + identifier
look-ahead) switches clang's IncrementalExtensions off for that one statement, never inside system
headers, restored by an RAII guard.

## Still crashing (SIGSEGV rc 139 with V15; master crashes or hangs) - probes scratch/repro_keep/v15/rv15c/n*.h
Each one is wrapped as `namespace n { ... }`:
- `n::x = 5;` (n01), `::n::x = 1;` (n12): an identifier followed by `::`, or a leading `::`
- `*p = 5;` (n02), `&x;` (n06)
- `sizeof(x);` (n04), `[]{}();` (n05), `static_cast<int>(x);` (n10)
- `#define S x = 5;` then `S` (n03): the identifier comes from a macro expansion
- a header ending in `int x; x` (n17): the look-ahead hits end of input

Also out of scope: statements at global (file) scope (m09, m17).

## Fix direction - and the traps
- The reviewer's suggestion is to switch the extension off at EVERY statement start in a non-system
  namespace body. CAUTION: V15 r2 did that at declaration starts and clang looped forever in error
  recovery on `namespace tt { int broken( }` (Test/library/cpp_transitive_syntax_inner.h).
  Any widening must keep err_cpp_transitive_header_syntax_error.cb fast, with test.sh Elapsed normal.
- Switching the extension off for a WHOLE header, system headers included, loses std::function
  binding (W3 trap, cause untraced).
- Alternative: guard the crash site in Sema, or detect ParseTopLevelStmtDecl in a namespace context and
  diagnose there instead of predicting it from tokens.
- After any change, clear `cheaders`: entries a buggy round wrote at the same cache version show up as
  false regressions.
