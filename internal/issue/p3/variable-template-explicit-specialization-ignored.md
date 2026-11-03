# Non-constexpr C++ variable template values are refused

Summary: a NON-const variable template (`template<class T> inline int R = 4;`, specialization
`template<> inline int R<long> = 9;`) cannot be read as a value. `ns.R<long>` and even the primary
`ns.R<int>` are refused with "does not name a C++ class type in the imported headers". clang: 4 and 9.
Refusal, not a wrong value. Constexpr explicit, partial and same-header specializations are fixed
(CPPFIX: a specialization in a later import is now part of the template's request group).
Repro: `pa.h` declares `namespace ns { template<class T> inline int R = 4; }`; `pb.h` includes it and
specializes `R<long> = 9`; CFlat `int r = ns.R<long>;` with `import cpp "pb.h";`.
Root cause: HarvestCxxNamespaceVar skips variable templates ("bound per specialization on use"); the
decltype request (CClangExtract.cpp, VisitVarDecl request path) returns early for a variable that is
neither const nor constexpr, so nothing is published.
Fix direction: bind a non-const variable template specialization as a global object (its clang
mangled symbol, per specialization), like a non-const namespace variable. Needs a design check on
storage/linkage per import group first.
Found: T436 r1 opus review, 2026-10-24 box, PRE-EXISTING

Residue 2 (indirect read through a constexpr variable template): a variable template whose
initializer reads another variable template with a specialization in a later import gets the
primary value. Repro: va.hpp `namespace ns { template<class T> inline constexpr int V = 3;
template<class T> inline constexpr int AV = V<T> + 100; }`; vb.hpp `#include "va.hpp"` +
`template<> inline constexpr int V<long> = 5;`; CFlat `import cpp "va.hpp"; import cpp "vb.hpp";`
`int x = ns.AV<long>;` gives 103, clang 105 (AV<int> 103 both). Root cause: the request group for
AV is the group that publishes it (va.hpp); no group records that AV's pattern depends on V's
specialization, so vb.hpp never joins the request. Fix needs extraction of the pattern's
variable-template references plus a serialized dependency list on the import group (cache version
and round-trip). A namespace-wide dependency widening would change every class template request
too, so it is not taken. Found: CPPFIX r2 review, 2026-10-30.
