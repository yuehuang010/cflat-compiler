# Passing a lowered (C++-field) CFlat struct by value moves it, but a later read is not reported as use of a moved variable

Found by Q5 (2026-10-01): `pass(b)` with a CFlat struct holding a C++ class field moves `b`; a later `b.id` read is silently accepted, while the plain-struct control reports "use of moved variable". Diagnostic gap only (no corruption observed). Fix direction: the by-value parameter path for lowered structs (V1b) must mark the source moved for the CFlat moved-from analysis like the plain path.
