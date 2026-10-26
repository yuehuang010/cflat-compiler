# C++ call parity leftovers seen in fix box 2026-10-22 reviews (pre-existing on master)

Values follow the T418 encoding (value*1e7 + ctor/copy/move/ca/ma/dtor/live digits) or the probe's own
value/copies pair; every cell measured against clang++ -std=c++20 by the Sol reviewer of T416/T418/T420.

Wrong overload (copy ctor const-ness):
- A direct free call with a `const` class local selects `CV(CV&)` instead of `CV(const CV&)`: 107 vs clang 207.
- An incoming `const` by-value C++ class parameter is copy-constructed through the non-const copy ctor:
  n=107 vs clang 207.

Refusals clang accepts:
- A global as a `?:` leaf into a C++ call (`take(c ? default : g)`): refused, clang 701.
- `default_competing` (two overloads both viable for `default`): refused on master.
- A field of a by-value parameter as a `?:` leaf (`take(c ? default : w.tw)` inside `int pf(Wrap w, bool c)`):
  bind error "no viable conversion from 'unsigned long long *' to 'cpptw::Twin'".
- A bare ADL call to a C++ free function fails overload lookup.
- A non-template callee taking `Fn&&` refuses a CFlat callable.

Extra copies (values right, counts wrong):
- `?:` condition that is a bool field or global: 203/0 vs clang 203/2 copies.
- Member-template and template-only overload sets with a `?:` argument: one extra copy per arm.

Other:
- Ctor `S(A&... rest, int a, int b)` (non-final reference pack): 1111 vs clang 1011 (T416 keeps the
  conservative slot rule; see fix-issue-lessons digest T416).
- By-value class temp through the free wrapper moves once more than clang (1010020 vs 1000010) - also listed
  in p2/cpp-ternary-default-arm-unbound-callee-wrong-arm.md.
