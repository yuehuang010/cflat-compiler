# Interface return call-borrow: what remains after T522

T522 (ruling 2026-10-25, Rust lifetime elision) closed every dangle in the former p2
`iface-return-dangle-via-calls-aggregates-and-rebinds.md`. Legs: `Test/errors/err_iface_return_call_borrow.cb`
(refusals), `testInterfaceReturnCallBorrowAcceptSet` in `Test/test_interface.cb` (accepted controls).
Callee summaries are definition-order independent (calls to bodies emitted later are decided at
module end, compositionally) and recursive groups are summarized to a fixed point.

## Ruling-allowed refusal

- Variable-index overwrite loop `s[1] = alias; for (j) s[j] = &g; return s[1];`: a may-write with no
  kill. Master refuses it too; the ruling lets it stay refused.

## Pre-existing, separate mechanism

- `T* get(T* p) { T x; p = &x; return p; }` returns its own frame through a reassigned parameter
  (repro `scratch/rev_t522_sol_cases/param_rebind.cb`, stack-use-after-return on master too). This
  needs the callee's own pointer-return check, not the call-borrow rule.
- A pointer hidden behind an explicit cast (`h.c = (char*)&x;` in a `char*` field, then cast back and returned as an interface) is outside the tracked model: explicit casts erase the type that type narrowing relies on (repro `scratch/rev_t522r4_sol_cases/erased_char_local.cb`; dangles on master too).
- A ternary of two pointer locals returned as an interface (`return k==0 ? a : b;`) dangles on master
  and after T522; the same values returned without the ternary are refused (T522 review 8).
- Loop-carried closure copy whose last value is a global (`for (...) f = pick ? fl : fg;` then return
  through `f`) is refused: the static analysis cannot know which copy was last (by design, T522 review 7).
- Over-strict, not unsafe: a callee returning a field of a by-value struct parameter is not narrowed to
  that parameter, so `IN take2(N* q, BigH b) { return b.h; }` called as `take2(&loc, b)` and a method
  `m.holdB(b)` on a stack-local receiver are refused even when the field holds `&g` (T522 r12/review 10).
