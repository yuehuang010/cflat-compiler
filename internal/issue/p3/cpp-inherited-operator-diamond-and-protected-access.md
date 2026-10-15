# Inherited C++ members: non-virtual diamond ambiguity and protected base operator access

Filed 2026-10-14 from the T221 review 2 (PRE-EXISTING on master; probes in the T221 worktree
scratch/rev_t221_opus_probe2_full.sh / probe2.sh).

1. Non-virtual diamond `NDD : NX1, NX2 { using NX1::f; using NX1::operator-; }` - clang refuses
   `d.f(2)` / `d - 2` as an ambiguous conversion to the shared base; CFlat accepts both (`d.f` on
   master, `d - 2` since T221).
2. A protected base `operator-` with no using-declaration is callable from outside the class; clang
   refuses (protected). Named protected methods are already refused.
3. Virtual diamond is refused "virtual inheritance not supported" (known limitation, not a bug).
