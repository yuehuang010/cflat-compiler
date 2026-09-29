# C++ assignment result as a value: leftovers after B9

Summary: from the B9 review (2026-09-29). Probes and clang oracles are in scratch/repro_keep/b9rv/; `rt1.cb` is the P2.
- One extra copy but no leak: a parenthesized ternary declaration, and `C b = (arr[1] = 10)`. Master was the same or worse; for the array element it leaked.
- Pre-existing, unrelated, same on master:
  - `a = rv.mkR(4).get();` into a C++ class fails with "Call arguments (0)".
  - `(a -= 1);` with a free by-value operator and `(a += 1);` on a move-only class are refused as a discarded owning value.
  - Passing `mkR(5)` by value moves it instead of eliding.
Also carried over from the B9 report, out of scope: a global destination; a trivial class with a by-value `operator=(int)`; an unparenthesized brace-list element; native designated field init and a CFlat lambda returning a local, both of which copy more than clang.

From the B18 review (2026-09-29; scratch/repro_keep/b18rv/). B18 routes a parenthesized ternary through the ternary path only when an arm is an assignment. Other forms keep the call/convert path. Still open:
- A parenthesized ternary of prvalue-call arms, `T b = (f ? mk(1) : mk(2));`, gives 2 copies and 2 dtors; clang elides to 0/0.
- The bare spelling `W b = f ? 1 : 2;`, with scalar arms and a converting ctor `W(int)`, is refused; the parenthesized form works.
- A bare nested mixed ternary, `N b = f ? (g ? xa : mk(2)) : xb;`, is refused. The parenthesized form works, with one extra copy for `(f ? xa : (g ? xb : mk(2)))`.

(The paren-ternary P2 landed in 1955bd95, B18.)
