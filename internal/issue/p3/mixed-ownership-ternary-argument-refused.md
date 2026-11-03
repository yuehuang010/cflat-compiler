# Mixed ownership ternary argument is refused

Summary: A valid conditional mixing a returned value and a C++ lvalue cannot be passed to a function.
Repro: `take(c ? rv.make2() : b)`.
Master vs expected: master refuses due to the mixed-ownership rule; clang accepts.
Found: T454 r1 opus review, 2026-10-24 box, PRE-EXISTING

MIX10: refusal is the ownership rule `RejectMixedOwnershipTernaryJoin` (MainListener_Declarations.cpp ~15943), pinned by Test/errors/err_ternary_mixed_ownership.cb case (d) (argument position `MixTake(h ? other : MixMake())`). The comment at MainListener_Expressions.cpp ~13862 also states that other sinks keep the mixed-ownership rejection. Needs a maintainer ruling. Not relaxed.
Repro reproduced on master: `t454_take(c ? t454_make_box() : o)` refused ('?:' arms handle ownership differently); clang accepts. Also refused: brace `rev.Val s{c ? default : v}` (C++ class by-value ctor). The free-function shape `t391_take(c ? default : v)` is accepted (matches clang).

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): STAYS REFUSED, working as intended. The 2026-09-03 ruling
"a mixed owning/borrow ?: join is a compile error, not a documented leak" covers every sink, argument position included; clang
accepts because C++ copies `b` implicitly, and CFlat never inserts an implicit copy of a non-trivial value (design north star).
Escape: make both arms owning (`c ? rv.make2() : b.copy()` / a C++ copy spelled as `rev.Val(b)`) or bind the join to a local
first. Test consequence: err_ternary_mixed_ownership.cb case (d) stands; no leg flips; the item closes without code.
