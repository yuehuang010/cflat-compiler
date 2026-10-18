# `move` of a call result skips the C++ member move at return, call argument and subfield sites

Summary: T330 makes `H d = move mk();` (and array element / named field / tag-form field) move-construct the
C++ member of a lowered call temp like clang. Three other `move`-of-call-temp sites still bit-copy the member
and skip its move constructor, and a conditional field init leaks one destructor. Found by the T330 round-1
review (2026-10-16), identical on master and the T330 branch.

Setup: `import cpp "library/cpp_t253_brace_move.hpp"; struct H { t253.Counted m; int k = 1; };`
`struct W { H h; int z = 0; }; H mk() { H r = default; r.m.value = 5; return r; }` (move ctor adds 1000).
Results are value,ctor,copy,move,dtor:
- argument: `int arg(H h) { return h.m.value; }` ... `arg(move mk())` -> 5,1,0,0,1; clang 1005,2,0,1,2.
- return: `H ret() { return move mk(); }` ... `H d = ret();` -> 5,1,0,0,1; clang 1005,2,0,1,2.
- subfield: `W makew() { W w={h=mk()}; return w; }` ... `H d = move makew().h;` -> 5,1,0,0,2; clang 1005,2,0,1,2.
- conditional field: `W d = {h=move (true ? mk() : other())};` -> 1005,2,0,1,1 (one dtor short); clang ...,2.

Fix direction: route these sites through the T330 shared move-from-call-temp slot helper (return and call
argument lowering, member access on a call result), and arm the conditional arm's temp cleanup.
