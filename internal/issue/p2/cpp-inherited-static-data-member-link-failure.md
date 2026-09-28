# Inherited C++ static DATA member: --check accepts, -o fails to link

Found 2026-09-27 by the fix/inherited-static review (pre-existing on master).
`DataBase { static const int K; }` defined out of line in the header's library, `Derived : DataBase`,
CFlat `Derived.K` -> `--check` passes, `-o` fails at link: undefined `rev::DataBase::K`. clang++ accepts
and links. Static member FUNCTIONS inherited from a base work (fix/inherited-static).
Repro: scratch/rev_ih/rev_ih_data.cb (+ rev_ih_edges.h), main checkout.
Fix direction: the inherited static data member is bound under the derived record's name or without the
defining-TU symbol; bind it to the base's mangled symbol (as clang does), or emit it in the companion when
it is inline/constexpr. Link failure with a clean --check = p2 (compile-time acceptance, link-time failure).
