# clang-tidy: performance backlog (needless copies and string building)

Summary: the clang-tidy baseline 2026-11-07 reported ~385 performance findings in cflat's own code.
None is a correctness bug; together they are avoidable copies on hot compile paths.

| Check | Count | Typical case |
|---|---|---|
| performance-inefficient-string-concatenation | 232 | `a + b + c` building a std::string in a loop |

Fix direction: string concatenation only where profiling shows the function hot; otherwise
consider turning that check off. Gate: scratch/libs_perf.sh shows no
compile-time regression, suites green.

Landed 2026-11-07 (fix box 12): unnecessary-value-param, unnecessary-copy-initialization and
for-range-copy - 52 of their 72 findings fixed. The 20 left are deliberate: std::exception_ptr and
sink params, copies kept because the source is mutated or invalidated while the copy is live
(push_back / erase on the same container, SaveAndRestore aliases), and simdjson value handles.
no-automatic-move (35 `const` drops on returned locals) and inefficient-vector-operation (43
reserve calls) landed the same day.
Open: inefficient-string-concatenation only.
