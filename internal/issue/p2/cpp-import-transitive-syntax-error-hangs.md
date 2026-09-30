# A syntax error in a transitively included C++ header hangs the compile

Found by the H1 review (perf timebox 2026-09-29). This is on master; H1 did not cause it.

## Repro
A C++ import whose header includes another header containing `namespace tt { int broken( }`.
Clang's incremental error recovery spins (a diagnostic loop in Parser::ParseTopLevelStmtDecl). Master was still running when the 90 s timeout killed it; a branch build ran for more than 9 minutes before it was killed. Scripts and inputs are kept in scratch/repro_keep/h1/ (h1r1_bad.sh, h1r1_bad/inc/t3.h; put the broken line back into t3.h).

## Expected
The compile stops with the relayed clang syntax error ("clang: " prefix) at the import, like a syntax error in the primary header, and does not hang.

## Direction
Find why the Interpreter path loops where a plain clang -fsyntax-only stops: error limit / fatal-error handling on the incremental parser, or a retry loop in CxxIncrementalGroup that re-parses on failure. Set an error limit, or treat a failed chunk 0 as fatal.
