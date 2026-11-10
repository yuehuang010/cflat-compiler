# C++ by-value parameters take an extra move; explicit-move parameter returns over-transfer

Filed 2026-10-14 from the T177 and T178 reviews. Item 1 was resolved in T446 under the 2026-10-21 C++20 parity ruling.

## 2. Same-type return does not apply NRVO to a by-value C++ parameter (T446, still open)
The `cm_param`/`dst_param` extra prvalue move and the `cm_generic` template-selection mismatch were fixed in T446.
The remaining 44606 case returns a same-type local C++ value: cflat still reports `srcMove=1, dtor=2`, while clang's
NRVO reports `srcMove=0, dtor=1`. NRVO is out of scope for the current round and remains open here.

Repros: scratch/repro_keep/t177_rv1/ (probe.sh, rv1.hpp, c/ with clang twins).

Regression coverage: Test/test_cpp_interop_bridge.cb legs 44602, 44607 and 44608 now pin the landed by-value call-result elision.
Leg 44606 keeps the current `srcMove=1, dtor=2` counts until same-type return NRVO is addressed.
## 3. `return move f;` of a by-value C++ param (T178 Opus review, PRE-EXISTING)
`tk.Trk m(tk.Trk f) { return move f; }` called with `tk.Trk(5)`: cflat ctor/copy/move/dtor 2,1,2,5 vs clang 2,0,1,3 (same on
pre-T173). Same family as item 2 (by-value param materialization). Repro scratch/repro_keep/t178_rv1/.
