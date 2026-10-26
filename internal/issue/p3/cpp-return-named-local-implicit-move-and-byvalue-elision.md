# C++ by-value parameters take an extra move; explicit-move parameter returns over-transfer

Filed 2026-10-14 from the T177 and T178 reviews. Item 1 was resolved in T446 under the 2026-10-21 C++20 parity ruling.

## 2. By-value C++ param takes an extra move instead of eliding the prvalue (no ruling needed)
`dst_param`, `cm_param`: one extra move ctor + dtor vs clang (counts only exceed clang). When the move-like ctor is a template
the selected ctor differs: `cm_generic` 407 vs clang 207 (master refused).

Repros: scratch/repro_keep/t177_rv1/ (probe.sh, rv1.hpp, c/ with clang twins).

Pinned today (T446): Test/test_cpp_interop_bridge.cb legs 44602 (cm_param: target copy=1, dtor=2; clang 0/1) and 44606 (same-type return: srcMove=1, dtor=2; clang NRVO 0/1) hard-code the current counts - update them when this item lands.
## 3. `return move f;` of a by-value C++ param (T178 Opus review, PRE-EXISTING)
`tk.Trk m(tk.Trk f) { return move f; }` called with `tk.Trk(5)`: cflat ctor/copy/move/dtor 2,1,2,5 vs clang 2,0,1,3 (same on
pre-T173). Same family as item 2 (by-value param materialization). Repro scratch/repro_keep/t178_rv1/.
