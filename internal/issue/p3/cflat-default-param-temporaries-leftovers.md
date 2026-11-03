# CFlat default-parameter temporaries: leftovers after DEFCLS

DEFCLS (61b3e235, 2026-10-30 box 7) accepted C++-class-typed defaults on CFlat functions (expression, `default`, `{}`) via the
default-param wrapper. Found by its Opus reviews; shapes inline (probe folder was removed with the worktree), header = any traced C++
class `B` with a by-value ctor and dtor counter, `N` = native struct with a counting dtor.
- Destroy timing: the default temp is destroyed when the wrapper returns, not at the end of the caller's full expression
  (`wdefB(rv.mkB(1))`: clang `b1 b5 =1005 live=3 ~b5 ~b1`, cflat `b1 b5 ~b5 =1005 live=2 ~b1`). Counts and order vs other args are right.
- Ternary default `rv.B d = flag ? rv.B(5) : rv.B(6)` takes one extra move (`b5 mb5 ~b5 ~b5`; clang elides). Live count balanced.
- Native defaults (master behaviour, kept on purpose): `int f(N a, N d = N(5))` never destroys the default temp (leak, live stays up);
  `N fRd(N a, N d = N(5)) { return d; }` called explicitly as `fRd(mkN(1), N(5))` destroys `n5` twice (live -1). Native or unique temps
  created INSIDE a C++ class default (`rv.B d = rv.B(mkN(7).v)`) leak the same way.
- Non-empty brace default `rv.B d = {5}` on a C++ class is still refused.
Related: C++ functions with an OMITTED class-typed default landed as CDEFARG (c2c68587, caller-slot initializer); shapes whose
re-read default fails the declaration-identity check fall back to the forwarding wrapper (extra move, early destroy) by design.
