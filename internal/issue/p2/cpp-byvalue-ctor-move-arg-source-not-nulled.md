# `move f` into a by-value C++ ctor parameter does not null `f`; misleading ctor-failure prefix

Found 2026-10-14 by the T248 review (repros scratch/repro_keep/t248_rev/cases/, header h.hpp). Ruled a BUG 2026-10-15:
the explicit-move-nulls-source ruling already covers it.

1. `function<int(int)> f = q.plus; q.useByVal(q.ByVal(move f)); q.out(f == nullptr ? 1 : 0);` with C++ `ByVal(Fn g)`
   prints 0: `f` is not nulled after `move f` bound to a by-value C++ ctor parameter (c12_move_local.cb). Must print 1,
   as for a by-value function call argument.
2. A failed ctor bind inside an argument, `const function<int(int)> l; q.useRef(q.RefOnly(l))` with `RefOnly(Fn&)`, leads
   its error with "'useRef' is not a member of namespace 'q'" before the real clang reason (c13_constparam_ref.cb,
   c14_constlocal_ref.cb, logs out/c13*.log, out/c14*.log). The clang reason must come first; the outer-lookup line is
   noise.
