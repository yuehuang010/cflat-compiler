# `move f` into a by-value C++ ctor parameter does not null `f`; misleading ctor-failure prefix

Found 2026-10-14 by the T248 review (repros scratch/repro_keep/t248_rev/cases/, header h.hpp). Ruled a BUG 2026-10-15:
the explicit-move-nulls-source ruling already covers it.

1. `function<int(int)> f = q.plus; q.useByVal(q.ByVal(move f)); q.out(f == nullptr ? 1 : 0);` with C++ `ByVal(Fn g)`
   prints 0: `f` is not nulled after `move f` bound to a by-value C++ ctor parameter (c12_move_local.cb). Must print 1,
   as for a by-value function call argument.

PREMISE CORRECTED 2026-10-15 (T270, verified by the main session on master 8bd415044): `move f` of a thin
`function<int(int)>` value nulls the source NOWHERE today - not into a by-value CFlat call argument (`take(move f)`),
not into a local (`function<int(int)> k2 = move k;`); a `move` param read is refused "use of moved variable" instead.
So the by-value C++ ctor case is one instance of a language-wide gap, not a C++-path bug. NEEDS RULING: should `move`
of a function<> value null the source everywhere (explicit-move-nulls-source applied to function values)? If yes, fix it
once for all sinks (call arg, local, field, C++ ctor/call). T270's attempt at the C++ path alone crashed test_cpp_interop
(T56 legs) and was reverted.
Item 2 (nested ctor bind error prefix) landed in T270.
