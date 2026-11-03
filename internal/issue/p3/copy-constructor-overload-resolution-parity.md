# Copy constructor overload resolution differs from clang

Summary: Const copy assignment chooses a different overload and return initialization adds a copy.
Repro: `y = x` with const `x` and `operator=(CV&)` / `operator=(const CV&)`; `rv.CV y = f(rv.CV(7))`.
Master vs expected: master returns 17 vs clang 27 and makes one extra copy (107 vs 7).
Found: T461 r1 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX3): const copy assignment FIXED (rhsConstRef on copy-assign Structors; a const lvalue
source binds operator=(const CV&), non-const binds operator=(CV&); MainListener_Expressions.cpp).
Residual (open): `CV z = f(CV(7))` where f returns a fresh CV still copies. Cause: the declaration
arms the sret slot only when the initializer text has exactly one '(' (MainListener_Declarations.cpp
~8179, `std::count(pfText ..., '(') == 1`); with a nested call the call builds into cxx.rettemp and
the copy ctor runs. A parse-tree gate (`pf->LeftParen().size() == 1`) removed the copy but regressed
test_cpp_interop_bridge (exit 58, cause not yet identified: an inner same-type call consumes the armed
slot) and was reverted. Also open: a call-result argument to a by-value C++ parameter
(`make(mk(7))`, `pick2(CV(3), mk(4))`) copies once per such argument.

## Root cause (MIX3 round 2, I2): defaulted const copy-assign is never a candidate

`struct DF { DF& operator=(const DF&) = default; DF& operator=(DF& o) {...} };` with a const
source: clang binds the defaulted `const DF&` (trivial, byte copy); CFlat binds `DF&` (dfc=47,
clang 7). Chain: CClangExtract.cpp ~8403 (`!(w.md->isDefaulted() && w.md->isTrivial())`) gives the
trivial defaulted member no linkage name, so ~8410 drops it from copyAssignOverloads. The record
flag hasTrivialCopyAssign (CClangExtract.cpp ~95, first copy-assign only) is then not a usable
const-ref fallback either. Fix direction: export a per-record `hasTrivialConstCopyAssign` (extract,
StateAndImports save/load, CInterop record copy) and byte-copy in lvalueReceiverAssignment when
the const-source overload is absent. Probe: scratch/mix3_rev/cpt.cb with cp.hpp (dfc: clang 7).
