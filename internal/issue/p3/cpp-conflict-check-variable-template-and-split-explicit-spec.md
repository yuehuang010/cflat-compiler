# C++ namespace conflict check: variable templates missing; explicit specialization in a second header

Found by T94 review 1 (2026-10-10), both pre-existing on master. Probes:
scratch/repro_keep/t94/rev_t94/ (run scratch/repro_keep/t94/rev_t94_probe.sh; case | master | fix | clang).

1. c01_cls_vs_vartmpl: `struct V` in a.h vs `template<class T> int V` in b.h - clang refuses
   ("redefinition of 'V' as different kind of symbol"), cflat accepts. CheckCxxNamespaceConflicts
   has no identity tag for variable templates.
2. c2rev p1 (scratch/repro_keep/c2/c2rev/): explicit specialization `S<long>` declared in b.h of a
   primary template from a.h -> "no imported C++ header declares 'n::S'" on both exes; clang accepts.

Also note for whoever fixes item 1: [cpp] struct generated identities collide when two registered
names encode alike (`ns_.Self` / `ns._Self` -> ns___Self); T94 falls back to the "__" split then,
and using the second one through weak_ptr is refused (b8_ambig_lower). CppStructCxxName could be made
injective to close that.
