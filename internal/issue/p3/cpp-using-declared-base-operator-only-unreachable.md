# A base operator brought in only by a using-declaration is not reachable for a by-value argument

Summary: `struct B { long operator-(long) const; }; struct D : B { using B::operator-; };` then
`D e; e - 2` reports "no overload of 'operator-'" with one candidate (master and T127 alike);
clang++ -std=c++20 calls B::operator-(long). When D also declares its own `operator-` overload (the
MSVC std::_Vector_iterator shape) the base overload is found, so the std case is unaffected.

Found: T127 (2026-10-11), pre-existing.

Fix direction: when a using-declaration is the derived class's only declaration of the name, the
lookup must still continue into the named base (the using shadow carries the base member); check
CollectCxxImplicitArgumentCandidates and the direct member registration for using shadows.
