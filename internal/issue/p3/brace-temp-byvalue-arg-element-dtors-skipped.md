# A brace temp passed by value to a CFlat function never runs its element destructors

Found by the T312 Opus review (2026-10-16), pre-existing on master. Pure CFlat.
`struct E { string s; E() { ct++ } ~E() { dt++ } }; struct O { E[2] h; }; struct O2 { O[2] a; };`
- `takeO({ h = { {}, {} } })` (takeO(O o)): dt 0 after the call on master and T312; clang++ -std=c++20 2/2.
- nested `takeO2({ a = ... })`: master 8/0, T312 4/0, clang 4/4.
Heap audit shows 0 leaked blocks with literal strings (string payloads may hide a leak with heap strings - check).
Probe: scratch/repro_keep/wt_t312/rev_t312_opus_probe.sh.
Fix direction: the by-value brace argument temp must be destroyed after the call (or the callee's parameter destroys it) exactly once, for
element types with a user dtor; compare the named-local argument path, which runs them.
