# CFlat aliases of C++ templates: remaining type-use gaps

Round 2 covers non-constructor type uses of aliases to all-defaulted C++ class templates, scoped shadowing, and specific refusals for partially-defaulted aliases. The remaining gaps are:

- A C++ alias-template target is still not handled as a C++ alias template.
- A namespace-qualified use of an alias declared inside a CFlat namespace is still unsupported. Repro: `namespace ns { using G = cppt.dflt.Gate; }` followed by `sizeof(ns.G)` reports that `G` is not a member of `ns`. Unqualified `G` inside `ns` resolves.
- `function<long(cppt.dflt.Gate<>*)>` fails for both the alias and spelled template type with `cannot find the type`.
- `unique<G>` and the spelled `unique<cppt.dflt.Gate<>>` both fail while mapping to `std::unique_ptr`; the spelled form also fails, so this is separate from alias completion.
