# `move` of a call result skips the C++ member move: parenthesized and call-result-join spellings

Summary: T483 (2026-10-24) made `arg(move mk())`, `return move mk()`, `H d = move makew().h` and the conditional field
init move-construct the C++ member of a lowered call temp like clang. These spellings still bit-copy the member and skip its
move constructor (master same, found by the T483 Sol review).

Setup: `import cpp "library/cpp_t253_brace_move.hpp"; struct H { t253.Counted m; int k = 1; };`
`H mk() { H r = default; r.m.value = 5; return r; }`, `int arg(H h) { return h.m.value; }` (move ctor adds 1000).
Results value,ctor,copy,move,dtor:
- parentheses: `arg(move (mk()))`, `arg((move mk()))`, `return (move mk())` -> 5,1,0,0,1; clang 1005,2,0,1,2.
  The argument detector (MainListener_Expressions.cpp ~601) inspects only the immediate postfix and TopLevelMoveExpression
  (MainListener_Statements.cpp ~1338) stops at parentheses. Fix: unwrap parens structurally in both paths.
- call-result joins: `arg(true ? move mk() : move other())` -> 5,1,0,0,2 and `arg(move (true ? mk() : other()))` ->
  5,1,0,0,1; clang 1005,2,0,1,2. IsDirectMoveCall (LLVMBackend_Overloads.cpp ~8471) excludes joins; unlike the named T249
  join, a call-result join never performed the member move. Fix: transfer at the join or the outer move site without a
  second move for named joins.
