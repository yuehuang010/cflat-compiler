# Brace element that is a call returning a C++ class: destroyed more times than constructed

Found by T97 review 1 (2026-10-10), pre-existing on master. A CFlat struct field default
`H h = { d = mk() };` (mk returns a C++ class by value) runs 2 constructions and 3 destructor calls;
clang runs 1 construction and 1 destruction. An unbalanced destructor is a double destroy (UB for a
class owning a resource). Repro: scratch/repro_keep/t97/rev_t97/call.cb (counting header in the same folder).

Fix direction: the call's sret temporary should be the field slot (C++17 elision) or be moved and
destroyed exactly once; find which owned-temp ledger entry is flushed twice
(FlushOwnedTempsSince / RegisterOwnedStructTemp around ParseFieldDefaultBraceInitializer).
